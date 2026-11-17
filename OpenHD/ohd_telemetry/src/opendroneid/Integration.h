// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include <optional>
#include <vector>
#include "ConfigJson.h"
#include "openhd_settings_imp.h"

namespace openhd::odid {
// Lifetime gate captured by value in endpoint callbacks. Closing prevents new
// entries and waits for active callbacks without holding endpoint/worker locks.
class CallbackGate {
 public:
  template <typename F> void run(F&& f) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) return;
      ++active_;
    }
    struct Exit {
      CallbackGate& gate;
      ~Exit() {
        std::lock_guard<std::mutex> lock(gate.mutex_);
        if (--gate.active_ == 0) gate.cv_.notify_all();
      }
    } exit{*this};
    f();
  }
  void close() {
    std::unique_lock<std::mutex> lock(mutex_);
    closed_ = true;
    cv_.wait(lock, [this] { return active_ == 0; });
  }
 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool closed_ = false;
  size_t active_ = 0;
};

class Integration : public std::enable_shared_from_this<Integration> {
 public:
  Integration(std::string path, int fc_system);
  std::vector<Setting> settings();
  void set_fc_system(int system);
  void ingest(const mavlink_message_t& m) noexcept { manager_.ingest(m); }
  std::optional<mavlink_message_t> poll_status(Time now);
  std::string debug() const;
  void stop() { manager_.stop(); }
 private:
  bool change(int Config::*field, int value);
  bool change(std::string Config::*field, std::string value);
  bool save(const Config& next);
  void apply();
  mutable std::mutex mutex_;
  std::string path_;
  Config config_;
  int fc_system_;
  bool file_valid_ = true;
  Time last_status_{};
  Manager manager_;
};
}  // namespace openhd::odid
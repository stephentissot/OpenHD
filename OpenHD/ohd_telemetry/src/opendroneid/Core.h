// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <openhd/mavlink.h>
#include <opendroneid.h>

namespace openhd::odid {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;

// Radio support is separately opt-in. Requests remain distinct from observed
// BlueZ registration state, including in dry run; no RF reception is inferred.
struct Config {
  int enable = 0;
  int control = 0;  // 0 LOCAL, 1 FC_CUSTOM (local ArduPilot extension)
  int ble = 0;
  int nan = 0;
  int beacon = 0;
  int dry_run = 1;
  int fc_component = MAV_COMP_ID_AUTOPILOT1;
  std::string bt_device;
  std::string wifi_interface;
  bool valid() const;
};

struct Report {
  bool enabled = false;
  uint8_t requested = 0;
  uint8_t available = 0;
  uint8_t effective = 0;
  bool data_ready = false;
  std::string reason = "disabled";
  std::array<std::array<uint8_t, ODID_MESSAGE_SIZE>,
             ODID_BASIC_ID_MAX_MESSAGES + 4> frames{};
  size_t frame_count = 0;
  uint64_t accepted = 0;
  uint64_t rejected = 0;
};

// Single-owner deterministic state machine; Manager supplies synchronization.
// Bounded latest-value state. No allocation, disk, radio or UART in ingest().
class Core {
 public:
  Core();
  void configure(const Config& config, uint8_t fc_system);
  void ingest(const mavlink_message_t& message, Time now);
  Report evaluate(Time now) const;
  const ODID_UAS_Data& data_for_test() const { return data_; }

 private:
  bool target(uint8_t system, uint8_t component) const;
  bool local(uint8_t system, uint8_t component, const uint8_t* id) const;
  void reset();
  Config config_;
  uint8_t fc_system_ = 1;
  ODID_UAS_Data data_{};
  Time location_at_{}, system_at_{}, source_at_{}, command_at_{};
  bool have_source_ = false, have_command_ = false, have_boot_ = false;
  uint32_t boot_ms_ = 0;
  uint8_t mask_ = 0;
  uint64_t accepted_ = 0, rejected_ = 0;
};

// Worker does encoding and optional bounded BlueZ operations outside the lock.
// UART observer only copies/validates latest values under a short lock.
class Manager {
 public:
  Manager();
  ~Manager();
  Manager(const Manager&) = delete;
  Manager& operator=(const Manager&) = delete;
  void configure(const Config& config, uint8_t fc_system);
  void ingest(const mavlink_message_t& message) noexcept;
  Report report() const;
  void stop();  // ingress must be disconnected/joined before destruction

 private:
  void run() noexcept;
  mutable std::mutex mutex_;
  std::mutex stop_mutex_;
  std::condition_variable cv_;
  Core core_;
  Config config_;
  Report report_;
  bool stop_ = false;
  uint64_t generation_ = 0;
  std::thread worker_;
};

// Vehicle system ID, dedicated dialect-defined ODID component; never OpenHD's
// onboard-computer identity, and never an ACK on behalf of the FC.
mavlink_message_t arm_status(uint8_t fc_system, const Report& report);
}  // namespace openhd::odid
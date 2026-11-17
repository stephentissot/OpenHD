// SPDX-License-Identifier: GPL-3.0-or-later
#include "Integration.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <cerrno>

namespace openhd::odid {
Integration::Integration(std::string path, int sys)
    : path_(std::move(path)), fc_system_(sys) {
  struct stat st{};
  errno = 0;
  const bool exists = ::stat(path_.c_str(), &st) == 0;
  const bool missing = !exists && errno == ENOENT;
  std::ifstream input(path_);
  if (input) {
    // Bound a corrupt/hostile configuration file; never log private contents.
    std::string text(4097, '\0');
    input.read(text.data(), text.size());
    text.resize(static_cast<size_t>(input.gcount()));
    file_valid_ = text.size() <= 4096 && parse_config(text, config_);
  } else {
    // A missing file is normal. Other read failures must not permit activation.
    file_valid_ = missing;
  }
  apply();
}
void Integration::apply() {
  auto c = config_;
  if (!file_valid_ || fc_system_ < 1 || fc_system_ > 254) c.enable = 0;
  manager_.configure(c, fc_system_ >= 1 && fc_system_ <= 254 ? fc_system_ : 1);
}
bool Integration::save(const Config& next) {
  if (!file_valid_ || !next.valid()) return false;
  const auto temporary = path_ + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) return false;
    out << serialize_config(next) << '\n';
    out.flush();
    if (!out) { std::remove(temporary.c_str()); return false; }
  }
  if (std::rename(temporary.c_str(), path_.c_str()) != 0) {
    std::remove(temporary.c_str());
    return false;
  }
  config_ = next;
  apply();
  return true;
}
bool Integration::change(int Config::*field, int value) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Never switch transmission/control policy while enabled. Disable, configure,
  // explicitly re-enable: avoids an implicit in-flight transport takeover.
  if (config_.enable && !(field == &Config::enable && value == 0)) return false;
  auto next = config_;
  next.*field = value;
  try { return save(next); } catch (...) { return false; }
}
bool Integration::change(std::string Config::*field, std::string value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (config_.enable) return false;
  auto next = config_;
  next.*field = std::move(value);
  try { return save(next); } catch (...) { return false; }
}
void Integration::set_fc_system(int sys) {
  std::lock_guard<std::mutex> lock(mutex_);
  fc_system_ = sys;
  apply();
}
std::vector<Setting> Integration::settings() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Setting> out;
  const auto weak = weak_from_this();
  auto integer = [&](const char* id, int Config::*field) {
    out.push_back({id, IntSetting{config_.*field,
        [weak, field](std::string, int value) {
          auto self = weak.lock();
          return self && self->change(field, value);
        }}});
  };
  integer("ODID_ENABLE", &Config::enable);
  integer("ODID_CTRL_SRC", &Config::control);
  integer("ODID_BLE", &Config::ble);
  integer("ODID_NAN", &Config::nan);
  integer("ODID_BEACON", &Config::beacon);
  integer("ODID_DRY_RUN", &Config::dry_run);
  integer("ODID_FC_COMP", &Config::fc_component);
  auto text = [&](const char* id, std::string Config::*field) {
    out.push_back({id, StringSetting{config_.*field,
        [weak, field](std::string, std::string value) {
          auto self = weak.lock();
          return self && self->change(field, std::move(value));
        }}});
  };
  text("ODID_BT_DEV", &Config::bt_device);
  text("ODID_WIFI_IF", &Config::wifi_interface);
  // Dynamic read-only diagnostics: accepted requests are NOT capabilities.
    out.push_back({"ODID_CAP", IntSetting{0, [](std::string, int) { return false; },
      [weak] { auto self = weak.lock(); return self ? int(self->manager_.report().available) : 0; }}});
    out.push_back({"ODID_EFFECTIVE", IntSetting{0, [](std::string, int) { return false; },
      [weak] { auto self = weak.lock(); return self ? int(self->manager_.report().effective) : 0; }}});
  out.push_back({"ODID_REQUEST", IntSetting{0, [](std::string, int) { return false; },
      [weak] { auto self = weak.lock(); return self ? int(self->manager_.report().requested) : 0; }}});
  out.push_back({"ODID_STATE", StringSetting{"disabled", [](std::string, std::string) { return false; },
      [weak] {
        auto self = weak.lock();
        if (!self) return std::string("stopped");
        std::lock_guard<std::mutex> guard(self->mutex_);
        return self->file_valid_ ? self->manager_.report().reason : std::string("invalid ODID JSON; repair/restart");
      }}});
  return out;
}
std::optional<mavlink_message_t> Integration::poll_status(Time now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!file_valid_ || !config_.enable || fc_system_ < 1 || fc_system_ > 254 ||
      now - last_status_ < std::chrono::seconds(1)) return std::nullopt;
  last_status_ = now;
  return arm_status(static_cast<uint8_t>(fc_system_), manager_.report());
}
std::string Integration::debug() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!file_valid_) return " ODID: invalid/unreadable configuration; activation blocked";
  const auto r = manager_.report();
  return " ODID: " + r.reason + " requested=" + std::to_string(r.requested) +
         " capability=" + std::to_string(r.available) +
         " registered=" + std::to_string(r.effective) + " encoded=" + std::to_string(r.frame_count) +
         " accepted=" + std::to_string(r.accepted) +
         " rejected=" + std::to_string(r.rejected);
}
}  // namespace openhd::odid
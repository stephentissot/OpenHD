// SPDX-License-Identifier: GPL-3.0-or-later
#include "ConfigJson.h"
#include <nlohmann/json.hpp>

namespace openhd::odid {
bool parse_config(const std::string& text, Config& output) {
  output = Config{};
  try {
    const auto j = nlohmann::json::parse(text);
    if (!j.is_object()) return false;
    Config c;
    auto integer = [&j](const char* key, int fallback) {
      if (!j.contains(key)) return fallback;
      const auto& v = j.at(key);
      if (!v.is_number_integer() || v.get<int64_t>() < 0 ||
          v.get<int64_t>() > 255) throw std::invalid_argument("ODID integer");
      return v.get<int>();
    };
    if (integer("schema", 1) != 1) return false;
    c.enable = integer("enable", 0);
    c.control = integer("control", 0);
    c.ble = integer("ble", 0);
    c.nan = integer("nan", 0);
    c.beacon = integer("beacon", 0);
    c.dry_run = integer("dry_run", 1);
    c.fc_component = integer("fc_component", MAV_COMP_ID_AUTOPILOT1);
    c.bt_device = j.value("bt_device", std::string{});
    c.wifi_interface = j.value("wifi_interface", std::string{});
    if (!c.valid()) return false;
    output = c;
    return true;
  } catch (...) { return false; }
}
std::string serialize_config(const Config& c) {
  return nlohmann::json{{"schema", 1}, {"enable", c.enable},
      {"control", c.control}, {"ble", c.ble}, {"nan", c.nan},
      {"beacon", c.beacon}, {"dry_run", c.dry_run},
      {"fc_component", c.fc_component}, {"bt_device", c.bt_device},
      {"wifi_interface", c.wifi_interface}}.dump(4);
}
}  // namespace openhd::odid
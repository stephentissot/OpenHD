// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic data only. This executable cannot open a UART or radio device.
#include "ConfigJson.h"
#include "Integration.h"  // CallbackGate only; no dependency on OpenHD runtime
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <future>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
using namespace openhd::odid;
using namespace std::chrono_literals;
const Time epoch = Time{} + 100s;

template <typename T> mavlink_message_t message(const T& value);
#define MESSAGE(TYPE, NAME) \
  template <> mavlink_message_t message(const mavlink_##TYPE##_t& v) { \
    mavlink_message_t m{}; mavlink_msg_##NAME##_encode(1, 1, &m, &v); return m; }
MESSAGE(open_drone_id_basic_id, open_drone_id_basic_id)
MESSAGE(open_drone_id_location, open_drone_id_location)
MESSAGE(open_drone_id_system, open_drone_id_system)
MESSAGE(open_drone_id_system_update, open_drone_id_system_update)
MESSAGE(open_drone_id_self_id, open_drone_id_self_id)
MESSAGE(open_drone_id_operator_id, open_drone_id_operator_id)
MESSAGE(command_long, command_long)
MESSAGE(system_time, system_time)
MESSAGE(heartbeat, heartbeat)

Config enabled(bool custom = false) {
  Config c;
  c.enable = 1; c.ble = 1; c.control = custom;
  return c;
}
std::vector<mavlink_message_t> fixture() {
  mavlink_open_drone_id_basic_id_t b{};
  b.id_type = 1; b.ua_type = 2;
  std::memcpy(b.uas_id, "SYNTHETIC-TEST-ONLY", 19);
  mavlink_open_drone_id_location_t l{};
  l.latitude = 10000000; l.longitude = 20000000;
  l.direction = 12345; l.speed_horizontal = 123; l.speed_vertical = -123;
  l.altitude_geodetic = 100; l.altitude_barometric = -1000;
  l.height = 10; l.timestamp = 1234.5f;
  mavlink_open_drone_id_system_t s{};
  s.operator_latitude = 30000000; s.operator_longitude = 40000000;
  s.operator_altitude_geo = 80; s.area_count = 1; s.area_radius = 100;
  s.area_ceiling = 120; s.area_floor = 50; s.timestamp = 123456;
  s.classification_type = 1; s.category_eu = 1; s.class_eu = 2;
  mavlink_open_drone_id_self_id_t self{};
  std::memcpy(self.description, "Synthetic fixture", 17);
  mavlink_open_drone_id_operator_id_t op{};
  std::memcpy(op.operator_id, "TEST-ONLY", 9);
  return {message(b), message(l), message(s), message(self), message(op)};
}
void feed(Core& c, Time now = epoch) {
  for (const auto& m : fixture()) c.ingest(m, now);
}
mavlink_message_t command(float mask) {
  mavlink_command_long_t v{};
  v.command = 31000; v.param1 = 4944; v.param2 = mask;
  return message(v);
}
void configuration() {
  Config c;
  CHECK(!c.enable && !c.ble && !c.nan && !c.beacon && c.dry_run);
  CHECK(parse_config("{}", c) && !c.enable && c.dry_run);
  CHECK(parse_config("{\"ble\":1}", c) && c.ble == 1 && !c.enable);
  CHECK(parse_config(serialize_config(enabled()), c) && c.enable == 1);
  for (const auto& text : {"null", "[]", "bad", "{\"enable\":true}",
      "{\"enable\":2}", "{\"enable\":1.0}", "{\"schema\":2}",
      "{\"enable\":-1}", "{\"enable\":4294967296}",
      "{\"bt_device\":\"hci0;reboot\"}", "{\"fc_component\":0}",
      "{\"wifi_interface\":null}"}) {
    CHECK(!parse_config(text, c)); CHECK(!c.enable);
  }
  c = enabled(); c.fc_component = 236; CHECK(!c.valid());
  Core core;
  bool threw = false;
  try { core.configure(enabled(), 0); } catch (...) { threw = true; }
  CHECK(threw);
}
void encoding_and_merge() {
  Core c;
  c.configure(enabled(), 1);
  feed(c);
  auto r = c.evaluate(epoch);
  CHECK(r.data_ready && r.frame_count == 5 && r.requested == 1);
  CHECK(r.available == 0 && r.effective == 0 && r.reason.find("DRY_RUN") == 0);
  ODID_Location_encoded raw{};
  std::memcpy(&raw, r.frames[1].data(), 25);
  ODID_Location_data l{};
  CHECK(decodeLocationMessage(&l, &raw) == ODID_SUCCESS);
  CHECK(std::abs(l.TimeStamp - 1234.5f) < 0.11f);
  CHECK(std::abs(l.Direction - 123) < 0.01f);
  CHECK(std::abs(l.SpeedVertical + 1.0f) < 0.01f);
  CHECK(std::abs(l.Latitude - 1.0) < 1e-6);
  mavlink_open_drone_id_system_update_t u{};
  u.operator_latitude = 50000000; u.operator_longitude = 60000000;
  u.operator_altitude_geo = 90; u.timestamp = 123457;
  c.ingest(message(u), epoch + 1s);
  const auto& d = c.data_for_test().System;
  CHECK(d.Timestamp == u.timestamp && d.OperatorLatitude == 5 && d.AreaRadius == 100);
  CHECK(d.ClassificationType == 1 && d.ClassEU == 2 && d.AreaCeiling == 120);
  r = c.evaluate(epoch + 1s);
  ODID_System_encoded sr{};
  std::memcpy(&sr, r.frames[2].data(), 25);
  ODID_System_data sd{};
  CHECK(decodeSystemMessage(&sd, &sr) == ODID_SUCCESS && sd.Timestamp == 123457);
  u.timestamp = 1; c.ingest(message(u), epoch + 1500ms);
  CHECK(c.data_for_test().System.Timestamp == 123457);
  // Second BasicID type occupies its own slot; replacement never overflows.
  mavlink_open_drone_id_basic_id_t b{};
  b.id_type = 2; b.ua_type = 2; b.uas_id[0] = 'X';
  c.ingest(message(b), epoch + 1s);
  CHECK(c.evaluate(epoch + 1s).frame_count == 6);
  b.id_type = 3; c.ingest(message(b), epoch + 1s);
  CHECK(c.evaluate(epoch + 1s).frame_count == 6);
  // No update can fabricate a full System before static fields have arrived.
  Core partial; partial.configure(enabled(), 1); partial.ingest(message(u), epoch);
  CHECK(!partial.data_for_test().SystemValid);
  c.configure(enabled(), 1); feed(c);
  CHECK(!c.evaluate(epoch + 3s).data_ready);
  CHECK(c.evaluate(epoch + 3s).frame_count == 0);
  // Frozen FC time is not refreshed even by repeated valid packets.
  feed(c, epoch + 2500ms);
  CHECK(!c.evaluate(epoch + 3s).data_ready);
  auto live = enabled(); live.dry_run = 0;
  c.configure(live, 1); feed(c);
  CHECK(c.evaluate(epoch).reason == "BLE/NAN/Beacon unavailable");
  auto arm = arm_status(1, c.evaluate(epoch));
  CHECK(arm.sysid == 1 && arm.compid == MAV_COMP_ID_ODID_TXRX_1);
  mavlink_open_drone_id_arm_status_t status{};
  mavlink_msg_open_drone_id_arm_status_decode(&arm, &status);
  CHECK(status.status == MAV_ODID_ARM_STATUS_PRE_ARM_FAIL_GENERIC);
}
void filtering_and_invalid() {
  const auto f = fixture();
  for (int kind = 0; kind < 5; ++kind) {
    Core c; c.configure(enabled(), 1);
    for (auto m : f) {
      if (kind == 0) m.sysid = 2;
      if (kind == 1) m.compid = 2;
      if (kind == 2) m.magic = MAVLINK_STX_MAVLINK1;
      if (kind == 3 || kind == 4) {
        // All tested ODID standard messages provide these accessors/fields.
        const auto* entry = mavlink_get_msg_entry(m.msgid);
        CHECK(entry);
        const auto offset = kind == 3 ? entry->target_system_ofs : entry->target_component_ofs;
        _MAV_PAYLOAD_NON_CONST(&m)[offset] = kind == 3 ? 2 : 237;
        // MAVLink 2 trims trailing zero bytes, including zero targets. Extend
        // the synthetic decoded payload when replacing such a trailing byte.
        m.len = std::max(m.len, static_cast<uint8_t>(offset + 1));
      }
      c.ingest(m, epoch);
    }
    CHECK(!c.evaluate(epoch).data_ready);
    CHECK(c.evaluate(epoch).accepted == 0);
  }
  // id_or_mac identifies a remote observation, not this vehicle's own data.
  Core c; c.configure(enabled(), 1);
  mavlink_open_drone_id_basic_id_t b{};
  b.id_type = 1; b.uas_id[0] = 'X'; b.id_or_mac[0] = 1;
  c.ingest(message(b), epoch);
  CHECK(!c.data_for_test().BasicIDValid[0]);
  b.id_or_mac[0] = 0;
  b.uas_id[0] = 0; b.uas_id[1] = 'X';
  for (int type : {1, 2}) {
    b.id_type = type;
    c.ingest(message(b), epoch);
    CHECK(!c.data_for_test().BasicIDValid[0]);
  }
  b.id_type = 3;  // Binary UTM UUID, zero first byte is allowed.
  c.ingest(message(b), epoch);
  CHECK(c.data_for_test().BasicIDValid[0]);
  for (float bad : {std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::infinity(), -1001.0f, 40000.0f}) {
    mavlink_open_drone_id_location_t l{};
    mavlink_msg_open_drone_id_location_decode(&f[1], &l);
    l.altitude_geodetic = bad; c.ingest(message(l), epoch);
    CHECK(!c.data_for_test().LocationValid);
    mavlink_open_drone_id_system_t s{};
    mavlink_msg_open_drone_id_system_decode(&f[2], &s);
    s.operator_altitude_geo = bad; c.ingest(message(s), epoch);
    CHECK(!c.data_for_test().SystemValid);
  }
  // A directed command to our vehicle/component is accepted, other identities aren't.
  c.configure(enabled(true), 1);
  auto cmd = command(7);
  mavlink_command_long_t v{}; mavlink_msg_command_long_decode(&cmd, &v);
  v.target_system = 1; v.target_component = 236;
  c.ingest(message(v), epoch); feed(c);
  CHECK(c.evaluate(epoch).requested == 7);
}
void custom_and_restart() {
  for (int mask = 0; mask <= 7; ++mask) {
    Core c; c.configure(enabled(true), 1);
    c.ingest(command(float(mask)), epoch); feed(c);
    CHECK(c.evaluate(epoch).requested == mask);
    CHECK(c.evaluate(epoch + 15s).requested == 0);
    CHECK(c.evaluate(epoch + 15s).reason == "FC command missing/expired");
  }
  for (float mask : {-1.0f, 8.0f, 0.5f,
      std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    Core c; c.configure(enabled(true), 1); feed(c); c.ingest(command(mask), epoch);
    CHECK(c.evaluate(epoch).requested == 0);
  }
  for (int wrong = 0; wrong < 6; ++wrong) {
    Core c; c.configure(enabled(true), 1); feed(c);
    mavlink_command_long_t v{};
    auto cmd = command(7);
    mavlink_msg_command_long_decode(&cmd, &v);
    if (wrong == 0) v.command = 31001;
    if (wrong == 1) v.param1 = 4943;
    if (wrong == 2) v.param1 = std::numeric_limits<float>::quiet_NaN();
    if (wrong == 3) v.target_system = 2;
    if (wrong == 4) v.target_component = 237;
    cmd = message(v);
    if (wrong == 5) cmd.sysid = 2;
    c.ingest(cmd, epoch);
    CHECK(c.evaluate(epoch).requested == 0);
  }
  Core c; c.configure(enabled(true), 1);
  mavlink_system_time_t t{}; t.time_boot_ms = 100000;
  c.ingest(message(t), epoch); c.ingest(command(1), epoch); feed(c);
  CHECK(c.evaluate(epoch).data_ready);
  t.time_boot_ms = 99500; c.ingest(message(t), epoch + 100ms);
  CHECK(c.evaluate(epoch + 100ms).data_ready);
  t.time_boot_ms = 10; c.ingest(message(t), epoch + 200ms);
  CHECK(!c.evaluate(epoch + 200ms).data_ready && !c.data_for_test().SystemValid);
  CHECK(c.evaluate(epoch + 200ms).requested == 0);
  // Configuration/source changes and disable/re-enable cannot replay a command.
  c.ingest(command(1), epoch); feed(c);
  c.configure(Config{}, 1); feed(c);
  CHECK(!c.evaluate(epoch).enabled && c.evaluate(epoch).frame_count == 0);
  c.configure(enabled(true), 1); feed(c);
  CHECK(c.evaluate(epoch).requested == 0);
  c.ingest(command(1), epoch); feed(c);
  mavlink_heartbeat_t hb{}; c.ingest(message(hb), epoch + 6s);
  CHECK(!c.data_for_test().LocationValid && c.evaluate(epoch + 6s).requested == 0);
  // uint32 boot wrap is not a reboot.
  c.configure(enabled(true), 1);
  t.time_boot_ms = 0xfffffff0; c.ingest(message(t), epoch);
  c.ingest(command(1), epoch); feed(c);
  t.time_boot_ms = 10; c.ingest(message(t), epoch + 1s);
  CHECK(c.evaluate(epoch + 1s).data_ready);
}
void lifecycle() {
  for (int i = 0; i < 50; ++i) {
    Manager m;
    m.configure(enabled(), 1);
    const auto f = fixture();
    std::thread input([&] { for (int j = 0; j < 1000; ++j) m.ingest(f[j % f.size()]); });
    std::thread config([&] { for (int j = 0; j < 50; ++j) {
      m.configure(j % 2 ? enabled() : Config{}, 1); (void)m.report();
    } });
    input.join(); config.join();
    m.configure(Config{}, 1);
    CHECK(!m.report().enabled && m.report().frame_count == 0);
    std::thread stop([&] { m.stop(); });
    m.stop(); stop.join();
  }
  auto gate = std::make_shared<CallbackGate>();
  std::promise<void> entered, release;
  auto released = release.get_future();
  std::thread cb([&] { gate->run([&] { entered.set_value(); released.wait(); }); });
  entered.get_future().wait();
  auto close = std::async(std::launch::async, [&] { gate->close(); });
  CHECK(close.wait_for(10ms) == std::future_status::timeout);
  release.set_value(); cb.join(); close.get();
  bool called = false; gate->run([&] { called = true; }); CHECK(!called);
  CallbackGate exceptions;
  try { exceptions.run([] { throw 1; }); } catch (...) {}
  exceptions.close();
}
void persistent_integration() {
  char directory[] = "/tmp/openhd-odid-test-XXXXXX";
  CHECK(mkdtemp(directory));
  const std::string path = std::string(directory) + "/opendroneid.json";
  auto set = [](const std::vector<openhd::Setting>& settings, const std::string& id, int value) {
    for (const auto& s : settings) if (s.id == id)
      return std::get<openhd::IntSetting>(s.setting).change_callback(id, value);
    CHECK(false); return false;
  };
  std::vector<openhd::Setting> old_settings;
  {
    auto integration = std::make_shared<Integration>(path, 1);
    old_settings = integration->settings();
    for (const auto& s : old_settings) CHECK(s.id.size() <= 16);
    CHECK(!integration->poll_status(Clock::now()));
    CHECK(access(path.c_str(), F_OK) == -1);  // default disabled does not write
    CHECK(!set(old_settings, "ODID_ENABLE", 2));
    CHECK(set(old_settings, "ODID_BLE", 1));
    CHECK(set(old_settings, "ODID_ENABLE", 1));
    CHECK(!set(old_settings, "ODID_CTRL_SRC", 1)); // no hot policy changes
    CHECK(integration->poll_status(Clock::now()).has_value());
    CHECK(!integration->poll_status(Clock::now())); // bounded 1Hz
    CHECK(set(old_settings, "ODID_ENABLE", 0));
    CHECK(!integration->poll_status(Clock::now() + 2s));
  }
  CHECK(!set(old_settings, "ODID_ENABLE", 1)); // no use-after-free parameter callbacks
  {
    auto integration = std::make_shared<Integration>(path, 1);
    auto settings = integration->settings();
    for (const auto& s : settings) if (s.id == "ODID_BLE")
      CHECK(std::get<openhd::IntSetting>(s.setting).value == 1);
  }
  {
    std::ofstream out(path); out << "{\"enable\":1,\"ble\":false}";
  }
  {
    auto integration = std::make_shared<Integration>(path, 1);
    CHECK(!set(integration->settings(), "ODID_ENABLE", 1));
    CHECK(integration->debug().find("invalid/unreadable") != std::string::npos);
    CHECK(!integration->poll_status(Clock::now()));
  }
  std::ifstream input(path); std::string contents;
  std::getline(input, contents);
  CHECK(contents == "{\"enable\":1,\"ble\":false}"); // preserve invalid file
  unlink(path.c_str()); rmdir(directory);
}
int main() {
  configuration(); encoding_and_merge(); filtering_and_invalid();
  custom_and_restart(); lifecycle(); persistent_integration();
  std::cout << "OpenHD ODID: config/encode/filter/custom/stale/restart/lifecycle PASS\n";
}
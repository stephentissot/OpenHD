// SPDX-License-Identifier: GPL-3.0-or-later
#include "Core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#ifdef OPENHD_ODID_BLUEZ
#include "BluezBackend.h"
#endif

namespace openhd::odid {
namespace {
bool flag(int v) { return v == 0 || v == 1; }
bool name(const std::string& s) {
  return s.size() <= 15 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}
bool coordinates(int32_t lat, int32_t lon) {
  return lat >= -900000000 && lat <= 900000000 &&
         lon >= -1800000000 && lon <= 1800000000;
}
bool altitude(float v) {
  return std::isfinite(v) && v >= MIN_ALT && v <= MAX_ALT;
}
bool fresh(Time now, Time then, std::chrono::seconds limit) {
  return now >= then && now - then < limit;
}
template <typename T> void copy_string(T& dst, const void* src, size_t n) {
  static_assert(sizeof(dst) > 1);
  std::memset(dst, 0, sizeof(dst));
  std::memcpy(dst, src, std::min(n, sizeof(dst) - 1));
}
}  // namespace

bool Config::valid() const {
  return flag(enable) && flag(control) && flag(ble) && flag(nan) &&
         flag(beacon) && flag(dry_run) && fc_component > 0 &&
         fc_component < 256 && fc_component != MAV_COMP_ID_ODID_TXRX_1 &&
         name(bt_device) && name(wifi_interface);
}

Core::Core() { reset(); }
void Core::reset() {
  odid_initUasData(&data_);
  have_source_ = have_command_ = have_boot_ = false;
  mask_ = 0;
}
void Core::configure(const Config& config, uint8_t fc_system) {
  if (!config.valid() || fc_system == 0 || fc_system == 255)
    throw std::invalid_argument("invalid ODID configuration/source");
  // Every policy change requires fresh FC data and (if custom) authorization.
  reset();
  config_ = config;
  fc_system_ = fc_system;
}
bool Core::target(uint8_t sys, uint8_t comp) const {
  return (sys == 0 || sys == fc_system_) &&
         (comp == 0 || comp == MAV_COMP_ID_ODID_TXRX_1);
}
bool Core::local(uint8_t sys, uint8_t comp, const uint8_t* id) const {
  return target(sys, comp) &&
         std::all_of(id, id + 20, [](uint8_t b) { return b == 0; });
}

void Core::ingest(const mavlink_message_t& m, Time now) {
  if (!config_.enable) return;
  // Only already-parsed MAVLink 2 from the explicitly configured FC component.
  if (m.magic != MAVLINK_STX || m.sysid != fc_system_ ||
      m.compid != config_.fc_component) {
    ++rejected_;
    return;
  }
  // A monotonic FC boot counter rollback > 2s invalidates ALL cached identities
  // and authorization. Small packet reordering is ignored; uint32 wrap is not
  // a reboot. Without SYSTEM_TIME, a 5s input gap also starts a new epoch.
  if (have_source_ && !fresh(now, source_at_, std::chrono::seconds(5))) reset();
  if (m.msgid == MAVLINK_MSG_ID_SYSTEM_TIME) {
    mavlink_system_time_t v{};
    mavlink_msg_system_time_decode(&m, &v);
    if (have_boot_ && v.time_boot_ms < boot_ms_ &&
        boot_ms_ - v.time_boot_ms > 2000 &&
        !(boot_ms_ > 0xffff0000U && v.time_boot_ms < 60000)) reset();
    if (!have_boot_ || v.time_boot_ms >= boot_ms_ || boot_ms_ > 0xffff0000U)
      boot_ms_ = v.time_boot_ms;
    have_boot_ = true;
    source_at_ = now;
    have_source_ = true;
    return;
  }
  bool ok = false;
  switch (m.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT:
      ok = true;
      break;
    case MAVLINK_MSG_ID_COMMAND_LONG: {
      mavlink_command_long_t v{};
      mavlink_msg_command_long_decode(&m, &v);
      if (config_.control != 1 || v.command != 31000 ||
          v.param1 != 4944.0f || !target(v.target_system, v.target_component) ||
          !std::isfinite(v.param2) || v.param2 < 0 || v.param2 > 7 ||
          std::floor(v.param2) != v.param2) break;
      mask_ = static_cast<uint8_t>(v.param2);
      command_at_ = now;
      have_command_ = true;
      ok = true;
      break;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID: {
      mavlink_open_drone_id_basic_id_t v{};
      mavlink_msg_open_drone_id_basic_id_decode(&m, &v);
      if (!local(v.target_system, v.target_component, v.id_or_mac) ||
          v.id_type < 1 || v.id_type > 4 || v.ua_type > 15 ||
          std::all_of(v.uas_id, v.uas_id + 20, [](uint8_t b) { return b == 0; })) break;
        // Serial/CAA IDs are strings in the codec; bytes after an initial NUL
        // cannot turn an empty identity into a usable one. Binary IDs may start 0.
        if ((v.id_type == 1 || v.id_type == 2) && v.uas_id[0] == 0) break;
      size_t slot = ODID_BASIC_ID_MAX_MESSAGES;
      for (size_t i = 0; i < ODID_BASIC_ID_MAX_MESSAGES; ++i)
        if (data_.BasicIDValid[i] && data_.BasicID[i].IDType == v.id_type) {
          slot = i;
          break;
        }
      if (slot == ODID_BASIC_ID_MAX_MESSAGES)
        for (size_t i = 0; i < ODID_BASIC_ID_MAX_MESSAGES; ++i)
          if (!data_.BasicIDValid[i]) { slot = i; break; }
      if (slot == ODID_BASIC_ID_MAX_MESSAGES) break;  // bounded, no eviction
      auto& d = data_.BasicID[slot];
      d.IDType = static_cast<ODID_idtype_t>(v.id_type);
      d.UAType = static_cast<ODID_uatype_t>(v.ua_type);
      copy_string(d.UASID, v.uas_id, 20);
      data_.BasicIDValid[slot] = 1;
      ok = true;
      break;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_LOCATION: {
      mavlink_open_drone_id_location_t v{};
      mavlink_msg_open_drone_id_location_decode(&m, &v);
      if (!local(v.target_system, v.target_component, v.id_or_mac) ||
          !coordinates(v.latitude, v.longitude) ||
          !altitude(v.altitude_barometric) || !altitude(v.altitude_geodetic) ||
          !altitude(v.height) || !std::isfinite(v.timestamp) ||
          !((v.timestamp >= 0 && v.timestamp < 3600) || v.timestamp == INV_TIMESTAMP) ||
          !(v.direction <= 35999 || v.direction == 36100) ||
          !(v.speed_horizontal <= 25425 || v.speed_horizontal == 25500) ||
          !(v.speed_vertical >= -6200 && (v.speed_vertical <= 6200 || v.speed_vertical == 6300)) ||
          v.status > 4 || v.height_reference > 1 || v.horizontal_accuracy > 12 ||
          v.vertical_accuracy > 6 || v.barometer_accuracy > 6 ||
          v.speed_accuracy > 4 || v.timestamp_accuracy > 15) break;
      auto& d = data_.Location;
      if (data_.LocationValid && d.TimeStamp != INV_TIMESTAMP &&
          v.timestamp != INV_TIMESTAMP) {
        // UTC seconds-of-hour may wrap. Reject backwards/replayed positions
        // rather than renewing their monotonic freshness. A large clock jump
        // needs a source epoch reset (boot rollback or input gap).
        const float delta = std::fmod(v.timestamp - d.TimeStamp + 3600.0f, 3600.0f);
        if (delta > 1800.0f) break;
      }
      // Repeated FC time must not make a frozen location fresh.
      const bool advanced = !data_.LocationValid || d.TimeStamp != v.timestamp;
      d.Status = static_cast<ODID_status_t>(v.status);
      d.Direction = v.direction / 100.0f;
      d.SpeedHorizontal = v.speed_horizontal / 100.0f;
      d.SpeedVertical = v.speed_vertical / 100.0f;
      d.Latitude = v.latitude / 1e7;
      d.Longitude = v.longitude / 1e7;
      d.AltitudeBaro = v.altitude_barometric;
      d.AltitudeGeo = v.altitude_geodetic;
      d.HeightType = static_cast<ODID_Height_reference_t>(v.height_reference);
      d.Height = v.height;
      d.HorizAccuracy = static_cast<ODID_Horizontal_accuracy_t>(v.horizontal_accuracy);
      d.VertAccuracy = static_cast<ODID_Vertical_accuracy_t>(v.vertical_accuracy);
      d.BaroAccuracy = static_cast<ODID_Vertical_accuracy_t>(v.barometer_accuracy);
      d.SpeedAccuracy = static_cast<ODID_Speed_accuracy_t>(v.speed_accuracy);
      d.TSAccuracy = static_cast<ODID_Timestamp_accuracy_t>(v.timestamp_accuracy);
      d.TimeStamp = v.timestamp;
      data_.LocationValid = 1;
      if (advanced) location_at_ = now;
      ok = true;
      break;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SYSTEM: {
      mavlink_open_drone_id_system_t v{};
      mavlink_msg_open_drone_id_system_decode(&m, &v);
      if (!local(v.target_system, v.target_component, v.id_or_mac) ||
          !coordinates(v.operator_latitude, v.operator_longitude) ||
          !altitude(v.area_ceiling) || !altitude(v.area_floor) ||
          !altitude(v.operator_altitude_geo) || v.area_radius > MAX_AREA_RADIUS ||
          v.operator_location_type > 2 || v.classification_type > 1 ||
          v.category_eu > 3 || v.class_eu > 7 ||
          (data_.SystemValid && v.timestamp < data_.System.Timestamp)) break;
      auto& d = data_.System;
      if (!data_.SystemValid || d.Timestamp != v.timestamp) system_at_ = now;
      d.OperatorLocationType = static_cast<ODID_operator_location_type_t>(v.operator_location_type);
      d.ClassificationType = static_cast<ODID_classification_type_t>(v.classification_type);
      d.OperatorLatitude = v.operator_latitude / 1e7;
      d.OperatorLongitude = v.operator_longitude / 1e7;
      d.AreaCount = v.area_count;
      d.AreaRadius = v.area_radius;
      d.AreaCeiling = v.area_ceiling;
      d.AreaFloor = v.area_floor;
      d.CategoryEU = static_cast<ODID_category_EU_t>(v.category_eu);
      d.ClassEU = static_cast<ODID_class_EU_t>(v.class_eu);
      d.OperatorAltitudeGeo = v.operator_altitude_geo;
      d.Timestamp = v.timestamp;
      data_.SystemValid = 1;
      ok = true;
      break;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SYSTEM_UPDATE: {
      mavlink_open_drone_id_system_update_t v{};
      mavlink_msg_open_drone_id_system_update_decode(&m, &v);
      if (!target(v.target_system, v.target_component) || !data_.SystemValid ||
          !coordinates(v.operator_latitude, v.operator_longitude) ||
          !altitude(v.operator_altitude_geo) || v.timestamp < data_.System.Timestamp) break;
      auto& d = data_.System;
      if (d.Timestamp != v.timestamp) system_at_ = now;
      d.OperatorLatitude = v.operator_latitude / 1e7;
      d.OperatorLongitude = v.operator_longitude / 1e7;
      d.OperatorAltitudeGeo = v.operator_altitude_geo;
      d.Timestamp = v.timestamp;
      ok = true;
      break;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SELF_ID: {
      mavlink_open_drone_id_self_id_t v{};
      mavlink_msg_open_drone_id_self_id_decode(&m, &v);
      if (!local(v.target_system, v.target_component, v.id_or_mac) || v.description_type > 2) break;
      data_.SelfID.DescType = static_cast<ODID_desctype_t>(v.description_type);
      copy_string(data_.SelfID.Desc, v.description, 23);
      data_.SelfIDValid = 1;
      ok = true;
      break;
    }
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_OPERATOR_ID: {
      mavlink_open_drone_id_operator_id_t v{};
      mavlink_msg_open_drone_id_operator_id_decode(&m, &v);
      if (!local(v.target_system, v.target_component, v.id_or_mac) || v.operator_id_type != 0) break;
      data_.OperatorID.OperatorIdType = static_cast<ODID_operatorIdType_t>(v.operator_id_type);
      copy_string(data_.OperatorID.OperatorId, v.operator_id, 20);
      data_.OperatorIDValid = 1;
      ok = true;
      break;
    }
    default: return;  // Auth and Message Pack deliberately unsupported
  }
  if (ok) {
    ++accepted_;
    source_at_ = now;
    have_source_ = true;
  } else {
    ++rejected_;
  }
}

Report Core::evaluate(Time now) const {
  Report r;
  r.enabled = config_.enable;
  r.accepted = accepted_;
  r.rejected = rejected_;
  if (!r.enabled) return r;
  if (config_.control == 1) {
    if (!have_command_ || !fresh(now, command_at_, std::chrono::seconds(15))) {
      r.reason = "FC command missing/expired";
      return r;
    }
    r.requested = mask_;
  } else {
    r.requested = config_.ble | (config_.nan << 1) | (config_.beacon << 2);
  }
  if (!have_source_ || !fresh(now, source_at_, std::chrono::seconds(5))) {
    r.reason = "FC source stale";
    return r;
  }
  if (!data_.LocationValid || !fresh(now, location_at_, std::chrono::seconds(3)) ||
      data_.Location.TimeStamp == INV_TIMESTAMP ||
      (data_.Location.Latitude == 0 && data_.Location.Longitude == 0) ||
      data_.Location.AltitudeGeo == INV_ALT || !data_.SystemValid ||
      !fresh(now, system_at_, std::chrono::seconds(3)) || data_.System.Timestamp == 0 ||
      (data_.System.OperatorLatitude == 0 && data_.System.OperatorLongitude == 0) ||
      data_.System.OperatorAltitudeGeo == INV_ALT || !data_.SelfIDValid ||
      !data_.OperatorIDValid || data_.OperatorID.OperatorId[0] == 0 ||
      !std::any_of(std::begin(data_.BasicIDValid), std::end(data_.BasicIDValid),
                   [](uint8_t v) { return v != 0; })) {
    r.reason = "ODID data missing/stale/unknown";
    return r;  // fail closed: do not produce a stale radio frame set
  }
  auto append = [&r](const auto& encoded, int result) {
    if (result != ODID_SUCCESS) return false;
    static_assert(sizeof(encoded) == ODID_MESSAGE_SIZE);
    std::memcpy(r.frames[r.frame_count++].data(), &encoded, ODID_MESSAGE_SIZE);
    return true;
  };
  bool ok = true;
  for (size_t i = 0; i < ODID_BASIC_ID_MAX_MESSAGES; ++i) {
    if (!data_.BasicIDValid[i]) continue;
    ODID_BasicID_encoded v{};
    const auto rc = encodeBasicIDMessage(&v, &data_.BasicID[i]);
    ok &= append(v, rc);
  }
  ODID_Location_encoded l{};
  ODID_System_encoded s{};
  ODID_SelfID_encoded self{};
  ODID_OperatorID_encoded op{};
  auto rc = encodeLocationMessage(&l, &data_.Location); ok &= append(l, rc);
  rc = encodeSystemMessage(&s, &data_.System); ok &= append(s, rc);
  rc = encodeSelfIDMessage(&self, &data_.SelfID); ok &= append(self, rc);
  rc = encodeOperatorIDMessage(&op, &data_.OperatorID); ok &= append(op, rc);
  r.data_ready = ok;
  if (!ok) { r.frame_count = 0; r.reason = "ODID encoder rejected data"; }
  else if (!r.requested) r.reason = "no transport requested";
  else if (config_.dry_run) r.reason = "DRY_RUN: no RF emission";
  else r.reason = "BLE/NAN/Beacon unavailable";
  return r;
}

Manager::Manager() : worker_(&Manager::run, this) {}
Manager::~Manager() { stop(); }
void Manager::stop() {
  std::lock_guard<std::mutex> stopping(stop_mutex_);
  { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}
void Manager::configure(const Config& c, uint8_t sys) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stop_) return;
  core_.configure(c, sys);
  config_ = c;
  report_ = Report{};  // no stale ready result after a policy transition
  report_.enabled = c.enable;
  report_.reason = c.enable ? "awaiting fresh FC data" : "disabled";
  ++generation_;
  cv_.notify_all();
}
void Manager::ingest(const mavlink_message_t& m) noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stop_) core_.ingest(m, Clock::now());
  } catch (...) { /* Never propagate an optional feature failure to UART. */ }
}
Report Manager::report() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return report_;
}
void Manager::run() noexcept {
#ifdef OPENHD_ODID_BLUEZ
  BluezBackend backend;
  size_t frame_index = 0;
  uint8_t counter = 0;
  Time retry_at{};
  uint64_t radio_generation = 0;
#endif
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    if (!config_.enable) {
#ifdef OPENHD_ODID_BLUEZ
      lock.unlock();
      backend.stop();
      lock.lock();
#endif
      cv_.wait(lock, [this] { return stop_ || config_.enable; });
      continue;
    }
    const auto generation = generation_;
    const auto snapshot = core_;
  #ifdef OPENHD_ODID_BLUEZ
    const auto policy = config_;
  #endif
    lock.unlock();
    Report result;
    try { result = snapshot.evaluate(Clock::now()); }
    catch (...) { result.enabled = true; result.reason = "ODID worker failure"; }
#ifdef OPENHD_ODID_BLUEZ
    try {
      if (radio_generation != generation) {
        backend.stop();
        frame_index = 0;
        retry_at = Time{};
        radio_generation = generation;
      }
      if (policy.dry_run || !result.data_ready || !(result.requested & 1)) {
        backend.stop();
      } else if (Clock::now() >= retry_at) {
        const auto error = backend.start_or_update(
            result.frames[frame_index % result.frame_count], counter++,
            policy.bt_device);
        if (error.empty() && backend.active()) {
          ++frame_index;
          result.available = 1;
          // Effective denotes local BlueZ registration, not proven reception.
          result.effective = 1;
          result.reason = (result.requested & ~1)
              ? "BLE registered; NAN/Beacon unavailable"
              : "BLE registered; RF unverified";
        } else {
          result.reason = error.empty() ? "BLE registration lost" : error;
          retry_at = Clock::now() + std::chrono::seconds(5);
        }
      } else {
        result.reason = "BLE unavailable; retry backoff";
      }
    } catch (...) {
      backend.stop();
      result.effective = result.available = 0;
      result.reason = "BLE worker failure";
      retry_at = Clock::now() + std::chrono::seconds(5);
    }
#endif
    lock.lock();
  #ifdef OPENHD_ODID_BLUEZ
    // Radio I/O can cross a freshness deadline or a FC reboot. Re-evaluate the
    // CURRENT bounded cache after I/O, not only the pre-I/O snapshot.
    if (result.effective) {
      const auto latest = core_;
      lock.unlock();
      // Encoding stays outside the UART observer's mutex, including rechecks.
      const auto current = latest.evaluate(Clock::now());
      lock.lock();
      if (stop_ || generation != generation_ || !current.data_ready ||
          !(current.requested & 1)) {
        result.effective = 0;
        result.reason = "BLE withdrawal requested: data expired/changed";
        lock.unlock();
        backend.stop();
        lock.lock();
      }
    }
  #endif
    if (!stop_ && generation == generation_) {
      report_ = std::move(result);
    }
#ifdef OPENHD_ODID_BLUEZ
    else {
      // A disable/source change during a bounded D-Bus operation invalidates
      // that operation's result. Withdraw our advertisement before waiting.
      lock.unlock();
      backend.stop();
      lock.lock();
    }
#endif
    cv_.wait_until(lock, Clock::now() + std::chrono::milliseconds(200),
                   [this, generation] { return stop_ || generation != generation_; });
  }
  lock.unlock();
#ifdef OPENHD_ODID_BLUEZ
  backend.stop();
#endif
}

mavlink_message_t arm_status(uint8_t sys, const Report& r) {
  static_assert(MAV_COMP_ID_ODID_TXRX_1 == 236, "verify local MAVLink dialect");
  mavlink_open_drone_id_arm_status_t payload{};
  // Intentionally NEVER good-to-arm: no validated RF backend in this stage.
  payload.status = MAV_ODID_ARM_STATUS_PRE_ARM_FAIL_GENERIC;
  copy_string(payload.error, r.reason.data(), r.reason.size());
  mavlink_message_t message{};
  mavlink_msg_open_drone_id_arm_status_encode(sys, MAV_COMP_ID_ODID_TXRX_1,
                                             &message, &payload);
  return message;
}
}  // namespace openhd::odid
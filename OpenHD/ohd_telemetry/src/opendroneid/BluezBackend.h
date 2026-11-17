// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace openhd::odid {
// Experimental, explicitly invoked backend; opt-in Manager worker integration.
// Serialize start_or_update()/stop() on the caller's worker (never UART).
// active() is a local registration observation, NOT proof of RF/compliance.
class BluezBackend {
 public:
  BluezBackend() noexcept = default;  // No thread, bus, sysfs or radio access.
  ~BluezBackend();
  BluezBackend(const BluezBackend&) = delete;
  BluezBackend& operator=(const BluezBackend&) = delete;
  BluezBackend(BluezBackend&&) = delete;
  BluezBackend& operator=(BluezBackend&&) = delete;

  std::string start_or_update(const std::array<uint8_t, 25>& frame,
                              uint8_t counter, const std::string& hci_name);
  void stop() noexcept;
  bool active() const;

 private:
  struct State;
  std::shared_ptr<State> state_;
};
}  // namespace openhd::odid
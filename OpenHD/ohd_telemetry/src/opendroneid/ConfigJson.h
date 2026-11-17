// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Core.h"

namespace openhd::odid {
// Missing fields migrate with safe defaults; invalid types/values/schema fail
// closed. Caller preserves invalid files and refuses parameter activation.
bool parse_config(const std::string& text, Config& output);
std::string serialize_config(const Config& config);
}  // namespace openhd::odid
#pragma once

#include <cstdint>

namespace OpenShock {
  enum class EStopState : uint8_t {
    Idle,
    Active,
    ActiveClearing,
    AwaitingRelease  // Unused since SP-174 replaced the momentary hold-to-clear. The visual state still names it.
  };
}  // namespace OpenShock

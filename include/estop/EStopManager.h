#pragma once

#include "estop/EStopState.h"
#include "softproxy/KillPath.h"

#include <hal/gpio_types.h>

#include <cstdint>

namespace OpenShock::EStopManager {
  [[nodiscard]] bool Init();
  bool SetEStopEnabled(bool enabled);
  bool SetEStopPin(gpio_num_t pin);
  bool IsEStopped();
  int64_t LastEStopped();

  /// Whether the checker task runs on a valid input pin and has sampled it recently. Without this nothing can engage
  /// the E-Stop from the button, so `HandleCommand` refuses shock and vibrate and a cap cannot rise.
  bool IsArmed();

  /// The pin the checker reads, or GPIO_NUM_NC when none is set. The stored pin can differ until a restart.
  gpio_num_t GetRunningPin();

  /// What set the latch, or `Released`. The first source holds until the hold-to-clear: a later press or `Trigger` does
  /// not restate it. A rise needs `Button` (SP-156).
  Softproxy::EStopLatchSource GetLatchSource();

  /// Engages the E-Stop at once, with or without a checker task. Only the hold-to-clear releases it, so with no task it
  /// holds until a power cycle swaps it for the cold boot latch, which stays engaged.
  void Trigger();
}  // namespace OpenShock::EStopManager

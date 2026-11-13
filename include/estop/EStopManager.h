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

  /// The longest time between two samples of one checker task since boot, in ms. A checker is stale, so not armed, after
  /// 1000 ms without a sample. The bench reads this to see how close the real worst gap comes (review T1).
  uint32_t MaxSampleIntervalMs();

  /// State event posts that failed since boot. A post that fails is retried on the next pass. Only the keep-alive
  /// switch and the LED follow the post, so the latch and `IsArmed` never wait on it (review T2).
  uint32_t FailedStatePosts();

  /// What set the latch, or `Released`. The first source holds until the clear: a later press or `Trigger` does not
  /// restate it. The one change is `Unconfirmed` to `Button`, when a press that follows a measured release turns solid.
  /// A rise needs `Button` (SP-156).
  Softproxy::EStopLatchSource GetLatchSource();

  /// Engages the E-Stop at once, with or without a checker task, and stops a clear in progress. Only the clear releases
  /// it: the mushroom measured down, then twisted up and left up for the clear time (SP-174). On a hub with no task and
  /// no stored pin nothing can run that, so it holds until a power cycle swaps it for the cold boot latch, which stays
  /// engaged. With a stored pin `estop enabled true` starts the task.
  void Trigger();
}  // namespace OpenShock::EStopManager

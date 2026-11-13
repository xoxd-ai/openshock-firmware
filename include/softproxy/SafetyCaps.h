#pragma once

// Hub-side store and gate for the Softproxy caps (include/softproxy/CapLimiter.h).
//
// The caps live in their own NVS namespace, outside the config file. So `rawconfig`, `jsonconfig`
// and the gateway cannot read or change them. Only the local serial command `safetycaps` sets them,
// and `factoryreset` clears them. A missing, unreadable or invalid record loads as all 0, which
// refuses every shock and vibrate command.

#include "ShockerCommandType.h"
#include "softproxy/CapLimiter.h"
#include "softproxy/RefusalReport.h"

#include <cstdint>

namespace OpenShock::Softproxy::SafetyCaps {
  /// Loads the stored caps. Safe to call more than once. On failure the caps stay 0 and every capped command refuses.
  bool Init();

  /// Copies the current caps of both channels.
  void Get(ChannelCaps& shock, ChannelCaps& vibrate);

  /// Stages counted for a channel in the last hour and day. `type` must be Shock or Vibrate.
  bool GetCounts(ShockerCommandType type, uint32_t& lastHour, uint32_t& lastDay);

  /// Result of `Set`.
  enum class SetResult : uint8_t {
    Stored,
    Invalid,            // Not Shock or Vibrate, or the caps fail `CapsSet` or `CapsCoherent`.
    Unreadable,         // The stored record cannot be read. Run `safetycaps clear` first.
    NeedsEngagedEStop,  // A cap would rise without an armed E-Stop engaged by its button (SP-113 rule 2, SP-156).
    StoreFailed,
  };

  /// Stores the caps of one channel. A write that raises any cap needs the E-Stop engaged and armed. A write that
  /// only lowers caps does not, and neither does `Clear`.
  SetResult Set(ShockerCommandType type, const ChannelCaps& caps);

  /// Whether any stored cap is above 0. An unreadable store counts as set, so the E-Stop stays locked.
  bool AnySet();

  /// Sets every cap of both channels to 0 and erases the stored record.
  bool Clear();

  /// Checks one command and records it when it is admitted. Stop, Sound and Light return `NotCapped`.
  /// A refusal is counted and logged (include/softproxy/RefusalReport.h). `admittedAtMs` receives the
  /// `OpenShock::millis()` reading the command was checked at, the start of the interval the limiter counts. The radio
  /// ends the command from it (review 3 F1).
  Verdict Admit(ShockerCommandType type, uint8_t intensity, uint16_t durationMs, int64_t& admittedAtMs);

  /// Counts and logs a refusal that came from outside `Admit`, for the E-Stop checks in `CommandHandler::HandleCommand`.
  void RecordRefusal(ShockerCommandType type, Verdict verdict);

  /// Refusals since boot and the latest one's wire code, for the gateway heartbeat and the serial port. Takes no lock.
  RefusalSnapshot GetRefusals();
}  // namespace OpenShock::Softproxy::SafetyCaps

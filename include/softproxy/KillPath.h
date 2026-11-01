#pragma once

// Decisions for the hub E-Stop as a Softproxy kill path (TIN-7125 SP-47, SP-113 and SP-136). src/EStopManager.cpp,
// src/CommandHandler.cpp, src/softproxy/SafetyCaps.cpp and src/config/Config.cpp apply them. No Arduino or ESP-IDF
// dependency, so a host compiler can test them (test/native/test_kill_path.cpp).

#include "softproxy/CapLimiter.h"

#include <cstdint>

namespace OpenShock::Softproxy {
#ifdef OPENSHOCK_LED_GPIO
  constexpr int kBoardLedPin = OPENSHOCK_LED_GPIO;
#else
  constexpr int kBoardLedPin = -1;
#endif
#ifdef OPENSHOCK_LED_WS2812B
  constexpr int kBoardRgbLedPin = OPENSHOCK_LED_WS2812B;
#else
  constexpr int kBoardRgbLedPin = -1;
#endif
#ifdef OPENSHOCK_RF_TX_GPIO
  constexpr int kBoardRfTxPin = OPENSHOCK_RF_TX_GPIO;
#else
  constexpr int kBoardRfTxPin = -1;
#endif
#ifdef OPENSHOCK_FW_CHIP_ESP32
  constexpr bool kChipHasInputOnlyPins = true;
#else
  constexpr bool kChipHasInputOnlyPins = false;
#endif

  /// The E-Stop input needs a pull-up, because the button pulls the pin low. GPIO 34 to 39 of a plain ESP32 have no
  /// internal pull-up, so with nothing external they float. The fork has no way to declare an external one.
  constexpr bool HasInternalPullUp(int pin)
  {
    return !(kChipHasInputOnlyPins && pin >= 34 && pin <= 39);
  }

  /// Whether a set E-Stop pin is unusable: it is the RF transmitter, the transmitter's board default (the hub falls
  /// back to it), a board LED, or has no pull-up. An unset pin (below 0) never conflicts. It leaves the E-Stop off.
  constexpr bool EStopPinConflicts(int estopPin, int txPin)
  {
    if (estopPin < 0) {
      return false;
    }
    return estopPin == txPin || estopPin == kBoardRfTxPin || estopPin == kBoardLedPin || estopPin == kBoardRgbLedPin || !HasInternalPullUp(estopPin);
  }

  /// Whether moving the RF transmitter to `txPin` would land on the E-Stop or a board LED. `configuredEStopPin` is
  /// the stored pin and `runningEStopPin` the pin the checker task reads. They differ until a restart.
  constexpr bool TxPinConflicts(int txPin, int configuredEStopPin, int runningEStopPin)
  {
    if (txPin < 0) {
      return false;
    }
    return txPin == configuredEStopPin || txPin == runningEStopPin || txPin == kBoardLedPin || txPin == kBoardRgbLedPin;
  }

  /// The E-Stop part of a whole-config write, from `jsonconfig` or `rawconfig`.
  struct KillConfig {
    bool enabled;
    int pin;
  };

  /// Whether a whole-config write may replace `current` with `next`. The new pins must stay apart in both directions,
  /// also from the pin the checker task reads now. While the E-Stop is engaged or any cap is set, `killLocked` is true
  /// and the E-Stop settings must not change, as for serial `estop`.
  constexpr bool KillConfigChangeAllowed(const KillConfig& current, const KillConfig& next, int nextTxPin, int runningEStopPin, bool killLocked)
  {
    if (EStopPinConflicts(next.pin, nextTxPin) || TxPinConflicts(nextTxPin, next.pin, runningEStopPin)) {
      return false;
    }
    return !killLocked || (next.enabled == current.enabled && next.pin == current.pin);
  }

  /// Whether `to` lets through anything `from` refuses. Lower `minGapSec` is a rise, since it admits sooner.
  constexpr bool CapsRise(const ChannelCaps& from, const ChannelCaps& to)
  {
    // Clearing admits nothing, even though `minGapSec` falls to 0.
    if (CapsCleared(to)) {
      return false;
    }
    return to.maxIntensityPct > from.maxIntensityPct || to.maxPulseMs > from.maxPulseMs || to.maxPulsesPerStage > from.maxPulsesPerStage || to.minGapSec < from.minGapSec || to.maxPerHour > from.maxPerHour || to.maxPerDay > from.maxPerDay
        || to.stageJoinMs > from.stageJoinMs || (CapsCleared(from) && !CapsCleared(to));
  }

  /// SP-113 rule 2: a cap can rise only while the E-Stop is engaged, and only with a live checker task behind it, so
  /// the engaged state came from a kill path that can stop what the new cap allows.
  constexpr bool CapRiseAllowed(bool estopArmed, bool estopEngaged)
  {
    return estopArmed && estopEngaged;
  }

  /// Wire code of the E-Stop in the `Pong` field `softproxy_estop`: bit 0 armed, bit 1 engaged.
  constexpr uint8_t EStopWireCode(bool armed, bool engaged)
  {
    return static_cast<uint8_t>((armed ? 1u : 0u) | (engaged ? 2u : 0u));
  }

  /// Latched E-Stop state in memory that a warm restart (software reset, panic or watchdog) does not clear, as
  /// `RetainedLimiters` is. A warm restart comes back engaged. A cold boot fails the check and starts released.
  struct RetainedEStop {
    uint32_t magic;
    uint32_t engaged;
    uint32_t check;
  };

  static_assert(std::is_trivial_v<RetainedEStop> && std::is_standard_layout_v<RetainedEStop>, "retained state must need no construction");

  constexpr uint32_t kRetainedEStopMagic    = 0x53504553;  // "SPES"
  constexpr uint32_t kRetainedEStopEngaged  = 0xE5707A11u;
  constexpr uint32_t kRetainedEStopReleased = 0x1A5E0000u;

  constexpr uint32_t RetainedEStopCheck(uint32_t magic, uint32_t engaged)
  {
    return ~(magic ^ engaged);
  }

  inline void SealEStop(RetainedEStop& out, bool engaged)
  {
    out.magic   = kRetainedEStopMagic;
    out.engaged = engaged ? kRetainedEStopEngaged : kRetainedEStopReleased;
    out.check   = RetainedEStopCheck(out.magic, out.engaged);
  }

  /// Reads the sealed latch. Returns false and changes nothing when `in` is not sealed, which is the cold boot case.
  inline bool RestoreEStop(const RetainedEStop& in, bool& engaged)
  {
    if (in.magic != kRetainedEStopMagic || in.check != RetainedEStopCheck(in.magic, in.engaged)) {
      return false;
    }
    if (in.engaged != kRetainedEStopEngaged && in.engaged != kRetainedEStopReleased) {
      return false;
    }
    engaged = in.engaged == kRetainedEStopEngaged;
    return true;
  }
}  // namespace OpenShock::Softproxy

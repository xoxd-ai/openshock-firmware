#pragma once

// Decisions for the hub E-Stop as a Softproxy kill path (TIN-7125 SP-47, SP-113, SP-136, SP-156, SP-158 and SP-174, and
// R-SOFTPROXY-ESTOP-NC).
// src/EStopManager.cpp, src/CommandHandler.cpp, src/softproxy/SafetyCaps.cpp and src/config/Config.cpp apply them. No
// Arduino or ESP-IDF dependency, so a host compiler can test them (test/native/test_kill_path.cpp).

#include "softproxy/CapLimiter.h"

#include <cstdint>
#include <initializer_list>

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
  constexpr bool kChipIsEsp32 = true;
#else
  constexpr bool kChipIsEsp32 = false;
#endif
  constexpr bool kChipHasInputOnlyPins = kChipIsEsp32;

  /// The E-Stop input needs a pull-up, because an open contact must read high, which is stop. GPIO 34 to 39 of a plain
  /// ESP32 have no internal pull-up, so with nothing external an open contact floats and can read low, which is run.
  /// The fork has no way to declare an external one.
  constexpr bool HasInternalPullUp(int pin)
  {
    return !(kChipHasInputOnlyPins && pin >= 34 && pin <= 39);
  }

  /// Plain ESP32 pins the E-Stop refuses, whatever the board bypasses. The chip reads its strapping pins at reset: GPIO 0
  /// and 2 pick the boot mode, GPIO 12 the flash voltage, and GPIO 15 the boot log and, with GPIO 5, the SDIO timing. A
  /// normally closed contact holds its pin low, and an open one leaves it pulled high, so the button would decide how the
  /// hub boots. GPIO 12 is also the RF line on `Pishock-2023`. GPIO 6 to 11 connect the SPI flash.
  constexpr bool IsEStopReservedPin(int pin)
  {
    if (!kChipIsEsp32) {
      return false;
    }
    return pin == 0 || pin == 2 || pin == 5 || pin == 12 || pin == 15 || (pin >= 6 && pin <= 11);
  }

  /// What set the E-Stop latch (SP-156, SP-158). Only a press read on the E-Stop pin shows that the kill path works.
  enum class EStopLatchSource : uint8_t {
    Released,     // Not engaged.
    ColdBoot,     // Every cold boot starts engaged. Nothing was pressed.
    Gateway,      // The gateway `Trigger`. It says nothing about the pin.
    Button,       // After a full window of low samples, the checker task read a full window of high samples: a solid press.
    Unconfirmed,  // A high sample engaged it with no solid press after a measured run: noise, or a pin already open.
  };

  constexpr const char* EStopLatchName(EStopLatchSource source)
  {
    switch (source) {
      case EStopLatchSource::Released:
        return "released";
      case EStopLatchSource::ColdBoot:
        return "cold-boot";
      case EStopLatchSource::Gateway:
        return "gateway";
      case EStopLatchSource::Button:
        return "button";
      case EStopLatchSource::Unconfirmed:
        return "unconfirmed";
      default:
        return "unknown";
    }
  }

  /// Whether the E-Stop settings must stay put. A cold boot latch leaves them open, because every hub starts there and
  /// the first `estop pin` has to work. That is safe: no setting clears the latch, and this latch never allows a rise.
  constexpr bool EStopSettingsLocked(EStopLatchSource source, bool anyCapSet)
  {
    return anyCapSet || source == EStopLatchSource::Button || source == EStopLatchSource::Gateway || source == EStopLatchSource::Unconfirmed;
  }

  /// Whether a set E-Stop pin is unusable: it is the RF transmitter, the transmitter's board default (the hub falls
  /// back to it), a board LED, a reserved strapping or flash pin, or has no pull-up. An unset pin (below 0) never
  /// conflicts. It leaves the E-Stop off.
  constexpr bool EStopPinConflicts(int estopPin, int txPin)
  {
    if (estopPin < 0) {
      return false;
    }
    return estopPin == txPin || estopPin == kBoardRfTxPin || estopPin == kBoardLedPin || estopPin == kBoardRgbLedPin || IsEStopReservedPin(estopPin) || !HasInternalPullUp(estopPin);
  }

#ifdef SOFTPROXY_PISHOCK_BUILD
  // The mutation run lists K29, dropping `estopPin == kBoardRfTxPin` above, as equivalent. That holds only while the
  // board RF TX pin is reserved, as GPIO 12 and 15 are, so `IsEStopReservedPin` refuses it anyway. A Softproxy build on
  // another RF TX pin fails here and puts K29 back in the run.
  static_assert(IsEStopReservedPin(kBoardRfTxPin), "K29 is equivalent only where the board RF TX pin is a reserved E-Stop pin");
#endif

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

  /// Whether `to` lets through anything `from` refuses. Lower `minGapSec` is a rise, since it admits sooner. Any change
  /// of `stageJoinMs` is a rise: a shorter window can turn a command refused `over-pulses` into a new stage.
  constexpr bool CapsRise(const ChannelCaps& from, const ChannelCaps& to)
  {
    // Clearing admits nothing, even though `minGapSec` falls to 0.
    if (CapsCleared(to)) {
      return false;
    }
    return to.maxIntensityPct > from.maxIntensityPct || to.maxPulseMs > from.maxPulseMs || to.maxPulsesPerStage > from.maxPulsesPerStage || to.minGapSec < from.minGapSec || to.maxPerHour > from.maxPerHour || to.maxPerDay > from.maxPerDay
        || to.stageJoinMs != from.stageJoinMs || (CapsCleared(from) && !CapsCleared(to));
  }

  /// SP-113 rule 2 with SP-156: a cap can rise only while a button press holds the E-Stop engaged, with a live checker
  /// task behind it. A gateway `Trigger` or a cold boot can engage a hub whose button was never wired, so neither counts.
  /// `Unconfirmed` does not count either: one high sample can be noise or an open pin.
  constexpr bool CapRiseAllowed(bool estopArmed, EStopLatchSource source)
  {
    return estopArmed && source == EStopLatchSource::Button;
  }

  /// Wire code of the E-Stop in the `Pong` field `softproxy_estop`: bit 0 armed, bit 1 engaged.
  constexpr uint8_t EStopWireCode(bool armed, bool engaged)
  {
    return static_cast<uint8_t>((armed ? 1u : 0u) | (engaged ? 2u : 0u));
  }

  /// Latched E-Stop state in memory that a warm restart (software reset, panic or watchdog) does not clear, as
  /// `RetainedLimiters` is. A warm restart comes back as it was, with the same source. A cold boot starts engaged
  /// (SP-158). The check rejects lost memory, but a power loss can keep this record, so `EStopManager::Init` also reads
  /// the reset reason.
  struct RetainedEStop {
    uint32_t magic;
    uint32_t latch;
    uint32_t check;
  };

  static_assert(std::is_trivial_v<RetainedEStop> && std::is_standard_layout_v<RetainedEStop>, "retained state must need no construction");

  constexpr uint32_t kRetainedEStopMagic       = 0x53504532;  // "SPE2"
  constexpr uint32_t kRetainedEStopReleased    = 0x1A5E0000u;
  constexpr uint32_t kRetainedEStopColdBoot    = 0xC01DB007u;
  constexpr uint32_t kRetainedEStopGateway     = 0x6A7E3A11u;
  constexpr uint32_t kRetainedEStopButton      = 0xB0770411u;
  constexpr uint32_t kRetainedEStopUnconfirmed = 0x55C0FF1Du;

  constexpr uint32_t RetainedEStopCode(EStopLatchSource source)
  {
    switch (source) {
      case EStopLatchSource::Released:
        return kRetainedEStopReleased;
      case EStopLatchSource::Gateway:
        return kRetainedEStopGateway;
      case EStopLatchSource::Button:
        return kRetainedEStopButton;
      case EStopLatchSource::Unconfirmed:
        return kRetainedEStopUnconfirmed;
      case EStopLatchSource::ColdBoot:
      default:
        return kRetainedEStopColdBoot;
    }
  }

  constexpr uint32_t RetainedEStopCheck(uint32_t magic, uint32_t latch)
  {
    return ~(magic ^ latch);
  }

  inline void SealEStop(RetainedEStop& out, EStopLatchSource source)
  {
    out.magic = kRetainedEStopMagic;
    out.latch = RetainedEStopCode(source);
    out.check = RetainedEStopCheck(out.magic, out.latch);
  }

  /// Reads the sealed latch. Returns false and changes nothing when `in` is not sealed, which is the cold boot case.
  inline bool RestoreEStop(const RetainedEStop& in, EStopLatchSource& source)
  {
    if (in.magic != kRetainedEStopMagic || in.check != RetainedEStopCheck(in.magic, in.latch)) {
      return false;
    }
    for (EStopLatchSource candidate : {EStopLatchSource::Released, EStopLatchSource::ColdBoot, EStopLatchSource::Gateway, EStopLatchSource::Button, EStopLatchSource::Unconfirmed}) {
      if (in.latch == RetainedEStopCode(candidate)) {
        source = candidate;
        return true;
      }
    }
    return false;
  }
}  // namespace OpenShock::Softproxy

#pragma once

// Softproxy cap enforcement for the xoxd-ai fork of the OpenShock firmware.
//
// Softproxy's alarm service checks every actuator step against per-channel caps before it sends it
// (Softproxy ADR-0008 decision 6, Device.dhall server check 3). This fork checks the same caps again
// on the hub, so a step that the server should have refused still cannot reach the shocker.
//
// The caps mirror Softproxy's `Device.Caps` record. No cap value lives in this source: every cap
// defaults to 0, and a channel with any cap at 0 refuses every command. The operator sets the values
// over the local serial port at an attended session (`safetycaps`), and they are stored on the hub.
//
// This header holds only the decision logic. It has no Arduino, FreeRTOS or ESP-IDF dependency, so a
// host compiler can test it (test/native/test_cap_limiter.cpp).

#include <array>
#include <cstddef>
#include <cstdint>

namespace OpenShock::Softproxy {
  /// Caps for one channel (shock or vibrate). Field names follow Softproxy `Device.Caps`.
  struct ChannelCaps {
    uint8_t maxIntensityPct;     // Highest intensity a command may carry, in percent.
    uint16_t maxPulseMs;         // Longest duration a command may carry, in milliseconds.
    uint16_t maxPulsesPerStage;  // Most commands in one stage (see `stageJoinMs`).
    uint32_t minGapSec;          // Least time between the starts of two stages.
    uint16_t maxPerHour;         // Most stage starts in any 60 minutes.
    uint16_t maxPerDay;          // Most stage starts in any 24 hours.
    uint32_t stageJoinMs;        // Firmware only. A command that starts within this many ms after the previous pulse of the open stage ends joins that stage. 0 makes every command its own stage.
  };

  /// Every Softproxy cap is above 0, so the operator has set it (Softproxy `capsSet`). `stageJoinMs` may be 0.
  constexpr bool CapsSet(const ChannelCaps& c)
  {
    return c.maxIntensityPct > 0 && c.maxPulseMs > 0 && c.maxPulsesPerStage > 0 && c.minGapSec > 0 && c.maxPerHour > 0 && c.maxPerDay > 0;
  }

  /// Caps agree with each other (Softproxy `capsCoherent`). Units and arithmetic only, no ceiling on any value.
  constexpr bool CapsCoherent(const ChannelCaps& c)
  {
    return c.maxIntensityPct <= 100 && c.maxPerHour <= c.maxPerDay && static_cast<uint64_t>(c.minGapSec) * c.maxPerHour <= 3600;
  }

  /// Every field is 0: the channel is cleared, which refuses every command.
  constexpr bool CapsCleared(const ChannelCaps& c)
  {
    return c.maxIntensityPct == 0 && c.maxPulseMs == 0 && c.maxPulsesPerStage == 0 && c.minGapSec == 0 && c.maxPerHour == 0 && c.maxPerDay == 0 && c.stageJoinMs == 0;
  }

  enum class Verdict : uint8_t {
    Allow,          // Within every cap. The command was recorded.
    NotCapped,      // Stop, Sound and Light carry no Softproxy cap.
    CapsNotSet,     // A cap is 0 or the caps are incoherent.
    BootHold,       // Less than `minGapSec` since boot. History from before a restart is unknown.
    OverIntensity,  // Intensity above `maxIntensityPct`.
    OverPulseMs,    // Duration above `maxPulseMs`.
    OverPulses,     // The open stage already holds `maxPulsesPerStage` commands.
    Gap,            // Less than `minGapSec` since the previous stage started.
    Hourly,         // `maxPerHour` stages started in the last 60 minutes.
    Daily,          // `maxPerDay` stages started in the last 24 hours.
  };

  constexpr const char* VerdictName(Verdict v)
  {
    switch (v) {
      case Verdict::Allow:
        return "allow";
      case Verdict::NotCapped:
        return "not-capped";
      case Verdict::CapsNotSet:
        return "caps-not-set";
      case Verdict::BootHold:
        return "boot-hold";
      case Verdict::OverIntensity:
        return "over-intensity";
      case Verdict::OverPulseMs:
        return "over-pulse-ms";
      case Verdict::OverPulses:
        return "over-pulses";
      case Verdict::Gap:
        return "gap";
      case Verdict::Hourly:
        return "hourly";
      case Verdict::Daily:
        return "daily";
      default:
        return "unknown";
    }
  }

  constexpr bool Admitted(Verdict v) { return v == Verdict::Allow || v == Verdict::NotCapped; }

  /// Counts events in a sliding window with fixed buckets. A bucket counts while any part of it lies
  /// inside the window, so the count can overstate by up to one bucket and never understates. An
  /// overstatement only refuses sooner.
  template <uint32_t WindowMs, uint32_t BucketMs>
  class WindowCounter {
    static_assert(BucketMs > 0 && WindowMs % BucketMs == 0, "the window must be a whole number of buckets");
    static constexpr std::size_t N = WindowMs / BucketMs + 1;

  public:
    WindowCounter() { Reset(); }

    void Reset()
    {
      m_index.fill(-1);
      m_count.fill(0);
    }

    /// Events with `nowMs - t < WindowMs`, plus at most one bucket more.
    uint32_t Count(int64_t nowMs) const
    {
      const int64_t current = nowMs / BucketMs;
      uint32_t sum          = 0;
      for (std::size_t i = 0; i < N; ++i) {
        if (m_index[i] >= 0 && m_index[i] <= current && m_index[i] > current - static_cast<int64_t>(N)) {
          sum += m_count[i];
        }
      }
      return sum;
    }

    void Add(int64_t nowMs)
    {
      const int64_t current = nowMs / BucketMs;
      const std::size_t slot = static_cast<std::size_t>(current % static_cast<int64_t>(N));
      if (m_index[slot] != current) {
        m_index[slot] = current;
        m_count[slot] = 0;
      }
      if (m_count[slot] < UINT16_MAX) {
        ++m_count[slot];
      }
    }

  private:
    std::array<int64_t, N> m_index;
    std::array<uint16_t, N> m_count;
  };

  /// Admission state for one channel. `nowMs` is milliseconds since boot (`OpenShock::millis()`).
  ///
  /// A stage starts with a command that does not join the open stage. Gap, hourly and daily limits
  /// apply to stage starts, as the Softproxy server counts one request per stage. Counters live in RAM
  /// and restart empty after a reboot, so the boot hold refuses until `minGapSec` has passed since boot.
  class ChannelLimiter {
  public:
    /// Checks one command against the caps. On `Allow` it records the command before returning.
    Verdict Admit(const ChannelCaps& caps, uint8_t intensity, uint16_t durationMs, int64_t nowMs)
    {
      if (!CapsSet(caps) || !CapsCoherent(caps)) {
        return Verdict::CapsNotSet;
      }
      if (intensity > caps.maxIntensityPct) {
        return Verdict::OverIntensity;
      }
      if (durationMs > caps.maxPulseMs) {
        return Verdict::OverPulseMs;
      }

      const int64_t gapMs = static_cast<int64_t>(caps.minGapSec) * 1000;
      if (nowMs < gapMs) {
        return Verdict::BootHold;
      }

      const bool joins = m_hasStage && caps.stageJoinMs > 0 && nowMs >= m_stageStartMs && nowMs <= m_lastPulseEndMs + static_cast<int64_t>(caps.stageJoinMs);
      if (joins) {
        if (m_pulsesInStage >= caps.maxPulsesPerStage) {
          return Verdict::OverPulses;
        }
        ++m_pulsesInStage;
        m_lastPulseEndMs = nowMs + durationMs;
        return Verdict::Allow;
      }

      if (m_hasStage && nowMs - m_stageStartMs < gapMs) {
        return Verdict::Gap;
      }
      if (m_hour.Count(nowMs) >= caps.maxPerHour) {
        return Verdict::Hourly;
      }
      if (m_day.Count(nowMs) >= caps.maxPerDay) {
        return Verdict::Daily;
      }

      m_hasStage       = true;
      m_stageStartMs   = nowMs;
      m_lastPulseEndMs = nowMs + durationMs;
      m_pulsesInStage  = 1;
      m_hour.Add(nowMs);
      m_day.Add(nowMs);
      return Verdict::Allow;
    }

    uint32_t StagesLastHour(int64_t nowMs) const { return m_hour.Count(nowMs); }
    uint32_t StagesLastDay(int64_t nowMs) const { return m_day.Count(nowMs); }

  private:
    bool m_hasStage          = false;
    int64_t m_stageStartMs   = 0;
    int64_t m_lastPulseEndMs = 0;
    uint16_t m_pulsesInStage = 0;
    WindowCounter<3'600'000, 60'000> m_hour;   // 1 minute buckets.
    WindowCounter<86'400'000, 900'000> m_day;  // 15 minute buckets.
  };
}  // namespace OpenShock::Softproxy

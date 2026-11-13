#pragma once

// Softproxy cap enforcement for the xoxd-ai fork of the OpenShock firmware.
//
// Softproxy's alarm service checks every actuator step against per-channel caps before it sends it
// (Softproxy ADR-0008 decision 6, Device.dhall server check 3). This fork checks the same caps again
// on the hub as a second line. The limits and their gaps are listed in SOFTPROXY.md.
//
// The caps mirror Softproxy's `Device.Caps` record. No cap value lives in this source: every cap
// defaults to 0, and a channel with any cap at 0 refuses every command. The operator sets the values
// over the local serial port at an attended session (`safetycaps`), and they are stored on the hub.
//
// This header holds only the decision logic. It has no Arduino, FreeRTOS or ESP-IDF dependency, so a
// host compiler can test it (test/native/test_cap_limiter.cpp).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

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

  /// Caps agree with each other (Softproxy `capsCoherent`). Units and arithmetic only, no ceiling on any value. A
  /// pulse may not outlast the gap: the radio overwrites a shocker's running sequence with each new command, so a pulse
  /// longer than the gap would let stages started every `minGapSec` join into one unbroken output (review H1).
  constexpr bool CapsCoherent(const ChannelCaps& c)
  {
    return c.maxIntensityPct <= 100 && c.maxPerHour <= c.maxPerDay && static_cast<uint64_t>(c.minGapSec) * c.maxPerHour <= 3600 && static_cast<uint64_t>(c.maxPulseMs) <= static_cast<uint64_t>(c.minGapSec) * 1000;
  }

  /// Output that resumes less than this many ms after the channel's previous output ended continues the same run. It is
  /// the RF task's terminator time (`kTerminatorDurationMs` in src/radio/RFTransmitter.cpp): a shocker's sequence stays
  /// in the task, sending the stop frame, for 300 ms after its end, and a command for that shocker in those 300 ms
  /// overwrites it. Only output that stays off for the whole terminator ends a run.
  constexpr int64_t kOutputRunBreakMs = 300;

  /// Longest radio frame of any shocker model the fork encodes, in ms, rounded up (review N2). The RF task writes whole
  /// frames and starts one whenever a sequence's end still lies ahead, so the last frame of a command can run up to one
  /// frame past its end. The value bounds every frame: for each model, each item of the frame at the longest value any
  /// id, type and intensity gives it, summed. The largest is the Petrainer's, 72200 us at the RF task's 1000 ns tick
  /// (src/radio/rmt/PetrainerEncoder.cpp), against 69200 us for its longest real frame. test/native/test_command_paths.cpp
  /// rebuilds the bound from every encoder and the tick, and fails if this constant drifts from it.
  constexpr int64_t kMaxRadioFrameMs = 73;

  /// Output time one admitted command counts in its run: its duration plus one frame, since the radio can send up to
  /// one frame past the end. A command of 0 ms sends no payload, only the stop frame, and counts nothing.
  constexpr int64_t CountedOutputMs(uint16_t durationMs)
  {
    return durationMs > 0 ? static_cast<int64_t>(durationMs) + kMaxRadioFrameMs : 0;
  }

  /// Most output time one run may hold: one full stage, `maxPulsesPerStage` pulses of `maxPulseMs`, each counted with
  /// its frame as `CountedOutputMs` counts it (review H1, N2). A stage of full pulses whose frames do not overlap the
  /// next pulse reaches it exactly.
  constexpr int64_t OnTimeCeilingMs(const ChannelCaps& c)
  {
    return CountedOutputMs(c.maxPulseMs) * c.maxPulsesPerStage;
  }

  /// Every field is 0: the channel is cleared, which refuses every command.
  constexpr bool CapsCleared(const ChannelCaps& c)
  {
    return c.maxIntensityPct == 0 && c.maxPulseMs == 0 && c.maxPulsesPerStage == 0 && c.minGapSec == 0 && c.maxPerHour == 0 && c.maxPerDay == 0 && c.stageJoinMs == 0;
  }

  /// After a cold boot the hub has no history, so a channel refuses until its clock reaches this many ms.
  /// It is the longest of the gap and the even spacing of the hourly and daily caps, so every power cycle
  /// costs a full hold before the next stage. Coherent caps never let the gap exceed the hourly spacing.
  constexpr int64_t ColdBootHoldMs(const ChannelCaps& c)
  {
    const int64_t gapMs  = static_cast<int64_t>(c.minGapSec) * 1000;
    const int64_t hourMs = c.maxPerHour > 0 ? (3'600'000 + c.maxPerHour - 1) / c.maxPerHour : 0;
    const int64_t dayMs  = c.maxPerDay > 0 ? (86'400'000 + c.maxPerDay - 1) / c.maxPerDay : 0;
    int64_t hold         = gapMs;
    if (hourMs > hold) {
      hold = hourMs;
    }
    if (dayMs > hold) {
      hold = dayMs;
    }
    return hold;
  }

  enum class Verdict : uint8_t {
    Allow,          // Within every cap. The command was recorded.
    NotCapped,      // Stop, Sound and Light carry no Softproxy cap.
    CapsNotSet,     // A cap is 0 or the caps are incoherent.
    BootHold,       // The hub clock is below `ColdBootHoldMs`. History from before a cold boot is unknown.
    OverIntensity,  // Intensity above `maxIntensityPct`.
    OverPulseMs,    // Duration above `maxPulseMs`.
    OverPulses,     // The open stage already holds `maxPulsesPerStage` commands.
    Gap,            // Less than `minGapSec` since the previous stage started.
    Hourly,         // `maxPerHour` stages started in the last 60 minutes.
    Daily,          // `maxPerDay` stages started in the last 24 hours.
    EStopEngaged,   // The E-Stop is engaged. Any command refuses, capped or not.
    EStopNotArmed,  // No live E-Stop checker task on a valid pin. Shock and vibrate refuse (include/softproxy/KillPath.h).
    OverOnTime,     // The command would take the channel's output run past `OnTimeCeilingMs`.
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
      case Verdict::EStopEngaged:
        return "estop-engaged";
      case Verdict::EStopNotArmed:
        return "estop-not-armed";
      case Verdict::OverOnTime:
        return "over-on-time";
      default:
        return "unknown";
    }
  }

  constexpr bool Admitted(Verdict v)
  {
    return v == Verdict::Allow || v == Verdict::NotCapped;
  }

  /// Counts events in a sliding window with fixed buckets. A bucket counts while any part of it lies
  /// inside the window, so the count can overstate by up to one bucket and never understates. An
  /// overstatement only refuses sooner.
  template<uint32_t WindowMs, uint32_t BucketMs>
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
        const int64_t index = m_index[i];
        if (index >= 0 && index <= current && index > current - static_cast<int64_t>(N)) {
          sum += m_count[i];
        }
      }
      return sum;
    }

    void Add(int64_t nowMs)
    {
      const int64_t current  = nowMs / BucketMs;
      const std::size_t slot = static_cast<std::size_t>(current % static_cast<int64_t>(N));
      if (m_index[slot] != current) {
        m_index[slot] = static_cast<int32_t>(current);
        m_count[slot] = 0;
      }
      if (m_count[slot] < UINT16_MAX) {
        ++m_count[slot];
      }
    }

  private:
    std::array<int32_t, N> m_index;  // Bucket number, or -1 when empty. 32 bits last over 4000 years of 1 minute buckets.
    std::array<uint16_t, N> m_count;
  };

  /// Admission state for one channel. `nowMs` is the hub clock in milliseconds. It starts at 0 on a cold
  /// boot and never runs ahead of real time (see `RetainedLimiters`).
  ///
  /// A stage starts with a command that does not join the open stage. Gap, hourly and daily limits
  /// apply to stage starts, as the Softproxy server counts one request per stage. After a cold boot the
  /// counters start empty, so the boot hold refuses until the clock reaches `ColdBootHoldMs`.
  ///
  /// The limiter also bounds continuous output, whatever the stages (review H1). A run is the channel's output from
  /// a start until it stays off for `kOutputRunBreakMs`. Each admitted command counts as output from `nowMs` for
  /// `CountedOutputMs`, its duration plus one radio frame, and a time two commands overlap counts once. A command that
  /// would bring the run's output time above `OnTimeCeilingMs` refuses with `OverOnTime`. The run is kept per channel,
  /// not per shocker, so output to two shockers counts as one run, which can only refuse sooner per shocker.
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

      if (nowMs < ColdBootHoldMs(caps)) {
        return Verdict::BootHold;
      }

      const int64_t gapMs = static_cast<int64_t>(caps.minGapSec) * 1000;

      // The output time the run would hold with this command (see the class comment).
      const int64_t countedMs  = CountedOutputMs(durationMs);
      const int64_t endMs      = nowMs + countedMs;
      const bool continuesRun  = m_hasRun && nowMs < m_runEndMs + kOutputRunBreakMs;
      const int64_t addedMs    = continuesRun ? std::max<int64_t>(0, endMs - std::max(nowMs, m_runEndMs)) : countedMs;
      const int64_t runOnMs    = (continuesRun ? m_runOnMs : 0) + addedMs;
      const int64_t runEndMs   = continuesRun ? std::max(m_runEndMs, endMs) : endMs;
      const bool overOnTime    = runOnMs > OnTimeCeilingMs(caps);

      const bool joins = m_hasStage && caps.stageJoinMs > 0 && nowMs >= m_stageStartMs && nowMs <= m_lastPulseEndMs + static_cast<int64_t>(caps.stageJoinMs);
      if (joins) {
        if (m_pulsesInStage >= caps.maxPulsesPerStage) {
          return Verdict::OverPulses;
        }
        if (overOnTime) {
          return Verdict::OverOnTime;
        }
        recordRun(runOnMs, runEndMs);
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
      if (overOnTime) {
        return Verdict::OverOnTime;
      }

      recordRun(runOnMs, runEndMs);
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
    void recordRun(int64_t runOnMs, int64_t runEndMs)
    {
      m_hasRun   = true;
      m_runOnMs  = runOnMs;
      m_runEndMs = runEndMs;
    }

    bool m_hasStage          = false;
    int64_t m_stageStartMs   = 0;
    int64_t m_lastPulseEndMs = 0;
    uint16_t m_pulsesInStage = 0;
    bool m_hasRun            = false;
    int64_t m_runOnMs        = 0;  // Output time in the current run, overlaps counted once.
    int64_t m_runEndMs       = 0;  // Latest end of any output in the current run.
    WindowCounter<3'600'000, 60'000> m_hour;   // 1 minute buckets.
    WindowCounter<86'400'000, 900'000> m_day;  // 15 minute buckets.
  };

  static_assert(std::is_trivially_copyable_v<ChannelLimiter>, "the limiter state is copied byte for byte into retained memory");

  /// Limiter state and hub clock kept in memory that a warm restart (software reset, panic or watchdog)
  /// does not clear. The hub restores it at boot, so a restart neither empties the counters nor resets
  /// the clock. A cold boot fails the checksum and starts empty with the clock at 0.
  ///
  /// `clockMs` is the hub clock at the last seal. After a warm restart the clock resumes there and adds
  /// the time since boot, so the time before the restart and the downtime count as not passed. The clock
  /// therefore never runs ahead of real time, which can only refuse sooner.
  ///
  /// The type has no constructor, so placing it in no-init memory runs no code at boot.
  struct RetainedLimiters {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    int64_t clockMs;
    alignas(ChannelLimiter) unsigned char shock[sizeof(ChannelLimiter)];
    alignas(ChannelLimiter) unsigned char vibrate[sizeof(ChannelLimiter)];
    uint32_t checksum;
  };

  static_assert(std::is_trivial_v<RetainedLimiters> && std::is_standard_layout_v<RetainedLimiters>, "retained state must need no construction");
  static_assert(sizeof(RetainedLimiters) <= UINT16_MAX, "the size field is 16 bits");

  constexpr uint32_t kRetainedMagic   = 0x53505254;  // "SPRT"
  constexpr uint16_t kRetainedVersion = 2;  // 2 added the output run to `ChannelLimiter`. A version 1 record reads as a cold boot.

  /// FNV-1a over every byte before `checksum`.
  inline uint32_t RetainedChecksum(const RetainedLimiters& r)
  {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&r);
    uint32_t hash              = 2'166'136'261u;
    for (std::size_t i = 0; i < offsetof(RetainedLimiters, checksum); ++i) {
      hash ^= bytes[i];
      hash *= 16'777'619u;
    }
    return hash;
  }

  /// Writes the clock and both limiters into `out`, padding included, then seals it with the checksum. It writes in
  /// place because the struct is about 2 kilobytes and this runs on `main_app` for every command. A reset part way
  /// through fails the checksum, which reads as a cold boot.
  inline void SealRetained(RetainedLimiters& out, int64_t clockMs, const ChannelLimiter& shock, const ChannelLimiter& vibrate)
  {
    std::memset(&out, 0, sizeof(out));
    out.magic   = kRetainedMagic;
    out.version = kRetainedVersion;
    out.size    = static_cast<uint16_t>(sizeof(RetainedLimiters));
    out.clockMs = clockMs;
    std::memcpy(out.shock, &shock, sizeof(ChannelLimiter));
    std::memcpy(out.vibrate, &vibrate, sizeof(ChannelLimiter));
    out.checksum = RetainedChecksum(out);
  }

  /// Restores the clock and both limiters when `in` is sealed by this layout. Returns false and changes
  /// nothing otherwise, which is the cold boot case.
  inline bool RestoreRetained(const RetainedLimiters& in, int64_t& clockMs, ChannelLimiter& shock, ChannelLimiter& vibrate)
  {
    if (in.magic != kRetainedMagic || in.version != kRetainedVersion || in.size != static_cast<uint16_t>(sizeof(RetainedLimiters)) || in.checksum != RetainedChecksum(in) || in.clockMs < 0) {
      return false;
    }
    clockMs = in.clockMs;
    std::memcpy(static_cast<void*>(&shock), in.shock, sizeof(ChannelLimiter));
    std::memcpy(static_cast<void*>(&vibrate), in.vibrate, sizeof(ChannelLimiter));
    return true;
  }
}  // namespace OpenShock::Softproxy

#pragma once

// Report of the Softproxy cap refusals (include/softproxy/CapLimiter.h), so a refused step reaches
// Softproxy instead of reading as an actuation (TIN-7545). Each refusal leaves two traces:
//
// - one hub log line, `softproxy-refusal seq=<n> channel=<shock|vibrate> verdict=<name>`, on the serial log;
// - the gateway heartbeat: the hub's `Pong` carries `softproxy_cap_refusals` (count since boot) and
//   `softproxy_last_refusal` (wire code of the latest refusal). A receiver that sees the count rise between
//   two heartbeats knows that many steps were refused. A lower `uptime` marks a reboot, which restarts the count.
//
// No Arduino, FreeRTOS or ESP-IDF dependency, so the host test can check it.

#include "softproxy/CapLimiter.h"

#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace OpenShock::Softproxy {
  // `softproxy_last_refusal` carries the verdict as its enum value. These pin the order, since it is now a wire format.
  static_assert(static_cast<uint8_t>(Verdict::Allow) == 0, "wire code 0 means no refusal yet");
  static_assert(static_cast<uint8_t>(Verdict::NotCapped) == 1);
  static_assert(static_cast<uint8_t>(Verdict::CapsNotSet) == 2);
  static_assert(static_cast<uint8_t>(Verdict::BootHold) == 3);
  static_assert(static_cast<uint8_t>(Verdict::OverIntensity) == 4);
  static_assert(static_cast<uint8_t>(Verdict::OverPulseMs) == 5);
  static_assert(static_cast<uint8_t>(Verdict::OverPulses) == 6);
  static_assert(static_cast<uint8_t>(Verdict::Gap) == 7);
  static_assert(static_cast<uint8_t>(Verdict::Hourly) == 8);
  static_assert(static_cast<uint8_t>(Verdict::Daily) == 9);

  /// One consistent reading of the report.
  struct RefusalSnapshot {
    uint32_t count;     // Refusals since boot, saturating at `RefusalReport::kMaxCount`.
    uint8_t lastCode;   // Wire code of the latest refusal's verdict. 0 before the first refusal.
  };

  /// Counts refusals without a lock. The count and the last code share one atomic word, so a reader never
  /// pairs a count with the code of another refusal.
  class RefusalReport {
  public:
    static constexpr uint32_t kMaxCount = 0x00FF'FFFFu;

    /// Records one refusal and returns its sequence number, starting at 1. An admitted verdict is not a
    /// refusal: nothing is recorded and the result is 0. At `kMaxCount` the count stops and the code still updates.
    uint32_t Record(Verdict v)
    {
      if (Admitted(v)) {
        return 0;
      }
      const uint32_t code = static_cast<uint8_t>(v);
      uint32_t current    = m_word.load(std::memory_order_relaxed);
      uint32_t next;
      do {
        uint32_t count = current >> 8;
        if (count < kMaxCount) {
          ++count;
        }
        next = (count << 8) | code;
      } while (!m_word.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_relaxed));
      return next >> 8;
    }

    RefusalSnapshot Snapshot() const
    {
      const uint32_t word = m_word.load(std::memory_order_acquire);
      return RefusalSnapshot {.count = word >> 8, .lastCode = static_cast<uint8_t>(word & 0xFFu)};
    }

  private:
    std::atomic<uint32_t> m_word {0};
  };

  /// Verdict name for a wire code, or "none" for 0 and "unknown" past the last verdict.
  constexpr const char* RefusalCodeName(uint8_t code)
  {
    if (code == 0) {
      return "none";
    }
    if (code > static_cast<uint8_t>(Verdict::Daily)) {
      return "unknown";
    }
    return VerdictName(static_cast<Verdict>(code));
  }

  /// Buffer size that holds every line `FormatRefusalLine` writes.
  constexpr std::size_t kRefusalLineSize = 80;

  /// Writes the hub log line for one refusal. Returns what `snprintf` returns.
  inline int FormatRefusalLine(char* out, std::size_t size, uint32_t seq, const char* channel, Verdict v)
  {
    return std::snprintf(out, size, "softproxy-refusal seq=%" PRIu32 " channel=%s verdict=%s", seq, channel != nullptr ? channel : "unknown", VerdictName(v));
  }
}  // namespace OpenShock::Softproxy

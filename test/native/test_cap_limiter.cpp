// Host test for include/softproxy/CapLimiter.h. It needs no board and no PlatformIO:
//
//   g++ -std=c++20 -Wall -Wextra -Werror -Iinclude test/native/test_cap_limiter.cpp -o cap_limiter_test && ./cap_limiter_test
//
// Every cap below is a synthetic test value chosen for the arithmetic. None is a body-safety value.

#include "softproxy/CapLimiter.h"
#include "softproxy/Lockout.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace OpenShock::Softproxy;

static int s_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++s_failures;                                               \
    }                                                             \
  } while (0)

#define CHECK_VERDICT(actual, expected)                                                                          \
  do {                                                                                                           \
    Verdict a__ = (actual);                                                                                      \
    if (a__ != (expected)) {                                                                                     \
      std::printf("FAIL %s:%d: got %s, want %s\n", __FILE__, __LINE__, VerdictName(a__), VerdictName(expected)); \
      ++s_failures;                                                                                              \
    }                                                                                                            \
  } while (0)

static const int64_t kSec  = 1000;
static const int64_t kMin  = 60 * kSec;
static const int64_t kHour = 60 * kMin;
static const int64_t kDay  = 24 * kHour;

// A hub clock past every cold boot hold the fixtures produce. A multiple of both bucket sizes.
static const int64_t kWarm = 2 * kDay;

// Synthetic caps: 10 percent, 100 ms, 1 pulse, 1 s gap, 3 per hour, 5 per day.
static ChannelCaps fixture()
{
  return ChannelCaps {.maxIntensityPct = 10, .maxPulseMs = 100, .maxPulsesPerStage = 1, .minGapSec = 1, .maxPerHour = 3, .maxPerDay = 5, .stageJoinMs = 0};
}

static void zeroCapsRefuse()
{
  ChannelLimiter l;
  ChannelCaps zero = {};
  CHECK(!CapsSet(zero));
  CHECK(CapsCleared(zero));
  CHECK_VERDICT(l.Admit(zero, 0, 0, kHour), Verdict::CapsNotSet);

  // Any single cap at 0 refuses.
  for (int field = 0; field < 6; ++field) {
    ChannelCaps c = fixture();
    switch (field) {
      case 0:
        c.maxIntensityPct = 0;
        break;
      case 1:
        c.maxPulseMs = 0;
        break;
      case 2:
        c.maxPulsesPerStage = 0;
        break;
      case 3:
        c.minGapSec = 0;
        break;
      case 4:
        c.maxPerHour = 0;
        break;
      default:
        c.maxPerDay = 0;
        break;
    }
    ChannelLimiter fresh;
    CHECK_VERDICT(fresh.Admit(c, 0, 0, kHour), Verdict::CapsNotSet);
  }
}

static void incoherentCapsRefuse()
{
  ChannelCaps over     = fixture();
  over.maxIntensityPct = 101;
  CHECK(!CapsCoherent(over));

  ChannelCaps hourOverDay = fixture();
  hourOverDay.maxPerHour  = 6;
  CHECK(!CapsCoherent(hourOverDay));

  ChannelCaps gapTooWide = fixture();
  gapTooWide.minGapSec   = 1201;  // 1201 * 3 > 3600
  CHECK(!CapsCoherent(gapTooWide));

  ChannelLimiter l;
  CHECK_VERDICT(l.Admit(over, 1, 1, kHour), Verdict::CapsNotSet);
  CHECK_VERDICT(l.Admit(hourOverDay, 1, 1, kHour), Verdict::CapsNotSet);
  CHECK_VERDICT(l.Admit(gapTooWide, 1, 1, kHour), Verdict::CapsNotSet);
  CHECK(CapsCoherent(fixture()));
}

static void requestCaps()
{
  ChannelCaps c = fixture();
  ChannelLimiter l;
  CHECK_VERDICT(l.Admit(c, 11, 100, kWarm), Verdict::OverIntensity);
  CHECK_VERDICT(l.Admit(c, 10, 101, kWarm), Verdict::OverPulseMs);
  CHECK_VERDICT(l.Admit(c, 10, 100, kWarm), Verdict::Allow);
}

static void coldBootHoldSpacing()
{
  // 5 per day spaces stages 17280 s apart, longer than the 1200 s hourly spacing and the 1 s gap.
  CHECK(ColdBootHoldMs(fixture()) == kDay / 5);

  // The hourly spacing rounds up: 3600000 / 7 is 514285.7.
  ChannelCaps hourly = fixture();
  hourly.maxPerHour  = 7;
  hourly.maxPerDay   = 1000;
  CHECK(ColdBootHoldMs(hourly) == 514'286);

  // Coherent caps never let the gap exceed the hourly spacing.
  ChannelCaps wide = fixture();
  wide.minGapSec   = 1200;
  wide.maxPerHour  = 3;
  wide.maxPerDay   = 1000;
  CHECK(CapsCoherent(wide));
  CHECK(ColdBootHoldMs(wide) == 1200 * kSec);
}

static void bootHold()
{
  ChannelCaps c = fixture();
  c.minGapSec   = 30;
  c.maxPerHour  = 3;
  c.maxPerDay   = 72;  // 1200 s daily spacing, equal to the hourly spacing
  ChannelLimiter l;
  CHECK_VERDICT(l.Admit(c, 1, 1, 0), Verdict::BootHold);
  CHECK_VERDICT(l.Admit(c, 1, 1, 30 * kSec), Verdict::BootHold);  // past the gap, inside the hourly spacing
  CHECK_VERDICT(l.Admit(c, 1, 1, 1200 * kSec - 1), Verdict::BootHold);
  CHECK_VERDICT(l.Admit(c, 1, 1, 1200 * kSec), Verdict::Allow);
}

static void gapBetweenStages()
{
  ChannelCaps c = fixture();
  c.minGapSec   = 10;
  ChannelLimiter l;
  const int64_t t0 = kWarm;
  CHECK_VERDICT(l.Admit(c, 1, 1, t0), Verdict::Allow);
  CHECK_VERDICT(l.Admit(c, 1, 1, t0 + 10 * kSec - 1), Verdict::Gap);
  CHECK_VERDICT(l.Admit(c, 1, 1, t0 + 10 * kSec), Verdict::Allow);
}

static void hourlyAndDaily()
{
  ChannelCaps c = fixture();  // 3 per hour, 5 per day
  ChannelLimiter l;
  const int64_t t0 = kWarm;
  CHECK_VERDICT(l.Admit(c, 1, 1, t0), Verdict::Allow);
  CHECK_VERDICT(l.Admit(c, 1, 1, t0 + kSec), Verdict::Allow);
  CHECK_VERDICT(l.Admit(c, 1, 1, t0 + 2 * kSec), Verdict::Allow);
  CHECK_VERDICT(l.Admit(c, 1, 1, t0 + 3 * kSec), Verdict::Hourly);
  CHECK_VERDICT(l.Admit(c, 1, 1, t0 + 59 * kMin), Verdict::Hourly);

  // One hour plus one bucket later the first hour has left the window.
  const int64_t t1 = t0 + kHour + 2 * kMin;
  CHECK_VERDICT(l.Admit(c, 1, 1, t1), Verdict::Allow);
  CHECK_VERDICT(l.Admit(c, 1, 1, t1 + kSec), Verdict::Allow);
  CHECK_VERDICT(l.Admit(c, 1, 1, t1 + 2 * kSec), Verdict::Daily);  // 5 today

  // A day plus one bucket later the day has room again.
  CHECK_VERDICT(l.Admit(c, 1, 1, t0 + 24 * kHour + 16 * kMin), Verdict::Allow);
}

static void stageJoin()
{
  ChannelCaps c       = fixture();
  c.minGapSec         = 60;
  c.maxPerHour        = 2;
  c.maxPulsesPerStage = 2;
  c.stageJoinMs       = 500;
  ChannelLimiter l;
  const int64_t t0 = kWarm;
  CHECK_VERDICT(l.Admit(c, 1, 100, t0), Verdict::Allow);              // stage 1, pulse 1, ends t0+100
  CHECK_VERDICT(l.Admit(c, 1, 100, t0 + 300), Verdict::Allow);        // joins, pulse 2, ends t0+400
  CHECK_VERDICT(l.Admit(c, 1, 100, t0 + 600), Verdict::OverPulses);   // inside the join window, stage full
  CHECK_VERDICT(l.Admit(c, 1, 100, t0 + 901), Verdict::Gap);          // outside the window, inside the gap
  CHECK(l.StagesLastHour(t0 + 901) == 1);                             // joined pulses do not count as stages
  CHECK_VERDICT(l.Admit(c, 1, 100, t0 + 60 * kSec), Verdict::Allow);  // stage 2

  // With stageJoinMs at 0 every command is its own stage, so the gap applies between pulses.
  ChannelCaps strict = c;
  strict.stageJoinMs = 0;
  ChannelLimiter s;
  CHECK_VERDICT(s.Admit(strict, 1, 100, t0), Verdict::Allow);
  CHECK_VERDICT(s.Admit(strict, 1, 100, t0 + 300), Verdict::Gap);
}

static void refusalsRecordNothing()
{
  ChannelCaps c = fixture();
  ChannelLimiter l;
  const int64_t t0 = kWarm;
  for (int i = 0; i < 10; ++i) {
    CHECK_VERDICT(l.Admit(c, 11, 1, t0 + i * kSec), Verdict::OverIntensity);
  }
  CHECK(l.StagesLastHour(t0 + 10 * kSec) == 0);
  CHECK_VERDICT(l.Admit(c, 1, 1, t0 + 10 * kSec), Verdict::Allow);
}

// The bucketed count is never below the exact count of events with now - t < window.
static void windowNeverUnderstates()
{
  WindowCounter<3'600'000, 60'000> counter;
  std::vector<int64_t> events;
  uint64_t seed = 0x9E'37'79'B9'7F'4A'7C'15ULL;
  int64_t now   = 0;
  for (int i = 0; i < 4000; ++i) {
    seed = seed * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
    now += static_cast<int64_t>((seed >> 33) % (5 * kMin));
    if ((seed >> 20) & 1) {
      counter.Add(now);
      events.push_back(now);
    }
    uint32_t exact = 0;
    uint32_t loose = 0;
    for (int64_t t : events) {
      if (now - t < kHour) {
        ++exact;
      }
      if (now - t < kHour + kMin) {
        ++loose;
      }
    }
    CHECK(counter.Count(now) >= exact);
    CHECK(counter.Count(now) <= loose);
  }
}

// A warm restart restores the counters and the clock, so a restart loop gains nothing.
static void retainedSurvivesRestart()
{
  ChannelCaps c = fixture();  // 3 per hour, 5 per day
  ChannelLimiter shock;
  ChannelLimiter vibrate;
  const int64_t t0 = kWarm;
  CHECK_VERDICT(shock.Admit(c, 1, 1, t0), Verdict::Allow);
  CHECK_VERDICT(shock.Admit(c, 1, 1, t0 + kSec), Verdict::Allow);
  CHECK_VERDICT(shock.Admit(c, 1, 1, t0 + 2 * kSec), Verdict::Allow);
  CHECK_VERDICT(vibrate.Admit(c, 1, 1, t0 + 3 * kSec), Verdict::Allow);

  RetainedLimiters retained;
  std::memset(&retained, 0xA5, sizeof(retained));  // Stand-in for no-init memory.
  SealRetained(retained, t0 + 3 * kSec, shock, vibrate);

  // After the restart the clock resumes at the sealed value plus the time since boot.
  int64_t clockMs = -1;
  ChannelLimiter shockAfter;
  ChannelLimiter vibrateAfter;
  CHECK(RestoreRetained(retained, clockMs, shockAfter, vibrateAfter));
  CHECK(clockMs == t0 + 3 * kSec);
  const int64_t sinceBoot = 5 * kSec;
  const int64_t now       = clockMs + sinceBoot;
  CHECK(shockAfter.StagesLastHour(now) == 3);
  CHECK(vibrateAfter.StagesLastHour(now) == 1);
  CHECK_VERDICT(shockAfter.Admit(c, 1, 1, now), Verdict::Hourly);
  CHECK_VERDICT(vibrateAfter.Admit(c, 1, 1, now), Verdict::Allow);
}

// Garbage, a flipped byte or another layout reads as a cold boot and changes nothing.
static void retainedRejectsCorruption()
{
  ChannelCaps c = fixture();
  ChannelLimiter shock;
  ChannelLimiter vibrate;
  CHECK_VERDICT(shock.Admit(c, 1, 1, kWarm), Verdict::Allow);

  RetainedLimiters garbage;
  std::memset(&garbage, 0xA5, sizeof(garbage));
  int64_t clockMs = 7;
  ChannelLimiter s;
  ChannelLimiter v;
  CHECK(!RestoreRetained(garbage, clockMs, s, v));
  CHECK(clockMs == 7);
  CHECK(s.StagesLastDay(kWarm) == 0);

  RetainedLimiters sealed;
  SealRetained(sealed, kWarm, shock, vibrate);
  const std::size_t offsets[] = {offsetof(RetainedLimiters, clockMs), offsetof(RetainedLimiters, shock) + 17, offsetof(RetainedLimiters, vibrate) + 3, offsetof(RetainedLimiters, checksum)};
  for (std::size_t offset : offsets) {
    RetainedLimiters flipped = sealed;
    reinterpret_cast<unsigned char*>(&flipped)[offset] ^= 0x01;
    CHECK(!RestoreRetained(flipped, clockMs, s, v));
  }

  RetainedLimiters otherVersion = sealed;
  otherVersion.version          = static_cast<uint16_t>(kRetainedVersion + 1);
  otherVersion.checksum         = RetainedChecksum(otherVersion);
  CHECK(!RestoreRetained(otherVersion, clockMs, s, v));

  RetainedLimiters negativeClock;
  SealRetained(negativeClock, -1, shock, vibrate);
  CHECK(!RestoreRetained(negativeClock, clockMs, s, v));

  CHECK(clockMs == 7);
  CHECK(s.StagesLastDay(kWarm) == 0);
  CHECK(RestoreRetained(sealed, clockMs, s, v));
  CHECK(s.StagesLastDay(kWarm) == 1);
}

static void lockouts()
{
  static_assert(!kAllowOtaInstall, "the fork never installs firmware over the air");
  static_assert(!kAllowGatewayRestart, "the fork ignores the gateway restart trigger");
}

static void verdictHelpers()
{
  CHECK(Admitted(Verdict::Allow));
  CHECK(Admitted(Verdict::NotCapped));
  CHECK(!Admitted(Verdict::CapsNotSet));
  CHECK(!Admitted(Verdict::BootHold));
  CHECK(!Admitted(Verdict::Gap));
}

int main()
{
  zeroCapsRefuse();
  incoherentCapsRefuse();
  requestCaps();
  coldBootHoldSpacing();
  bootHold();
  gapBetweenStages();
  hourlyAndDaily();
  stageJoin();
  refusalsRecordNothing();
  windowNeverUnderstates();
  retainedSurvivesRestart();
  retainedRejectsCorruption();
  lockouts();
  verdictHelpers();

  if (s_failures != 0) {
    std::printf("%d check(s) failed\n", s_failures);
    return EXIT_FAILURE;
  }
  std::printf("cap limiter: all checks passed\n");
  return EXIT_SUCCESS;
}

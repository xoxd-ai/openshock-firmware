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
#include <algorithm>
#include <cstring>
#include <deque>
#include <utility>
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

// SP-9: a channel with every cap at 0 refuses every command the wire can carry. uint8 intensity by uint16 duration
// is all of them. A refusal leaves no count, so nothing reaches the radio or the window counters.
static void zeroCapsRefuseEveryCommand()
{
  ChannelCaps zero = {};
  ChannelLimiter l;
  uint64_t admitted = 0;
  for (int64_t now : {int64_t {0}, kHour, kWarm}) {
    for (int intensity = 0; intensity <= 255; ++intensity) {
      for (int durationMs = 0; durationMs <= 65'535; ++durationMs) {
        if (l.Admit(zero, static_cast<uint8_t>(intensity), static_cast<uint16_t>(durationMs), now) != Verdict::CapsNotSet) {
          ++admitted;
        }
      }
    }
  }
  CHECK(admitted == 0);
  CHECK(l.StagesLastHour(kWarm) == 0);
  CHECK(l.StagesLastDay(kWarm) == 0);
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

// Review H1: a pulse longer than the gap let stages started every `minGapSec` join into one output. The review's caps,
// 5000 ms pulses with a 2 s gap, 1800 per hour, now refuse as incoherent. A pulse equal to the gap is the edge.
static void pulseLongerThanTheGapIsIncoherent()
{
  ChannelCaps review       = fixture();
  review.maxIntensityPct   = 10;
  review.maxPulseMs        = 5000;
  review.maxPulsesPerStage = 1;
  review.minGapSec         = 2;
  review.maxPerHour        = 1800;
  review.maxPerDay         = 1800;
  CHECK(CapsSet(review));
  CHECK(!CapsCoherent(review));
  ChannelLimiter l;
  CHECK_VERDICT(l.Admit(review, 1, 1, kWarm), Verdict::CapsNotSet);

  ChannelCaps edge = review;
  edge.maxPulseMs  = 2000;
  CHECK(CapsCoherent(edge));
  edge.maxPulseMs = 2001;
  CHECK(!CapsCoherent(edge));

  // The longest pulse the wire carries fits under the widest gap the hourly product allows.
  ChannelCaps hourGap = fixture();
  hourGap.minGapSec   = 3600;
  hourGap.maxPerHour  = 1;
  hourGap.maxPerDay   = 1;
  hourGap.maxPulseMs  = UINT16_MAX;
  CHECK(CapsCoherent(hourGap));
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

// The frame each admitted command counts on top of its duration (review N2).
static const int64_t kF = kMaxRadioFrameMs;

// Review H1: a sender that starts a stage every `minGapSec`, each pulse as long as the gap, gets abutting pulses, which
// the radio joins into one output. The run ceiling, one full stage of 1 x 2000 ms plus its frame, refuses the second
// pulse, so the output is 2 s on and 2 s off, and the stage count shows only the admitted half.
static void stagesEveryGapCannotHoldOutputOn()
{
  ChannelCaps c       = fixture();
  c.maxPulseMs        = 2000;
  c.maxPulsesPerStage = 1;
  c.minGapSec         = 2;
  c.maxPerHour        = 1800;
  c.maxPerDay         = 1800;
  CHECK(CapsCoherent(c));
  CHECK(OnTimeCeilingMs(c) == 2000 + kF);

  ChannelLimiter l;
  const int64_t t0 = kWarm;
  int allowed      = 0;
  int overOnTime   = 0;
  for (int i = 0; i < 1800; ++i) {
    const Verdict v = l.Admit(c, 1, 2000, t0 + i * 2 * kSec);
    if (v == Verdict::Allow) {
      ++allowed;
      CHECK(i % 2 == 0);
    } else {
      CHECK_VERDICT(v, Verdict::OverOnTime);
      CHECK(i % 2 == 1);
      ++overOnTime;
    }
  }
  CHECK(allowed == 900);
  CHECK(overOnTime == 900);
}

// Review N2: each command counts its duration plus one frame, and the ceiling is one full stage counted the same way,
// 3 x (2000 + F). Three full pulses that each leave one frame before the next reach it exactly, so a full legal stage
// passes. Any more output refuses, even 1 ms, which counts 1 + F, until the output has been off for the whole 300 ms
// terminator after the run's last counted end. One millisecond short of that is still the same run.
static void onTimeCeilingBoundaries()
{
  ChannelCaps c       = fixture();
  c.maxPulseMs        = 2000;
  c.maxPulsesPerStage = 3;
  c.minGapSec         = 2;
  c.maxPerHour        = 1800;
  c.maxPerDay         = 1800;
  CHECK(CountedOutputMs(2000) == 2000 + kF);
  CHECK(CountedOutputMs(1) == 1 + kF);
  CHECK(CountedOutputMs(0) == 0);
  CHECK(OnTimeCeilingMs(c) == 3 * (2000 + kF));

  const int64_t t0   = kWarm;
  const int64_t step = 2000 + kF;  // one pulse and its frame
  ChannelLimiter l;
  CHECK_VERDICT(l.Admit(c, 1, 2000, t0), Verdict::Allow);
  CHECK_VERDICT(l.Admit(c, 1, 2000, t0 + step), Verdict::Allow);
  CHECK_VERDICT(l.Admit(c, 1, 2000, t0 + 2 * step), Verdict::Allow);  // exactly the ceiling
  const int64_t runEnd = t0 + 3 * step;
  CHECK_VERDICT(l.Admit(c, 1, 1, runEnd), Verdict::OverOnTime);
  CHECK_VERDICT(l.Admit(c, 1, 2000, runEnd + kOutputRunBreakMs - 1), Verdict::OverOnTime);
  CHECK(l.StagesLastHour(runEnd + kOutputRunBreakMs - 1) == 3);  // a refusal records nothing
  CHECK_VERDICT(l.Admit(c, 1, 2000, runEnd + kOutputRunBreakMs), Verdict::Allow);

  // The same stage with abutting pulses: the frames fall inside the next pulse, so only the last one counts, and the
  // run holds 3 x 2000 + F. Commands may then add up to two more frames of commanded time, which is the most commanded
  // output one run can hold, N x P + (N - 1) x F. On the air the run stays within the ceiling.
  ChannelLimiter m;
  CHECK_VERDICT(m.Admit(c, 1, 2000, t0), Verdict::Allow);
  CHECK_VERDICT(m.Admit(c, 1, 2000, t0 + 2000), Verdict::Allow);
  CHECK_VERDICT(m.Admit(c, 1, 2000, t0 + 4000), Verdict::Allow);
  CHECK_VERDICT(m.Admit(c, 1, static_cast<uint16_t>(2 * kF + 1), t0 + 6000), Verdict::OverOnTime);
  CHECK_VERDICT(m.Admit(c, 1, static_cast<uint16_t>(2 * kF), t0 + 6000), Verdict::Allow);  // commanded 6000 + 2F

  // After the break, and a full gap after the last stage start, a new run holds a full stage again.
  const int64_t t1 = t0 + 8000;
  CHECK(t1 >= t0 + 6000 + 3 * kF + kOutputRunBreakMs);
  CHECK_VERDICT(m.Admit(c, 1, 2000, t1), Verdict::Allow);
  CHECK_VERDICT(m.Admit(c, 1, 2000, t1 + step), Verdict::Allow);
  CHECK_VERDICT(m.Admit(c, 1, 2000, t1 + 2 * step), Verdict::Allow);
  CHECK_VERDICT(m.Admit(c, 1, 2000, t1 + 3 * step), Verdict::OverOnTime);
}

// The ceiling leaves ordinary use alone: a single pulse of exactly `maxPulseMs`, a joined stage of back to back pulses,
// a stage whose pulses leave short gaps and a stage whose pulses overlap all pass. A joined pulse that ends earlier
// adds no time.
static void normalStagesPass()
{
  CHECK(OnTimeCeilingMs(fixture()) == 100 + kF);
  ChannelLimiter single;
  CHECK_VERDICT(single.Admit(fixture(), 10, 100, kWarm), Verdict::Allow);  // exactly the ceiling of a 1 pulse stage

  ChannelCaps c       = fixture();
  c.maxPulseMs        = 100;
  c.maxPulsesPerStage = 3;
  c.minGapSec         = 60;
  c.maxPerHour        = 2;
  c.stageJoinMs       = 500;
  CHECK(OnTimeCeilingMs(c) == 3 * (100 + kF));
  const int64_t t0 = kWarm;

  ChannelLimiter backToBack;
  CHECK_VERDICT(backToBack.Admit(c, 1, 100, t0), Verdict::Allow);
  CHECK_VERDICT(backToBack.Admit(c, 1, 100, t0 + 100), Verdict::Allow);
  CHECK_VERDICT(backToBack.Admit(c, 1, 100, t0 + 200), Verdict::Allow);
  CHECK_VERDICT(backToBack.Admit(c, 1, 100, t0 + 300), Verdict::OverPulses);
  CHECK_VERDICT(backToBack.Admit(c, 1, 100, t0 + 60 * kSec), Verdict::Allow);

  // Gaps of at least one frame: each pulse counts in full with its frame, so the third reaches the ceiling exactly.
  ChannelLimiter shortGaps;
  CHECK_VERDICT(shortGaps.Admit(c, 1, 100, t0), Verdict::Allow);
  CHECK_VERDICT(shortGaps.Admit(c, 1, 100, t0 + 299), Verdict::Allow);
  CHECK_VERDICT(shortGaps.Admit(c, 1, 100, t0 + 598), Verdict::Allow);

  ChannelLimiter overlapping;
  CHECK_VERDICT(overlapping.Admit(c, 1, 100, t0), Verdict::Allow);
  CHECK_VERDICT(overlapping.Admit(c, 1, 100, t0 + 50), Verdict::Allow);
  CHECK_VERDICT(overlapping.Admit(c, 1, 10, t0 + 60), Verdict::Allow);

  // Two stages a full gap apart are two runs.
  ChannelCaps paced       = fixture();
  paced.maxPulseMs        = 1000;
  paced.maxPulsesPerStage = 1;
  paced.minGapSec         = 2;
  ChannelLimiter twoStages;
  CHECK_VERDICT(twoStages.Admit(paced, 1, 1000, t0), Verdict::Allow);
  CHECK_VERDICT(twoStages.Admit(paced, 1, 1000, t0 + 2 * kSec), Verdict::Allow);
}

// Review N3: the run ends at the latest end of any output in it, not at the end of the latest command. Two shockers on
// one channel: A on for 2000 ms, B for 100 ms inside it, then A again at 2000 and at 4000. The third A command takes
// A's unbroken output past one stage and refuses. A run end taken from B's command would have closed the run 300 ms
// after B and let A run 6000 ms unbroken (the reviewer's case, FORK-DELTA-REVIEW-2 extra/m9b_case.cpp).
static void runEndIsTheLatestEndOfAnyOutput()
{
  ChannelCaps c       = fixture();
  c.maxPulseMs        = 2000;
  c.maxPulsesPerStage = 2;
  c.minGapSec         = 2;
  c.maxPerHour        = 1800;
  c.maxPerDay         = 1800;
  c.stageJoinMs       = 100;
  CHECK(CapsCoherent(c));
  ChannelLimiter l;
  const int64_t t0 = kWarm;
  CHECK_VERDICT(l.Admit(c, 10, 2000, t0), Verdict::Allow);         // A
  CHECK_VERDICT(l.Admit(c, 10, 100, t0 + 100), Verdict::Allow);    // B joins the stage inside A
  CHECK_VERDICT(l.Admit(c, 10, 2000, t0 + 2000), Verdict::Allow);  // A again, a new stage, abutting
  CHECK_VERDICT(l.Admit(c, 10, 2000, t0 + 4000), Verdict::OverOnTime);
}

// The RF task as the review modelled it (FORK-DELTA-REVIEW-2, extra/test_h1_review.cpp, from TransmitTask in
// src/radio/RFTransmitter.cpp), for one shocker. `SendCommand` fixes end = arrival + duration. With no sequence the task
// blocks on the queue and starts a pass on arrival. With a sequence it drains the queue at each pass start, then writes
// one whole frame: the payload while the end lies ahead, else the stop frame. It drops the sequence 300 ms after the end.
struct RadioModel {
  int64_t frameMs;
  std::deque<int64_t> queue {};  // ends of queued commands
  bool hasSeq      = false;
  int64_t seqEnd   = 0;
  int64_t passAt   = 0;
  bool payloadOn   = false;
  int64_t frameEnd = 0;

  void command(int64_t now, int64_t durationMs)
  {
    queue.push_back(now + durationMs);
    if (!hasSeq && now >= frameEnd) passAt = now;
  }

  // True while a payload frame is on the air at `now`.
  bool tick(int64_t now)
  {
    if (now >= frameEnd) payloadOn = false;
    if ((hasSeq || !queue.empty()) && now >= passAt && now >= frameEnd) {
      while (!queue.empty()) {
        seqEnd = queue.front();
        hasSeq = true;
        queue.pop_front();
      }
      const int64_t ttl = seqEnd - now;
      if (ttl <= -kOutputRunBreakMs) {
        hasSeq = false;
      } else {
        payloadOn = ttl > 0;
        frameEnd  = now + frameMs;
        passAt    = frameEnd;
      }
    }
    return payloadOn;
  }
};

// Review N2, case 1e: a sender whose 1 ms commands land at each pass start joins them into one stage after another.
// Each command counted 1 ms but aired a whole frame, so before the fix the longest unbroken output on the air was 450 s
// against a 10 s ceiling. Counting each command as its duration plus the longest frame keeps every run on the air, split
// wherever the output stays off for 300 ms, within the ceiling, for the review's 45 ms frame and the longest one.
static void subFrameCommandsCannotHoldTheAirOn()
{
  for (const int64_t frameMs : {int64_t {45}, kF}) {
    ChannelCaps c       = fixture();
    c.maxPulseMs        = 50;
    c.maxPulsesPerStage = 200;
    c.minGapSec         = 1;
    c.maxPerHour        = 3600;
    c.maxPerDay         = 60'000;
    c.stageJoinMs       = static_cast<uint32_t>(frameMs + 15);  // the next pass start joins the stage
    CHECK(CapsCoherent(c));

    ChannelLimiter l;
    RadioModel air {frameMs};
    const int64_t t0 = kWarm;
    int64_t next = t0, lastEnd = 0, lastOn = INT64_MIN / 2, runOn = 0, longestOn = 0;
    int inStage = 0, admitted = 0, overOnTime = 0;
    for (int64_t t = t0; t < t0 + 900 * kSec; ++t) {
      if (t == next) {
        const Verdict v = l.Admit(c, 10, 1, t);
        if (v == Verdict::Allow) {
          air.command(t, 1);
          lastEnd = t + 1;
          ++inStage;
          ++admitted;
        }
        overOnTime += v == Verdict::OverOnTime;
        if (inStage >= c.maxPulsesPerStage) {
          inStage = 0;
          next    = lastEnd + c.stageJoinMs + 1;
          next += (frameMs - (next - t0) % frameMs) % frameMs;  // back in phase with the passes
        } else {
          next = t + frameMs;
        }
      }
      if (air.tick(t)) {
        if (t - lastOn > kOutputRunBreakMs) {
          runOn = 0;  // the output stayed off for the whole 300 ms
        }
        lastOn = t;
        ++runOn;
        longestOn = std::max(longestOn, runOn);
      }
    }
    CHECK(admitted > 0);
    CHECK(overOnTime > 0);
    CHECK(longestOn <= OnTimeCeilingMs(c));
    if (longestOn > OnTimeCeilingMs(c)) {
      std::printf("  frame %lld ms: longest air run %lld ms, ceiling %lld ms\n", static_cast<long long>(frameMs), static_cast<long long>(longestOn), static_cast<long long>(OnTimeCeilingMs(c)));
    }
  }
}

// Any sender, any caps: rebuild the output of the admitted commands as intervals of their counted time, split it into
// runs wherever it stays off for the 300 ms terminator, and check that no run holds more than one full stage of counted
// time, and no more commanded time than N x P + (N - 1) x F. The counted intervals hold every frame the radio sends,
// since an overwrite can only end a sequence sooner and the last frame ends at most one frame past the end.
static void noRunExceedsTheCeiling()
{
  uint64_t seed = 0x2545'F491'4F6C'DD1DULL;
  auto next     = [&seed](uint64_t mod) {
    seed = seed * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
    return (seed >> 33) % mod;
  };
  for (int trial = 0; trial < 200; ++trial) {
    ChannelCaps c       = fixture();
    c.minGapSec         = 1 + static_cast<uint32_t>(next(4));
    c.maxPulseMs        = static_cast<uint16_t>(1 + next(c.minGapSec * 1000));
    c.maxPulsesPerStage = static_cast<uint16_t>(1 + next(4));
    c.maxPerHour        = static_cast<uint16_t>(3600 / c.minGapSec);
    c.maxPerDay         = 60'000;
    c.stageJoinMs       = static_cast<uint32_t>(next(2) == 0 ? 0 : next(1000));
    CHECK(CapsSet(c) && CapsCoherent(c));

    ChannelLimiter l;
    std::vector<std::pair<int64_t, uint16_t>> admitted;
    int64_t now = kWarm;
    for (int i = 0; i < 400; ++i) {
      now += static_cast<int64_t>(next(c.minGapSec * 1000 + 600));
      const uint16_t duration = static_cast<uint16_t>(next(c.maxPulseMs + 1u));
      if (l.Admit(c, 1, duration, now) == Verdict::Allow) {
        admitted.emplace_back(now, duration);
      }
    }

    const int64_t commandedMax = static_cast<int64_t>(c.maxPulseMs) * c.maxPulsesPerStage + (c.maxPulsesPerStage - 1) * kF;

    int64_t runOn  = 0;
    int64_t runEnd = INT64_MIN / 2;
    int64_t cmdOn  = 0;
    int64_t cmdEnd = INT64_MIN / 2;
    for (const auto& [start, duration] : admitted) {
      const int64_t end = start + CountedOutputMs(duration);
      if (start >= runEnd + kOutputRunBreakMs) {
        runOn  = end - start;
        runEnd = end;
        cmdOn  = duration;
        cmdEnd = start + duration;
      } else {
        runOn += std::max<int64_t>(0, end - std::max(start, runEnd));
        runEnd = std::max(runEnd, end);
        cmdOn += std::max<int64_t>(0, start + duration - std::max(start, cmdEnd));
        cmdEnd = std::max(cmdEnd, start + static_cast<int64_t>(duration));
      }
      CHECK(runOn <= OnTimeCeilingMs(c));
      CHECK(cmdOn <= commandedMax);
    }
  }
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

// Review 3 F3: version 3 keeps the layout of version 2, but the run end and run total count one radio frame per command
// since review N2. A record a version 2 image sealed must not restore. It reads as a cold boot: the clock starts at 0, as
// `SafetyCaps` then sets it, and the channel holds.
static void version2RecordReadsAsColdBoot()
{
  CHECK(kRetainedVersion == 3);
  ChannelCaps c = fixture();
  ChannelLimiter shock;
  ChannelLimiter vibrate;
  CHECK_VERDICT(shock.Admit(c, 1, 1, kWarm), Verdict::Allow);

  RetainedLimiters older;
  SealRetained(older, kWarm + kSec, shock, vibrate);
  older.version  = 2;
  older.checksum = RetainedChecksum(older);

  int64_t clockMs = -1;
  ChannelLimiter s;
  ChannelLimiter v;
  CHECK(!RestoreRetained(older, clockMs, s, v));
  CHECK(clockMs == -1);
  CHECK(s.StagesLastDay(kWarm + kSec) == 0);

  // The cold boot path: the hub clock is the time since boot, which is below the hold.
  const int64_t sinceBoot = 5 * kSec;
  CHECK(sinceBoot < ColdBootHoldMs(c));
  CHECK_VERDICT(s.Admit(c, 1, 1, sinceBoot), Verdict::BootHold);
  CHECK_VERDICT(v.Admit(c, 1, 1, sinceBoot), Verdict::BootHold);
  CHECK_VERDICT(s.Admit(c, 1, 1, ColdBootHoldMs(c)), Verdict::Allow);
}

static void lockouts()
{
  static_assert(!kAllowOtaInstall, "the fork never installs firmware over the air");
  static_assert(!kAllowGatewayRestart, "the fork ignores the gateway restart trigger");
  static_assert(!kAllowGatewayActuation, "only the serial port actuates (review H3)");
  CHECK(!GatewayCommandAllowed(OpenShock::ShockerCommandType::Shock));
  CHECK(!GatewayCommandAllowed(OpenShock::ShockerCommandType::Vibrate));
  CHECK(GatewayCommandAllowed(OpenShock::ShockerCommandType::Stop));
  CHECK(GatewayCommandAllowed(OpenShock::ShockerCommandType::Sound));
  CHECK(GatewayCommandAllowed(OpenShock::ShockerCommandType::Light));
}

static void verdictHelpers()
{
  CHECK(Admitted(Verdict::Allow));
  CHECK(Admitted(Verdict::NotCapped));
  CHECK(!Admitted(Verdict::CapsNotSet));
  CHECK(!Admitted(Verdict::BootHold));
  CHECK(!Admitted(Verdict::Gap));
  CHECK(!Admitted(Verdict::OverOnTime));
}

int main()
{
  zeroCapsRefuse();
  zeroCapsRefuseEveryCommand();
  incoherentCapsRefuse();
  pulseLongerThanTheGapIsIncoherent();
  requestCaps();
  coldBootHoldSpacing();
  bootHold();
  gapBetweenStages();
  hourlyAndDaily();
  stageJoin();
  stagesEveryGapCannotHoldOutputOn();
  onTimeCeilingBoundaries();
  normalStagesPass();
  runEndIsTheLatestEndOfAnyOutput();
  subFrameCommandsCannotHoldTheAirOn();
  noRunExceedsTheCeiling();
  refusalsRecordNothing();
  windowNeverUnderstates();
  retainedSurvivesRestart();
  retainedRejectsCorruption();
  version2RecordReadsAsColdBoot();
  lockouts();
  verdictHelpers();

  if (s_failures != 0) {
    std::printf("%d check(s) failed\n", s_failures);
    return EXIT_FAILURE;
  }
  std::printf("cap limiter: all checks passed\n");
  return EXIT_SUCCESS;
}

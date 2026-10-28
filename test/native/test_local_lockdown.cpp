// Host test for the SP-46 local lockdown (include/softproxy/Lockout.h) and the cap refusal report
// (include/softproxy/RefusalReport.h). It needs no board and no PlatformIO:
//
//   g++ -std=c++20 -Wall -Wextra -Werror -Iinclude test/native/test_local_lockdown.cpp -o local_lockdown_test && ./local_lockdown_test

#include "softproxy/CapLimiter.h"
#include "softproxy/Lockout.h"
#include "softproxy/RefusalReport.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace OpenShock::Softproxy;

static int s_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++s_failures;                                               \
    }                                                             \
  } while (0)

// Every LocalToHubMessagePayload name at 1.5.2 (include/serialization/_fbs/LocalToHubMessage_generated.h).
static const char* const kUpstreamPayloads[] = {
  "NONE",
  "WifiScanCommand",
  "WifiNetworkSaveCommand",
  "WifiNetworkForgetCommand",
  "WifiNetworkConnectCommand",
  "WifiNetworkDisconnectCommand",
  "OtaUpdateSetIsEnabledCommand",
  "OtaUpdateSetDomainCommand",
  "OtaUpdateSetUpdateChannelCommand",
  "OtaUpdateSetCheckIntervalCommand",
  "OtaUpdateSetAllowBackendManagementCommand",
  "OtaUpdateSetRequireManualApprovalCommand",
  "OtaUpdateHandleUpdateRequestCommand",
  "OtaUpdateCheckForUpdatesCommand",
  "OtaUpdateStartUpdateCommand",
  "AccountLinkCommand",
  "AccountUnlinkCommand",
  "SetRfTxPinCommand",
  "SetEstopEnabledCommand",
  "SetEstopPinCommand",
};

// SP-46 names these. Each must refuse on the port 81 socket.
static const char* const kSp46Refused[] = {
  "SetEstopEnabledCommand",
  "SetEstopPinCommand",
  "WifiScanCommand",
  "WifiNetworkSaveCommand",
  "WifiNetworkForgetCommand",
  "WifiNetworkConnectCommand",
  "WifiNetworkDisconnectCommand",
  "AccountLinkCommand",
  "AccountUnlinkCommand",
};

static void sp46RefusesNamedCommands()
{
  for (const char* name : kSp46Refused) {
    if (LocalSocketAllows(name)) {
      std::printf("FAIL %s:%d: local socket allows %s\n", __FILE__, __LINE__, name);
      ++s_failures;
    }
  }
}

static void localSocketAllowsNothing()
{
  // SPR-75: the port 81 socket serves no command, the RF TX pin included, since that pin could be moved
  // onto the E-Stop GPIO.
  for (const char* name : kUpstreamPayloads) {
    if (LocalSocketAllows(name)) {
      std::printf("FAIL %s:%d: local socket allows %s\n", __FILE__, __LINE__, name);
      ++s_failures;
    }
  }
  CHECK(kLocalSocketAllowed.empty());
  CHECK(!LocalSocketAllows("SetRfTxPinCommand"));
}

static void localSocketFailsClosed()
{
  CHECK(!LocalSocketAllows(nullptr));
  CHECK(!LocalSocketAllows(""));
  CHECK(!LocalSocketAllows("NONE"));
  // A payload a later upstream release might add.
  CHECK(!LocalSocketAllows("SetCaptivePortalPasswordCommand"));
  // Exact match only: no prefix, suffix or case variant.
  CHECK(!LocalSocketAllows("SetRfTxPin"));
  CHECK(!LocalSocketAllows("SetRfTxPinCommandX"));
  CHECK(!LocalSocketAllows("setrftxpincommand"));
}

static void gatewayLockouts()
{
  static_assert(!kAllowGatewayCaptivePortalEnable, "SP-46: the gateway may not hold the captive portal open");
  static_assert(!kAllowOtaInstall, "the fork never installs firmware over the air");
  static_assert(!kAllowGatewayRestart, "the fork ignores the gateway restart trigger");
  static_assert(!LocalSocketAllows("SetEstopEnabledCommand"), "the allow list is usable at compile time");
  static_assert(!LocalSocketAllows("SetRfTxPinCommand"), "SPR-75: the RF TX pin is serial only");
}

static void reportStartsEmpty()
{
  RefusalReport r;
  RefusalSnapshot s = r.Snapshot();
  CHECK(s.count == 0);
  CHECK(s.lastCode == 0);
  CHECK(std::strcmp(RefusalCodeName(s.lastCode), "none") == 0);
}

static void reportIgnoresAdmitted()
{
  RefusalReport r;
  CHECK(r.Record(Verdict::Allow) == 0);
  CHECK(r.Record(Verdict::NotCapped) == 0);
  RefusalSnapshot s = r.Snapshot();
  CHECK(s.count == 0);
  CHECK(s.lastCode == 0);
}

static void reportCountsEachRefusal()
{
  const Verdict refusals[] = {Verdict::CapsNotSet, Verdict::BootHold, Verdict::OverIntensity, Verdict::OverPulseMs, Verdict::OverPulses, Verdict::Gap, Verdict::Hourly, Verdict::Daily};

  RefusalReport r;
  uint32_t expected = 0;
  for (Verdict v : refusals) {
    ++expected;
    CHECK(r.Record(v) == expected);
    RefusalSnapshot s = r.Snapshot();
    CHECK(s.count == expected);
    CHECK(s.lastCode == static_cast<uint8_t>(v));
    CHECK(std::strcmp(RefusalCodeName(s.lastCode), VerdictName(v)) == 0);

    // An admitted command between two refusals changes nothing.
    CHECK(r.Record(Verdict::Allow) == 0);
    CHECK(r.Snapshot().count == expected);
    CHECK(r.Snapshot().lastCode == static_cast<uint8_t>(v));
  }
}

static void reportSaturates()
{
  RefusalReport r;
  for (uint32_t i = 0; i < RefusalReport::kMaxCount; ++i) {
    r.Record(Verdict::Gap);
  }
  CHECK(r.Snapshot().count == RefusalReport::kMaxCount);
  CHECK(r.Record(Verdict::Daily) == RefusalReport::kMaxCount);
  RefusalSnapshot s = r.Snapshot();
  CHECK(s.count == RefusalReport::kMaxCount);
  CHECK(s.lastCode == static_cast<uint8_t>(Verdict::Daily));
}

static void wireCodeNames()
{
  CHECK(std::strcmp(RefusalCodeName(0), "none") == 0);
  CHECK(std::strcmp(RefusalCodeName(2), "caps-not-set") == 0);
  CHECK(std::strcmp(RefusalCodeName(9), "daily") == 0);
  CHECK(std::strcmp(RefusalCodeName(10), "unknown") == 0);
  CHECK(std::strcmp(RefusalCodeName(255), "unknown") == 0);
}

static void logLine()
{
  char line[kRefusalLineSize];
  int n = FormatRefusalLine(line, sizeof(line), 7, "shock", Verdict::Gap);
  CHECK(n > 0);
  CHECK(std::string(line) == "softproxy-refusal seq=7 channel=shock verdict=gap");

  FormatRefusalLine(line, sizeof(line), 1, nullptr, Verdict::Daily);
  CHECK(std::string(line) == "softproxy-refusal seq=1 channel=unknown verdict=daily");

  // The longest line fits the buffer without truncation.
  n = FormatRefusalLine(line, sizeof(line), RefusalReport::kMaxCount, "vibrate", Verdict::OverIntensity);
  CHECK(n > 0 && static_cast<std::size_t>(n) < kRefusalLineSize);
  CHECK(std::string(line) == "softproxy-refusal seq=16777215 channel=vibrate verdict=over-intensity");
}

int main()
{
  sp46RefusesNamedCommands();
  localSocketAllowsNothing();
  localSocketFailsClosed();
  gatewayLockouts();
  reportStartsEmpty();
  reportIgnoresAdmitted();
  reportCountsEachRefusal();
  reportSaturates();
  wireCodeNames();
  logLine();

  if (s_failures != 0) {
    std::printf("%d check(s) failed\n", s_failures);
    return 1;
  }
  std::printf("local lockdown and refusal report: all checks passed\n");
  return 0;
}

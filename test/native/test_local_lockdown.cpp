// Host test for the SP-46 local lockdown and the gateway lockouts (include/softproxy/Lockout.h). It needs no board and
// no PlatformIO:
//
//   g++ -std=c++20 -Wall -Wextra -Werror -Iinclude test/native/test_local_lockdown.cpp -o local_lockdown_test && ./local_lockdown_test

#include "softproxy/Lockout.h"

#include <cstdio>

using namespace OpenShock::Softproxy;

static int s_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++s_failures;                                               \
    }                                                             \
  } while (0)

// Every LocalToHubMessagePayload name in the fork schema (include/serialization/_fbs/LocalToHubMessage_generated.h).
// Upstream 1.5.2 also had SetEstopEnabledCommand and SetEstopPinCommand, removed by TIN-7125 858126ce.
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
};

// SP-46 names these. Each must refuse on the port 81 socket.
static const char* const kSp46Refused[] = {
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
  // SPR-75: the port 81 socket serves no command, the RF TX pin included.
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
  static_assert(!LocalSocketAllows("WifiScanCommand"), "the allow list is usable at compile time");
  static_assert(!LocalSocketAllows("SetRfTxPinCommand"), "SPR-75: the RF TX pin is serial only");
  static_assert(!kOpenPortalOnRadioFailure, "the portal stays closed when only the radio fails");
  static_assert(kLocalSocketMaxMessageBytes == 4096, "the port 81 message bound is the verifier limit");
}

int main()
{
  sp46RefusesNamedCommands();
  localSocketAllowsNothing();
  localSocketFailsClosed();
  gatewayLockouts();

  if (s_failures != 0) {
    std::printf("%d check(s) failed\n", s_failures);
    return 1;
  }
  std::printf("local lockdown: all checks passed\n");
  return 0;
}

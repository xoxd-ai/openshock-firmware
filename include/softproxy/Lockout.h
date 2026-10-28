#pragma once

// Remote and local paths that the xoxd-ai fork closes so nobody off the serial port can undo the Softproxy
// caps (include/softproxy/CapLimiter.h) or the hub E-Stop. No Arduino or ESP-IDF dependency, so the host test
// can check them.

#include <array>
#include <string_view>

namespace OpenShock::Softproxy {
  /// The fork never installs firmware over the air. An upstream image carries no cap enforcement, so a
  /// remote install would remove it. The gateway `OtaUpdateRequest`, the update check on startup or on a
  /// timer and a changed CDN domain all stop before any download. Firmware changes only by a serial flash.
  constexpr bool kAllowOtaInstall = false;

  /// The fork ignores the gateway `Restart` trigger. A restart is no longer a way to clear the counters,
  /// since they survive a warm restart (`RetainedLimiters`), but the gateway has no need to reboot the hub.
  /// The local serial `restart` command still works.
  constexpr bool kAllowGatewayRestart = false;

  /// The fork ignores the gateway `CaptivePortalEnable` trigger (Softproxy SP-46). The portal still opens
  /// while the gateway is unreachable, as upstream does, but the gateway can no longer hold it open.
  /// `CaptivePortalDisable` still works, since it only closes the portal.
  constexpr bool kAllowGatewayCaptivePortalEnable = false;

  /// Local socket commands (captive portal WebSocket, port 81) that the fork still serves, by their
  /// `LocalToHubMessagePayload` name. The socket has no authentication and the portal access point has no
  /// password, so SP-46 refuses the E-Stop, E-Stop pin, Wi-Fi and account-link commands there. The serial
  /// port stays the local path for each of them: `estop`, `networks` and `authtoken`.
  ///
  /// This is an allow list, and it is empty: the socket serves no command. A payload not named here,
  /// including one a later upstream release adds, is refused. The RF TX pin is refused as well (TIN-7125
  /// SPR-75), because it could move the transmitter onto the E-Stop GPIO and so disable the E-Stop from the
  /// open access point. Serial `rftxpin` stays its local path. The nine OTA commands are empty handlers at
  /// 1.5.2, so refusing them changes nothing today and keeps a later upstream body from opening an install
  /// path.
  inline constexpr std::array<std::string_view, 0> kLocalSocketAllowed{};

  /// Whether the local socket may run the command with this payload name. A null or empty name refuses.
  constexpr bool LocalSocketAllows(const char* payloadName)
  {
    if (payloadName == nullptr) {
      return false;
    }
    const std::string_view name(payloadName);
    if (name.empty()) {
      return false;
    }
    for (std::string_view allowed : kLocalSocketAllowed) {
      if (name == allowed) {
        return true;
      }
    }
    return false;
  }
}  // namespace OpenShock::Softproxy

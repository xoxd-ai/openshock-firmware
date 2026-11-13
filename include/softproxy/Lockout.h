#pragma once

// Remote and local paths that the xoxd-ai fork closes so nobody off the serial port can undo the Softproxy
// caps (include/softproxy/CapLimiter.h) or the hub E-Stop. No Arduino or ESP-IDF dependency, so the host test
// can check them.

#include "ShockerCommandType.h"

#include <array>
#include <cstddef>
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

  /// The fork refuses Shock and Vibrate that arrive from the gateway in a `ShockerCommandList` (review H3). The hub caps
  /// and the E-Stop would still apply, but none of Softproxy's server checks 4 to 8, so a linked account could actuate
  /// outside the alarm service. Only the local serial port, where Softproxy's driver runs, actuates. The gateway
  /// `Trigger` `EmergencyStop` still engages the E-Stop, and a gateway Stop or Sound still passes.
  constexpr bool kAllowGatewayActuation = false;

  /// Whether a command of this type from the gateway may reach `HandleCommand`.
  constexpr bool GatewayCommandAllowed(ShockerCommandType type)
  {
    return kAllowGatewayActuation || (type != ShockerCommandType::Shock && type != ShockerCommandType::Vibrate);
  }

  /// The fork keeps the portal closed when only the radio fails. Upstream opens it so a phone can fix the RF TX pin,
  /// but the local socket serves no command here, so the portal would add an open access point and no repair path.
  constexpr bool kOpenPortalOnRadioFailure = false;

  /// Largest message, in bytes, that the port 81 socket accepts. The flatbuffers verifier uses it too. The
  /// defragmenter holds a fragmented message to this bound, and an unbounded one could exhaust the heap and restart
  /// the hub from the open access point (`WebSocketDeFragger`).
  constexpr std::size_t kLocalSocketMaxMessageBytes = 4096;

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
  inline constexpr std::array<std::string_view, 0> kLocalSocketAllowed {};

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

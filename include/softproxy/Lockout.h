#pragma once

// Remote paths that the xoxd-ai fork closes so the gateway cannot undo the Softproxy caps
// (include/softproxy/CapLimiter.h). No Arduino or ESP-IDF dependency, so the host test can check them.

namespace OpenShock::Softproxy {
  /// The fork never installs firmware over the air. An upstream image carries no cap enforcement, so a
  /// remote install would remove it. The gateway `OtaUpdateRequest`, the update check on startup or on a
  /// timer and a changed CDN domain all stop before any download. Firmware changes only by a serial flash.
  constexpr bool kAllowOtaInstall = false;

  /// The fork ignores the gateway `Restart` trigger. A restart is no longer a way to clear the counters,
  /// since they survive a warm restart (`RetainedLimiters`), but the gateway has no need to reboot the hub.
  /// The local serial `restart` command still works.
  constexpr bool kAllowGatewayRestart = false;
}  // namespace OpenShock::Softproxy

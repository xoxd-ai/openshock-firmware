# Softproxy cap enforcement (xoxd-ai fork)

This fork of `OpenShock/Firmware` starts from tag `1.5.2` (commit `5546c13eb745e4fb2b4ca7b0f4160de8b261872c`). It adds one change: the hub checks every shock and vibrate command against operator-set caps before the radio sends it. Softproxy's alarm service checks the same caps first (Softproxy ADR-0008 decision 6, `Device.dhall` server check 3). The hub check is the second line. A step over the intensity, pulse, per-stage or gap caps cannot reach the shocker even when the server sends it. The hourly and daily caps hold across a warm restart, and a power cycle costs a full cold boot hold (see Fail closed). To keep the gateway from removing the check, the fork also refuses over-the-air installs and ignores the gateway restart trigger (see Remote paths). The captive portal's local socket refuses the E-Stop, Wi-Fi and account-link commands (SP-46, see Local socket). Each refusal reaches the hub log and the gateway heartbeat (see Refusal report). Linear: TIN-7545.

## What the hub enforces

| Cap | Hub rule | Refusal |
| --- | --- | --- |
| `maxIntensityPct` | command intensity at most this, and at most 100 | `over-intensity` |
| `maxPulseMs` | command duration at most this | `over-pulse-ms` |
| `maxPulsesPerStage` | commands in one stage at most this | `over-pulses` |
| `minGapSec` | a new stage starts at least this long after the previous stage started | `gap` |
| `maxPerHour` | stage starts in any 60 minutes at most this | `hourly` |
| `maxPerDay` | stage starts in any 24 hours at most this | `daily` |

Shock and vibrate have separate caps and separate counters. Stop, Sound and Light carry no cap. The keep-alive ping (vibrate at intensity 0) goes straight to the radio and is not counted.

A stage is one Softproxy request. A command starts a new stage unless `stageJoinMs` is above 0 and the command starts within `stageJoinMs` after the previous pulse of the open stage ends. `stageJoinMs` is hub-only and defaults to 0, so every command is its own stage and the gap applies between pulses.

## Fail closed

- The fork ships no cap value. Every cap defaults to 0, and a channel with any cap at 0 refuses every command (Softproxy SP-9). Caps that are incoherent (intensity above 100, hourly above daily, `minGapSec * maxPerHour` above 3600) also refuse, as Softproxy `capsCoherent` does.
- A missing, unreadable or unknown stored record loads as all 0.
- The counters and the hub clock live in RTC no-init memory, sealed with a checksum after every shock or vibrate command the gate checks. A warm restart (software reset, panic or watchdog) keeps that memory, so the hub restores the counters and the clock resumes where it was last sealed. The downtime counts as not passed, so the clock never runs ahead of real time, which can only refuse sooner.
- A cold boot (power loss) fails the checksum. The counters start empty and the clock starts at 0, so each channel refuses until the clock reaches the longest of `minGapSec`, 3600 s divided by `maxPerHour` and 86400 s divided by `maxPerDay`, rounded up (`boot-hold`). Every power cycle therefore costs a full hold. A hub that runs between power cycles can still exceed `maxPerDay` across them, so Softproxy's server counters stay the authority for the daily limit.
- The window counters use fixed buckets (1 minute for the hour, 15 minutes for the day). They can overstate a count by up to one bucket, which refuses sooner, and never understate it.

## Remote paths

Upstream lets the gateway replace the firmware and reboot the hub. Either would undo the hub check: an upstream image carries no caps, and a reboot used to empty the counters. `include/softproxy/Lockout.h` closes both, whatever the stored config says:

| Path | Upstream 1.5.2 | This fork |
| --- | --- | --- |
| Gateway `OtaUpdateRequest` | queues an install of any version from the CDN | `TryStartFirmwareUpdate` refuses |
| OTA check on startup or on a timer, a changed CDN domain | installs the found version | the OTA task stops before any download |
| Gateway `Trigger` `Restart` | `esp_restart()` | ignored and logged |
| Gateway `Trigger` `CaptivePortalEnable` | holds the portal open while the gateway is connected | ignored and logged (SP-46). `CaptivePortal::SetAlwaysEnabled(true)` also refuses, whatever the caller |
| Serial flash, serial `restart` | allowed | allowed, both need the local serial port |

## Local socket

The captive portal opens an access point with no password whenever the gateway is unreachable, and its WebSocket on port 81 has no authentication. Upstream lets anyone on that access point turn the E-Stop off, move its pin, change the Wi-Fi networks and link the hub to another account. Softproxy SP-46 (TIN-7125 `fd8d8374`) refuses those commands there. The serial port stays the local path.

`kLocalSocketAllowed` in `include/softproxy/Lockout.h` is an allow list, checked in `src/message_handlers/websocket/Local.cpp` before dispatch. A refused command is logged (`Refusing local socket command <name>`) and the client gets the error message `refused_local_command`.

| Local command | Upstream 1.5.2 | This fork | Serial path |
| --- | --- | --- | --- |
| `SetEstopEnabledCommand` | allowed | refused | `estop enabled` |
| `SetEstopPinCommand` | allowed | refused | `estop pin` |
| `WifiScanCommand`, `WifiNetworkSaveCommand`, `WifiNetworkForgetCommand`, `WifiNetworkConnectCommand`, `WifiNetworkDisconnectCommand` | allowed | refused | `networks` |
| `AccountLinkCommand`, `AccountUnlinkCommand` | allowed | refused | `authtoken` |
| The nine `OtaUpdate*` commands | empty handlers | refused, so a later upstream body cannot open an install path | none needed |
| `SetRfTxPinCommand` | allowed | allowed, SP-46 does not name it | `rftxpin` |
| a payload a later release adds | handled | refused until it is added to the list | |

The portal itself still serves the static frontend, and the access point still opens while the gateway is down.

## Refusal report

Before this change a refused step left one log line and nothing else, so Softproxy logged an actuation that never reached the shocker (softproxy #27 review, finding 4). `SafetyCaps::Admit` now counts every refusal in `include/softproxy/RefusalReport.h` and writes one log line. Gateway and serial `rftransmit` commands both pass there.

| Trace | Form | Read by |
| --- | --- | --- |
| Hub log (serial) | `softproxy-refusal seq=<n> channel=<shock\|vibrate> verdict=<name>`, level warning, kept by the production log level `INFO` | Softproxy on the local serial path |
| Gateway heartbeat | `Pong.softproxy_cap_refusals` (`uint32`, field 2, vtable offset 8): refusals since boot, saturating at 16777215. `Pong.softproxy_last_refusal` (`uint8`, field 3, vtable offset 10): verdict code of the latest refusal | the gateway, on every Ping |
| Serial `safetycaps` | `SafetyCaps\|refusals\|count=<n>,last=<name>` after the two channel lines | the operator or Softproxy |

The verdict names are those in the cap table above, plus `caps-not-set` and `boot-hold`. Wire codes: 0 none, 2 `caps-not-set`, 3 `boot-hold`, 4 `over-intensity`, 5 `over-pulse-ms`, 6 `over-pulses`, 7 `gap`, 8 `hourly`, 9 `daily`. A receiver that sees `softproxy_cap_refusals` rise between two heartbeats knows that many steps were refused. The count lives in RAM and restarts at 0 after any reboot, which a lower `uptime` in the same `Pong` shows.

The two fields come after `uptime` and `rssi`, so a gateway built from the upstream schema reads the heartbeat as before and ignores them. `softproxy/schemas/HubToGatewayMessage.fbs` holds the fork schema. The `schemas` submodule still points at upstream commit `aaceae18`, and `include/serialization/_fbs/HubToGatewayMessage_generated.h` was edited to match the fork schema without running `flatc`.

## Who can set the caps

The caps live in their own NVS namespace (`spcaps`), outside the config file. `rawconfig`, `jsonconfig`, the captive portal and the gateway cannot read or change them, and with OTA installs refused the gateway cannot replace the code that reads them. Only the local serial port can:

| Serial command | Effect |
| --- | --- |
| `safetycaps` | print both channels, whether each is set and the stages counted since boot |
| `safetycaps shock <maxIntensityPct> <maxPulseMs> <maxPulsesPerStage> <minGapSec> <maxPerHour> <maxPerDay> [stageJoinMs]` | store the shock caps |
| `safetycaps vibrate ...` | store the vibrate caps, same arguments |
| `safetycaps clear` | set every cap to 0 |

`factoryreset` also clears the caps. The operator sets the values at an attended calibration session (Softproxy SP-10). The real values live only in the Softproxy private registry and never in this repository. Softproxy's alarm service must never send `safetycaps shock` or `safetycaps vibrate`. It may read `safetycaps` and refuse when the hub caps differ from the registry.

## Code

| File | Role |
| --- | --- |
| `include/softproxy/CapLimiter.h` | decision logic, cold boot hold and the retained state seal, no Arduino or ESP-IDF dependency |
| `include/softproxy/Lockout.h` | the OTA install, gateway restart and captive portal enable lockouts, and the local socket allow list |
| `include/softproxy/RefusalReport.h` | the refusal count, the wire codes and the hub log line, no Arduino or ESP-IDF dependency |
| `include/softproxy/SafetyCaps.h`, `src/softproxy/SafetyCaps.cpp` | NVS store, mutex, the `Admit` gate, the hub clock and the RTC no-init copy of the counters |
| `src/OtaUpdateManager.cpp`, `src/message_handlers/websocket/gateway/Trigger.cpp`, `src/captiveportal/Manager.cpp` | apply the gateway lockouts |
| `src/message_handlers/websocket/Local.cpp` | applies the local socket allow list |
| `src/serialization/WSGateway.cpp`, `softproxy/schemas/HubToGatewayMessage.fbs` | the two `Pong` refusal fields |
| `src/CommandHandler.cpp` | `HandleCommand` calls `SafetyCaps::Admit` after the E-Stop check, before the radio. Gateway and serial `rftransmit` commands both pass here |
| `src/serial/command_handlers/safetycaps.cpp` | the `safetycaps` serial command |
| `src/config/Config.cpp` | `FactoryReset` clears the caps |
| `test/native/test_cap_limiter.cpp` | host test of the decision logic, the retained state and the lockout constants |
| `test/native/test_local_lockdown.cpp` | host test of the local socket allow list, the portal lockout, the refusal count, the wire codes and the log line |
| `.github/workflows/softproxy-host-test.yml` | runs both host tests on pushes and PRs to `softproxy/**` |

Run the host tests with:

```sh
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude test/native/test_cap_limiter.cpp -o cap_limiter_test && ./cap_limiter_test
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude test/native/test_local_lockdown.cpp -o local_lockdown_test && ./local_lockdown_test
```

## Not covered here

- The hub cannot see a request's purpose, schedule or arm state. Those stay Softproxy server checks 4 to 8.
- Hourly and daily counters do not survive a power cycle. The cold boot hold spaces stages after one, and the server counters stay the authority (see Fail closed).
- RTC no-init retention across a software, panic and watchdog reset is ESP-IDF behaviour, not yet checked on the PiShock boards.
- The upstream live control gateway (`openshock/live-control-gateway:3.17.0`) does not read the two `Pong` fields. They reach it on the wire, but Softproxy sees them only through a gateway change or by reading the serial log and `safetycaps`.
- The refusal count restarts after every reboot. A refusal just before a reboot shows in the hub log, not in the next heartbeat.
- The TypeScript code generated for the frontend (`frontend/src/lib/_fbs`) still has the upstream `Pong`. The frontend never reads it.
- Raising a cap needs only serial access. Whether to also require the E-Stop to be engaged while caps change is an open operator decision.

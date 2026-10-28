# Softproxy cap enforcement (xoxd-ai fork)

This fork of `OpenShock/Firmware` starts from tag `1.5.2` (commit `5546c13eb745e4fb2b4ca7b0f4160de8b261872c`). It adds one change: the hub checks every shock and vibrate command against operator-set caps before the radio sends it. Softproxy's alarm service checks the same caps first (Softproxy ADR-0008 decision 6, `Device.dhall` server check 3). The hub check is the second line. A step over the intensity, pulse, per-stage or gap caps cannot reach the shocker even when the server sends it. The hourly and daily caps hold across a warm restart, and a power cycle costs a full cold boot hold (see Fail closed). To keep the gateway from removing the check, the fork also refuses over-the-air installs and ignores the gateway restart trigger (see Remote paths). Linear: TIN-7545.

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
| Serial flash, serial `restart` | allowed | allowed, both need the local serial port |

The gateway can still enable the captive portal. Its local commands cannot read or change the caps or start an install.

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
| `include/softproxy/Lockout.h` | the OTA install and gateway restart lockouts |
| `include/softproxy/SafetyCaps.h`, `src/softproxy/SafetyCaps.cpp` | NVS store, mutex, the `Admit` gate, the hub clock and the RTC no-init copy of the counters |
| `src/OtaUpdateManager.cpp`, `src/message_handlers/websocket/gateway/Trigger.cpp` | apply the lockouts |
| `src/CommandHandler.cpp` | `HandleCommand` calls `SafetyCaps::Admit` after the E-Stop check, before the radio. Gateway and serial `rftransmit` commands both pass here |
| `src/serial/command_handlers/safetycaps.cpp` | the `safetycaps` serial command |
| `src/config/Config.cpp` | `FactoryReset` clears the caps |
| `test/native/test_cap_limiter.cpp` | host test of the decision logic, the retained state and the lockout constants |
| `.github/workflows/softproxy-host-test.yml` | runs the host test on pushes and PRs to `softproxy/**` |

Run the host test with:

```sh
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude test/native/test_cap_limiter.cpp -o cap_limiter_test && ./cap_limiter_test
```

## Not covered here

- The hub cannot see a request's purpose, schedule or arm state. Those stay Softproxy server checks 4 to 8.
- Hourly and daily counters do not survive a power cycle. The cold boot hold spaces stages after one, and the server counters stay the authority (see Fail closed).
- RTC no-init retention across a software, panic and watchdog reset is ESP-IDF behaviour, not yet checked on the PiShock boards.
- Raising a cap needs only serial access. Whether to also require the E-Stop to be engaged while caps change is an open operator decision.

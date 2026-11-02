# Softproxy cap enforcement (xoxd-ai fork)

This fork of `OpenShock/Firmware` starts from tag `1.5.2` (commit `5546c13eb745e4fb2b4ca7b0f4160de8b261872c`). It adds one change: the hub checks every shock and vibrate command against operator-set caps before the radio sends it. Softproxy's alarm service checks the same caps first (Softproxy ADR-0008 decision 6, `Device.dhall` server check 3). The hub check is the second line. A step over the intensity, pulse, per-stage or gap caps cannot reach the shocker even when the server sends it. The hourly and daily caps hold across a warm restart, and a power cycle costs a full cold boot hold (see Fail closed). To keep the gateway from removing the check, the fork also refuses over-the-air installs and ignores the gateway restart trigger (see Remote paths). The captive portal's local socket refuses the E-Stop, Wi-Fi and account-link commands (SP-46, see Local socket). The hub refuses shock and vibrate unless a live E-Stop stands behind them, a cap rises only while a button press holds the E-Stop engaged, and a cold boot starts engaged (SP-113, SP-156, SP-158, see Kill path). Each cap or E-Stop refusal reaches the hub log and the gateway heartbeat (see Refusal report). Linear: TIN-7545.

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
- A cold boot (power loss) starts with the E-Stop engaged, and every command refuses until the hold-to-clear (SP-158, see Kill path). It also fails the checksum. The counters start empty and the clock starts at 0, so each channel refuses until the clock reaches the longest of `minGapSec`, 3600 s divided by `maxPerHour` and 86400 s divided by `maxPerDay`, rounded up (`boot-hold`). Every power cycle therefore costs a full hold. A hub that runs between power cycles can still exceed `maxPerDay` across them, so Softproxy's server counters stay the authority for the daily limit.
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

`kLocalSocketAllowed` in `include/softproxy/Lockout.h` is an allow list, now empty, checked in `src/message_handlers/websocket/Local.cpp` before dispatch. A refused command is logged (`Refusing local socket command <name>`) and the client gets the error message `refused_local_command`.

| Local command | Upstream 1.5.2 | This fork | Serial path |
| --- | --- | --- | --- |
| `SetEstopEnabledCommand` | allowed | refused | `estop enabled` |
| `SetEstopPinCommand` | allowed | refused | `estop pin` |
| `WifiScanCommand`, `WifiNetworkSaveCommand`, `WifiNetworkForgetCommand`, `WifiNetworkConnectCommand`, `WifiNetworkDisconnectCommand` | allowed | refused | `networks` |
| `AccountLinkCommand`, `AccountUnlinkCommand` | allowed | refused | `authtoken` |
| The nine `OtaUpdate*` commands | empty handlers | refused, so a later upstream body cannot open an install path | none needed |
| `SetRfTxPinCommand` | allowed | refused (SPR-75): the pin could be moved onto the E-Stop GPIO | `rftxpin` |
| a payload a later release adds | handled | refused until it is added to the list | |

The portal itself still serves the static frontend, and the access point still opens while the gateway is down. It no longer opens because the radio failed while the gateway is connected (`kOpenPortalOnRadioFailure`), since the socket has no repair command to offer.

The socket's defragmenter holds one fragmented message to `kLocalSocketMaxMessageBytes` (4096 bytes, the verifier limit) and uses `TinyVec::try_append`, so an endless fragment stream or a failed allocation drops the message and disconnects the client. Upstream appended with no bound until the `TinyVec` panic restarted the hub, before the allow list saw any message. A client can still spend heap in other ways, so the kill path does not depend on the hub never restarting (see Kill path).

## Kill path

The E-Stop is software on the same ESP32 that drives the radio. `include/softproxy/KillPath.h` holds its rules, and `src/EStopManager.cpp` applies them.

| Rule | Behaviour |
| --- | --- |
| Armed | `IsArmed` needs the checker task running on a valid input pin, with a sample in the last 1000 milliseconds. `HandleCommand` refuses shock and vibrate otherwise (`estop-not-armed`). This covers no pin, a disabled task, a stalled task and a config that fell back to defaults while the caps survived. Stop, Sound and Light still pass when not armed and released |
| Engaged | The latch is one retained value, written only through `estopmanager_writelatch`. It records what set it: `cold-boot`, `gateway` or `button` (SP-156). The first source holds until the hold-to-clear, so a later press or `Trigger` does not restate it. While engaged every command refuses (`estop-engaged`), including Stop, as upstream does. The transmit task ends sequences in flight with their 300 millisecond terminator |
| Release | Only the hold-to-clear releases it: a new press held for 5 seconds, then the release |
| Cold boot (SP-158) | The retained check fails and the hub starts engaged with source `cold-boot`. Every command refuses until the hold-to-clear, so a power cycle with nobody at the hub leaves every command refused until the operator clears it by hand. With no pin stored, nothing can clear it until `estop pin` and `estop enabled true` run. A retained record that fails its check reads the same way |
| Restart | The latch lives in RTC no-init memory, so a software reset, panic or watchdog comes back as it was, with the same source, and `Init` restores it before any command can arrive. A hub that was released comes back released. Starting the task reads the latch and never writes it. A button held across a restart does not count as a new press |
| Gateway `Trigger` `EmergencyStop` | Latches at once, with or without a checker task, with source `gateway`. It allows no cap rise. With no task only a power cycle ends it, and the hub then holds the cold boot latch. While engaged the hub ignores every gateway `Trigger` |
| Serial `estop` | `estop enabled false` and a new `estop pin` refuse while a press or the gateway holds the E-Stop engaged or any cap is above 0. The cold boot latch leaves them open with every cap at 0, because a freshly flashed hub starts there and needs its first pin. No setting releases the latch, and that latch never allows a rise. `safetycaps clear` unlocks them on the cold boot latch at once, and otherwise once the E-Stop is released. `estop enabled true` refuses with no valid pin. `estop state` prints the running state and the source, where `estop enabled` and `estop pin` print the stored config |
| Pin separation (SP-113 rule 1) | The E-Stop pin refuses the stored RF TX pin, the board's default RF TX pin, the board LED pins and, on a plain ESP32, GPIO 34 to 39, which have no internal pull-up. The RF TX pin refuses the stored and running E-Stop pins and the LED pins. The rule reads the pin set at run time, because no `Pishock-*` env defines `OPENSHOCK_ESTOP_PIN` and the two envs differ on RF TX (12 on `Pishock-2023`, 15 on `Pishock-Lite-2021`). `rftxpin`, `estop pin`, `jsonconfig`, `rawconfig`, `SaveFromFlatBuffer` and both `Init` paths all check it. `jsonconfig` and `rawconfig` also set `rf.txPin` and the E-Stop fields, so they obey the same lock as serial `estop` |
| Boot | A stored config that breaks these rules leaves the E-Stop off, which is not armed, so shock and vibrate refuse. A stored RF TX pin on the E-Stop pin or a board LED also leaves the radio off, and `rftxpin` repairs it. The hub still boots, so the serial port stays usable |
| Cap rise (SP-113 rule 2, SP-156) | `SafetyCaps::Set` refuses a write that raises any cap unless the E-Stop is armed and a button press holds it engaged. Armed means a live checker task, and the source must be `button`. A gateway `Trigger` and a cold boot do not count, on an armed hub either, because neither shows that a press reaches the pin. A press during a held latch starts the hold-to-clear and changes no source, so a rise needs a clear and then a press from released. A `button` latch keeps its source through a warm restart, and the settings stay locked, so the pin stays the one that was pressed. A lower `minGapSec` and any change of `stageJoinMs` count as rises, since a shorter join window can turn a command refused `over-pulses` into a new stage. Other lowering and `safetycaps clear` need no E-Stop |

Order at the attended session, with every cap at 0, from the cold boot latch: `estop pin <n>`, `estop enabled true`, check `estop state` reads `source=cold-boot`, the hold-to-clear, press the button, check `source=button`, `safetycaps ...`, then the hold-to-clear. That session waits for Softproxy's allow-listed serial driver, and nobody sends `rftransmit` by hand (SP-162).

Attended tests. The hub E-Stop gets no `independentKill` credit until the review closes and a cable pull, a warm restart and a serial-port reset test all pass at the attended session (SP-157). The rows below are expectations from reading the source, and none has run on a hub. They read state and logs with no vibrate step (SPR-333), so a pass does not show that the E-Stop stops real output.

| Test | Start state | Expected |
| --- | --- | --- |
| Power cycle | any | Engaged with `source=cold-boot`. Every command refuses until the hold-to-clear |
| Cable pull | `cold-boot` latch | Stays engaged with `source=cold-boot`. The hold-to-clear cannot run, and every command refuses |
| Cable pull | `button` latch | Stays engaged with `source=button`. The hold-to-clear cannot run |
| Cable pull | released and armed | Stays armed and released. Nothing detects the pull (see below), so this row records the blind spot |
| Warm restart, serial `restart` | `cold-boot`, `gateway` or `button` latch | Comes back engaged with the same source |
| Warm restart, serial `restart` | released | Comes back released |
| Serial-port reset, opening the port or pulsing EN | each latch and released | Unknown until run. A reset that keeps RTC memory acts as a warm restart. One that loses it acts as a power cycle. An engaged hub must never come back released. Record which one each board does |

What the firmware cannot see:

- The input is pulled up and a press reads low, so the switch must close to ground when pressed. A cut wire or an unplugged cable reads as released, and nothing detects it. A pin strapped low reads as a press, so `source=button` shows a low level on the pin, not a hand on the button. SP-166 keeps a normally open button with this press-reads-low logic and no supervised circuit, so the firmware does not change for it.
- A momentary button leaves the latch to hold the state. A latching button stays pressed: releasing it engages nothing and clears only after the hold.
- The button that engages also clears. SP-167 keeps the hold-to-clear alone, with no second factor, and revisits that at the November 30 audit. A press during the 250 millisecond rearm window is ignored unless still held at its end.

## Refusal report

Before this change a refused step left one log line and nothing else, so Softproxy logged an actuation that never reached the shocker (softproxy #27 review, finding 4). `SafetyCaps::Admit` now counts every cap refusal in `include/softproxy/RefusalReport.h` and writes one log line, and `HandleCommand` does the same for the E-Stop refusals through `SafetyCaps::RecordRefusal`. Gateway and serial `rftransmit` commands both pass there. A Stop, Sound or Light refused by an engaged E-Stop logs `channel=other`.

| Trace | Form | Read by |
| --- | --- | --- |
| Hub log (serial) | `softproxy-refusal seq=<n> channel=<shock\|vibrate\|other> verdict=<name>`, level warning, kept by the production log level `INFO` | Softproxy on the local serial path |
| Gateway heartbeat | `Pong.softproxy_cap_refusals` (`uint32`, field 2, vtable offset 8): refusals since boot, saturating at 16777215. `Pong.softproxy_last_refusal` (`uint8`, field 3, vtable offset 10): verdict code of the latest refusal. `Pong.softproxy_estop` (`uint8`, field 4, vtable offset 12): bit 0 armed, bit 1 engaged | the gateway, on every Ping |
| Serial `safetycaps` | `SafetyCaps\|refusals\|count=<n>,last=<name>` and `SafetyCaps\|estop\|armed=<bool>,engaged=<bool>,source=<released\|cold-boot\|gateway\|button>` after the two channel lines | the operator or Softproxy |
| Hub log on an E-Stop change | `softproxy-estop state=<idle\|active\|active-clearing\|awaiting-release>`, level warning | Softproxy on the local serial path |

The verdict names are those in the cap table above, plus `caps-not-set`, `boot-hold`, `estop-engaged` and `estop-not-armed`. Wire codes: 0 none, 2 `caps-not-set`, 3 `boot-hold`, 4 `over-intensity`, 5 `over-pulse-ms`, 6 `over-pulses`, 7 `gap`, 8 `hourly`, 9 `daily`, 10 `estop-engaged`, 11 `estop-not-armed`. A receiver that sees `softproxy_cap_refusals` rise between two heartbeats knows that many steps were refused. The count lives in RAM and restarts at 0 after any reboot, which a lower `uptime` in the same `Pong` shows.

The three fields come after `uptime` and `rssi`, so a gateway built from the upstream schema reads the heartbeat as before and ignores them. `softproxy/schemas/HubToGatewayMessage.fbs` holds the fork schema. The `schemas` submodule still points at upstream commit `aaceae18`. `include/serialization/_fbs/HubToGatewayMessage_generated.h` is byte-identical to the output of `scripts/flatc` (version 25.9.23) for that fork schema, laid over the submodule, with the C++ flags in `scripts/generate_schemas.py`. `scripts/generate_schemas.py` itself reads only the submodule, so running it unchanged would drop the three fields.

## Who can set the caps

The caps live in their own NVS namespace (`spcaps`), outside the config file. `rawconfig`, `jsonconfig`, the captive portal and the gateway cannot read or change them, and with OTA installs refused the gateway cannot replace the code that reads them. Only the local serial port can:

| Serial command | Effect |
| --- | --- |
| `safetycaps` | print both channels, whether each is set and the stages counted since boot |
| `safetycaps shock <maxIntensityPct> <maxPulseMs> <maxPulsesPerStage> <minGapSec> <maxPerHour> <maxPerDay> [stageJoinMs]` | store the shock caps |
| `safetycaps vibrate ...` | store the vibrate caps, same arguments |
| `safetycaps clear` | set every cap to 0 |

Raising a cap needs the E-Stop armed and engaged by its button (see Kill path). `factoryreset` also clears the caps. The operator sets the values at an attended calibration session (Softproxy SP-10). The real values live only in the Softproxy private registry and never in this repository. Softproxy's alarm service must never send `safetycaps shock` or `safetycaps vibrate`. It may read `safetycaps` and refuse when the hub caps differ from the registry.

## Code

| File | Role |
| --- | --- |
| `include/softproxy/CapLimiter.h` | decision logic, cold boot hold and the retained state seal, no Arduino or ESP-IDF dependency |
| `include/softproxy/Lockout.h` | the OTA install, gateway restart and captive portal lockouts, the local socket allow list and its message bound |
| `include/softproxy/KillPath.h` | the pin rules, the latch sources, the cap rise rule, the settings lock, the E-Stop config rule, the wire code and the retained latch, no Arduino or ESP-IDF dependency |
| `src/EStopManager.cpp` | the latch, the armed state, the checker task and the `estop` refusals |
| `src/WebSocketDeFragger.cpp`, `include/TinyVec.h`, `src/captiveportal/CaptivePortalInstance.cpp` | the message bound on port 81 and the disconnect after an overflow |
| `include/softproxy/RefusalReport.h` | the refusal count, the wire codes and the hub log line, no Arduino or ESP-IDF dependency |
| `include/softproxy/SafetyCaps.h`, `src/softproxy/SafetyCaps.cpp` | NVS store, mutex, the `Admit` gate, the hub clock and the RTC no-init copy of the counters |
| `src/OtaUpdateManager.cpp`, `src/message_handlers/websocket/gateway/Trigger.cpp`, `src/captiveportal/Manager.cpp` | apply the gateway lockouts |
| `src/message_handlers/websocket/Local.cpp` | applies the local socket allow list |
| `src/serialization/WSGateway.cpp`, `softproxy/schemas/HubToGatewayMessage.fbs` | the three `Pong` fields |
| `src/CommandHandler.cpp` | `HandleCommand` calls the E-Stop checks and `SafetyCaps::Admit` before the radio, and `SetRfTxPin` applies the pin rule. Gateway and serial `rftransmit` commands both pass here |
| `src/serial/command_handlers/safetycaps.cpp` | the `safetycaps` serial command |
| `src/config/Config.cpp` | `FactoryReset` clears the caps, and the whole-config writers apply the E-Stop rule |
| `test/native/test_cap_limiter.cpp` | host test of the decision logic, the retained state and the lockout constants. Every cap at 0 is checked against all 256 intensities by all 65536 durations |
| `test/native/test_local_lockdown.cpp` | host test of the local socket allow list, the portal lockout, the refusal count, the wire codes and the log line |
| `test/native/test_command_paths.cpp`, `test/native/stubs/` | host test that links the real `HandleCommand`, `SafetyCaps`, gateway and local dispatchers, `Trigger`, `SetRfTxPinCommand`, serial `rftransmit` and `rftxpin` and the five encoders. Fakes: the hardware edge, and the firmware logic in `EStopManager` (one boolean), `RFTransmitter`, `Config` and `CaptivePortal`. Its header gives the command |
| `test/native/test_kill_path.cpp`, `test/native/kill_stubs/` | host test of the real `EStopManager` task and latch, `HandleCommand`, `SafetyCaps`, `RFTransmitter` and its task, `Sequence`, serial `estop`, `rftxpin` and `safetycaps`, gateway `Trigger` and `WebSocketDeFragger`, built once for each `Pishock-*` RF TX default. Fakes: GPIO, RMT, FreeRTOS, NVS, `Config`, the number parser in `Convert` and `Chipset`. Its header gives the command |
| `.github/workflows/softproxy-host-test.yml` | runs all four host tests on pushes and PRs to `softproxy/**` and on PRs to the cap branch |

Run the first two host tests with:

```sh
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude test/native/test_cap_limiter.cpp -o cap_limiter_test && ./cap_limiter_test
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude test/native/test_local_lockdown.cpp -o local_lockdown_test && ./local_lockdown_test
```

## Not covered here

- The hub cannot see a request's purpose, schedule or arm state. Those stay Softproxy server checks 4 to 8.
- Hourly and daily counters do not survive a power cycle. The cold boot hold spaces stages after one, and the server counters stay the authority (see Fail closed).
- RTC no-init retention across a software, panic and watchdog reset is ESP-IDF behaviour, not yet checked on the PiShock boards. An EN pin reset is not covered. Many boards reach EN from the serial adapter's DTR and RTS, so opening the serial port may reset the hub, and SP-112 makes serial the calibration path. Whether that reset keeps RTC memory is unknown, and the sealed check cannot tell a lost record from a cold boot. See the serial-port reset row under Kill path. This is general ESP32 knowledge, unchecked on these boards.
- The upstream live control gateway (`openshock/live-control-gateway:3.17.0`) does not read the three `Pong` fields. They reach it on the wire, but Softproxy sees them only through a gateway change or by reading the serial log and `safetycaps`.
- The refusal count restarts after every reboot. A refusal just before a reboot shows in the hub log, not in the next heartbeat.
- The TypeScript code generated for the frontend (`frontend/src/lib/_fbs`) still has the upstream `Pong`. The frontend never reads it.
- No `Pishock-*` board environment defines `OPENSHOCK_ESTOP_PIN`, so the E-Stop starts with no pin, not armed, until `estop pin` and `estop enabled` run over serial. Until then and until the hold-to-clear, every command refuses.
- RTC no-init retention of the E-Stop latch is the same unverified ESP-IDF behaviour as the counters above.
- The E-Stop rules are host-tested with a fake GPIO. A switch, a cable, a cable pull and the board are tests for the attended session.
- The host test's fake `Chipset` accepts pins 0 to 39, so `CHIP_UNSAFE_GPIO`, the `OPENSHOCK_BYPASSED_GPIO` entries and the `int8_t` signature of the real functions do not run there. Reading the real header, the rules hold for both PiShock envs. On the wrong env GPIO 12 passes on `Pishock-Lite-2021` and would be the real RF line on a 2023 board, so the hub revision read also covers the pin rule.
- The cold boot latch publishes no state event while no checker task runs, so the LED shows it only after `estop enabled true`. `estop state`, `safetycaps` and the `Pong` field show it from boot.
- The keep-alive task and the `Config.cpp` writers are read, not run: the host test sets `keepAliveEnabled` false and has no filesystem. It tests the decision `KillConfigChangeAllowed` and scans that each writer calls it.
- A serial `rftransmit` command in the host test comes from a fake parser, so `JsonSerial::ParseShockerCommand` is read, not run.
- Stack margin on `main_app` for the refusal path is not profiled. `SealRetained` and `RestoreRetained` no longer put the 2 kilobyte record on the stack. The path still runs `snprintf` and a log call on each refusal.
- Once the hold-to-clear has released the cold boot latch, the image can transmit at every cap 0. Stop becomes a vibrate frame at intensity 0, as do the keep-alive and every terminator, and Sound is uncapped up to 65535 milliseconds. The encoding is non-electrical by source reading only. What a receiver does with a vibrate frame at intensity 0 is unmeasured.

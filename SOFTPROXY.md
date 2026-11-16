# Softproxy cap enforcement (xoxd-ai fork)

This fork of `OpenShock/Firmware` starts from tag `1.5.2` (commit `5546c13eb745e4fb2b4ca7b0f4160de8b261872c`). It adds one change: the hub checks every shock and vibrate command against operator-set caps before the radio sends it. Softproxy's alarm service checks the same caps first (Softproxy ADR-0008 decision 6, `Device.dhall` server check 3). The hub check is the second line. A step over the intensity, pulse, per-stage or gap caps cannot reach the shocker even when the server sends it. The hourly and daily caps hold across a warm restart, and a power cycle that loses RTC memory costs a full cold boot hold (see Fail closed). To keep the gateway from removing the check, the fork also refuses over-the-air installs and ignores the gateway restart trigger, and it refuses shock and vibrate from the gateway, so only the local serial port actuates (see Remote paths). The captive portal's local socket refuses every command, Wi-Fi and account link included (SP-46, see Local socket). Each cap refusal reaches the hub log and the gateway heartbeat (see Refusal report). The fork adds no E-Stop rule: Jess removed the E-Stop work, since there is no physical E-Stop and the safety is that the shocker can be taken off (TIN-7125 `858126ce`, `41d24ddc`, see Removed). Linear: TIN-7545.

## What the hub enforces

| Cap | Hub rule | Refusal |
| --- | --- | --- |
| `maxIntensityPct` | command intensity at most this, and at most 100 | `over-intensity` |
| `maxPulseMs` | command duration at most this | `over-pulse-ms` |
| `maxPulsesPerStage` | commands in one stage at most this | `over-pulses` |
| `minGapSec` | a new stage starts at least this long after the previous stage started | `gap` |
| `maxPerHour` | stage starts in any 60 minutes at most this | `hourly` |
| `maxPerDay` | stage starts in any 24 hours at most this | `daily` |
| `maxPulsesPerStage` times (`maxPulseMs` plus 73) | output time in one run at most this, each command counted with one radio frame (review H1, N2) | `over-on-time` |

Shock and vibrate have separate caps and separate counters. Stop, Sound and Light carry no cap. The keep-alive ping (vibrate at intensity 0) goes straight to the radio and is not counted.

A stage is one Softproxy request. A command starts a new stage unless `stageJoinMs` is above 0 and the command starts within `stageJoinMs` after the previous pulse of the open stage ends. `stageJoinMs` is hub-only and defaults to 0, so every command is its own stage and the gap applies between pulses.

The radio overwrites a shocker's running sequence with each new command for it, so commands that abut or overlap join into one unbroken output, whatever the stages (review H1, TIN-7545 `6fb2f27f`). The hub bounds that output per channel. A run is the channel's output from a start until it stays off for 300 milliseconds, the RF task's terminator time, after which the task has sent the stop frame for its full length and dropped the sequence. A short gap inside a run, under 300 milliseconds, adds no output time and does not end the run.

The RF task sends whole radio frames and starts one whenever a command's end still lies ahead, so a command can stay on the air up to one frame past its end (review N2). Each admitted shock or vibrate command above 0 ms therefore counts as output from the instant `SafetyCaps::Admit` reads the clock, for its duration plus one frame, and time where two commands overlap counts once. The radio ends the command at that same instant plus its duration: `HandleCommand` passes the admitted `millis()` reading to `SendCommand` (review 3 F1). Before that, `SendCommand` set the end from its own later clock read, so a wait at the seal, the refusal record or the transmitter lock moved the output past the counted interval, and the bound held only while that wait stayed under 73 ms less the frame. The keep-alive and a Stop still time from the send. The frame is `kMaxRadioFrameMs` in `include/softproxy/CapLimiter.h`, 73 ms. It bounds the frame of every model the fork encodes: each item of a model's frame at the longest value any id, type and intensity gives it, summed, at the RF task's 1 microsecond tick. The Petrainer's bound is the largest, 72.2 ms, and its longest real frame is 69.2 ms (`src/radio/rmt/PetrainerEncoder.cpp`). The D80's 56.3 ms is the next largest. The command path host test rebuilds the bound from all five encoders and the tick, and fails if the constant drifts. A 0 ms command sends only the stop frame and counts nothing. A command that would take the run's counted time above one full stage counted the same way, `maxPulsesPerStage` times (`maxPulseMs` plus 73), refuses as `over-on-time`. The run ends 300 milliseconds after its last counted end, so the break on the air starts after the last frame.

What that leaves reachable, for `P` = `maxPulseMs` and `N` = `maxPulsesPerStage`:

| Quantity | Bound |
| --- | --- |
| Output on the air in one run, per shocker, any `stageJoinMs` | `N * (P + 73)` ms, however long the send waits after admission. It assumes the RF task starts each frame within 73 ms less that frame of its own end check, at least 0.8 ms at the Petrainer's item bound (I). Before review N2 a sender whose 1 ms commands landed at each frame start could hold it far longer with `stageJoinMs` above 0 |
| Commanded output in one run | `N * P + (N - 1) * 73` ms. A command may fill the frame that counts after an earlier one, so commanded time can pass `N * P` by up to one frame per earlier pulse, while the air stays within the row above |
| Break between runs on the air | at least 300 ms after the last frame, less any delay of the RF task between its end check and the frame that follows it (I) |
| A full stage | passes: `N` pulses of `P` count at most `N * (P + 73)`, which is the ceiling, whatever their spacing |

A pulse now counts as its length plus one frame. A stage whose pulses fit the caps still never reaches the bound by itself. The bound refuses a stage that would continue the previous stage's output with less than 300 milliseconds between them, when the joined output would pass one full stage. Since the counted end lies one frame past the commanded end, two stages join a run when the next starts less than 373 ms after the previous stage's last pulse ends. With `stageJoinMs` at 0 and `maxPulsesPerStage` at 1, stages at the minimum gap all pass only while `maxPulseMs + 373 <= minGapSec * 1000`. Above that, every second stage at the minimum gap refuses as `over-on-time`. Pacing that should never meet `over-on-time` starts each stage at least 373 ms after the previous stage's last pulse ends.

The run is kept per channel and not per shocker, so output to two shockers on one channel counts as one run, which can only refuse sooner per shocker. Each shocker stays within the bound, but two receivers on one channel commanded together each get the counted time, so their sum is twice it (review N4).

## Fail closed

- The fork ships no cap value. Every cap defaults to 0, and a channel with any cap at 0 refuses every command (Softproxy SP-9). Caps that are incoherent (intensity above 100, hourly above daily, `minGapSec * maxPerHour` above 3600, `maxPulseMs` above `minGapSec * 1000`) also refuse, as Softproxy `capsCoherent` does for the first three. The pulse rule is the hub's alone until Softproxy adds it: a pulse longer than the gap let stages started every `minGapSec` join into one output (review H1). A stored record that breaks it loads as 0, and `safetycaps shock` or `safetycaps vibrate` refuses it.
- A missing, unreadable or unknown stored record loads as all 0.
- The counters and the hub clock live in RTC no-init memory, sealed with a checksum after every shock or vibrate command the gate checks. A warm restart (software reset, panic or watchdog) keeps that memory, so the hub restores the counters, the output run and the clock where they were last sealed. The retained layout is version 3. Version 2 added the output run, and version 3 keeps its layout but counts one radio frame per command in the run end and the run total (review N2, review 3 F3). The first boot of this image after an image with an older version reads as a cold boot of the counters and holds. The downtime counts as not passed, so the clock never runs ahead of real time, which can only refuse sooner.
- A cold boot that loses RTC memory, such as a power loss, fails the checksum. The counters then start empty and the clock starts at 0, so each channel refuses until the clock reaches the longest of `minGapSec`, 3600 s divided by `maxPerHour` and 86400 s divided by `maxPerDay`, rounded up (`boot-hold`). One that keeps RTC memory restores the counters and the clock, as a warm restart does, which can only refuse sooner, and it costs no hold. A hub that runs between power cycles can still exceed `maxPerDay` across them, so Softproxy's server counters stay the authority for the daily limit.
- The window counters use fixed buckets (1 minute for the hour, 15 minutes for the day). They can overstate a count by up to one bucket, which refuses sooner, and never understate it.

## Remote paths

Upstream lets the gateway replace the firmware and reboot the hub. Either would undo the hub check: an upstream image carries no caps, and a reboot used to empty the counters. Upstream also lets a linked gateway send shock and vibrate, which pass the hub caps but none of Softproxy's server checks 4 to 8 (review H3). `include/softproxy/Lockout.h` closes all three, whatever the stored config says:

| Path | Upstream 1.5.2 | This fork |
| --- | --- | --- |
| Gateway `OtaUpdateRequest` | queues an install of any version from the CDN | `TryStartFirmwareUpdate` refuses |
| OTA check on startup or on a timer, a changed CDN domain | installs the found version | the OTA task stops before any download |
| Gateway `Trigger` `Restart` | `esp_restart()` | ignored and logged |
| Gateway `ShockerCommandList` Shock or Vibrate | sends within the caps | refused before `HandleCommand` and logged (`Refusing gateway <type>`), at compile time by `kAllowGatewayActuation = false`. The refusal does not count in the refusal report, which counts Softproxy's own steps. A gateway Stop and Sound still pass |
| Gateway `Trigger` `CaptivePortalEnable` | holds the portal open while the gateway is connected | ignored and logged (SP-46). `CaptivePortal::SetAlwaysEnabled(true)` also refuses, whatever the caller |
| Serial flash, serial `restart` | allowed | allowed, both need the local serial port |

## Local socket

The captive portal opens an access point with no password whenever the gateway is unreachable, and its WebSocket on port 81 has no authentication. Upstream lets anyone on that access point change the Wi-Fi networks, move the RF TX pin and link the hub to another account. Softproxy SP-46 (TIN-7125 `fd8d8374`) refuses those commands there. The serial port stays the local path.

`kLocalSocketAllowed` in `include/softproxy/Lockout.h` is an allow list, now empty, checked in `src/message_handlers/websocket/Local.cpp` before dispatch. A refused command is logged (`Refusing local socket command <name>`) and the client gets the error message `refused_local_command`.

| Local command | Upstream 1.5.2 | This fork | Serial path |
| --- | --- | --- | --- |
| `WifiScanCommand`, `WifiNetworkSaveCommand`, `WifiNetworkForgetCommand`, `WifiNetworkConnectCommand`, `WifiNetworkDisconnectCommand` | allowed | refused | `networks` |
| `AccountLinkCommand`, `AccountUnlinkCommand` | allowed | refused | `authtoken` |
| The nine `OtaUpdate*` commands | empty handlers | refused, so a later upstream body cannot open an install path | none needed |
| `SetRfTxPinCommand` | allowed | refused (SPR-75) | `rftxpin` |
| a payload a later release adds | handled | refused until it is added to the list | |

The portal itself still serves the static frontend, and the access point still opens while the gateway is down. It no longer opens because the radio failed while the gateway is connected (`kOpenPortalOnRadioFailure`), since the socket has no repair command to offer.

The socket's defragmenter holds one fragmented message to `kLocalSocketMaxMessageBytes` (4096 bytes, the verifier limit) and uses `TinyVec::try_append`, so an endless fragment stream or a failed allocation drops the message and disconnects the client. Upstream appended with no bound until the `TinyVec` panic restarted the hub, before the allow list saw any message. A client can still spend heap in other ways, so a hub restart stays possible.

## Removed

Jess ruled on November 16, 2026 that the E-Stop work is removed everywhere (TIN-7125 `858126ce`, "Remove everywhere (Recommended)"). There is no physical E-Stop. In `41d24ddc` she added that no other gated safety feature is to be built, because the real safety is that the shocker can be taken off. This fork therefore drops the rules it had added to upstream's E-Stop: SP-113 (RF TX refused on the E-Stop GPIO, caps rise only with the E-Stop engaged), SP-136 and SP-47 (a button drives the E-Stop pin), SP-156 (latch sources), SP-158 (a cold boot starts engaged), SP-172 (the retained latch), SP-174 (the mushroom-up clear), SPR-362 (normally closed contact) and SPR-331. Their code, tests and mutation run are gone, and nothing replaces them.

Upstream 1.5.2's own optional E-Stop stays as upstream ships it, in `src/EStopManager.cpp`, `src/config/EStopConfig.cpp` and serial `estop`. No `Pishock-*` env defines `OPENSHOCK_ESTOP_PIN`, so it starts with no pin and disabled. Shock and vibrate no longer need it.

## Refusal report

Before this change a refused step left one log line and nothing else, so Softproxy logged an actuation that never reached the shocker (softproxy #27 review, finding 4). `SafetyCaps::Admit` now counts every cap refusal in `include/softproxy/RefusalReport.h` and writes one log line. Serial `rftransmit` commands pass there, and so do the gateway commands the lockout lets through, Stop and Sound.

| Trace | Form | Read by |
| --- | --- | --- |
| Hub log (serial) | `softproxy-refusal seq=<n> channel=<shock\|vibrate\|other> verdict=<name>`, level warning, kept by the production log level `INFO` | Softproxy on the local serial path |
| Gateway heartbeat | `Pong.softproxy_cap_refusals` (`uint32`, field 2, vtable offset 8): refusals since boot, saturating at 16777215. `Pong.softproxy_last_refusal` (`uint8`, field 3, vtable offset 10): verdict code of the latest refusal. Field 4 (vtable offset 12) was `softproxy_estop`. It is deprecated and no longer sent | the gateway, on every Ping |
| Serial `safetycaps` | `SafetyCaps\|refusals\|count=<n>,last=<name>` after the two channel lines. It is the last line of the reply | the operator or Softproxy |

The verdict names are those in the cap table above, plus `caps-not-set` and `boot-hold`. Wire codes: 0 none, 2 `caps-not-set`, 3 `boot-hold`, 4 `over-intensity`, 5 `over-pulse-ms`, 6 `over-pulses`, 7 `gap`, 8 `hourly`, 9 `daily`, 12 `over-on-time`. Codes 10 and 11 were the E-Stop verdicts. They are retired, never sent, and read as `unknown`. A receiver that sees `softproxy_cap_refusals` rise between two heartbeats knows that many steps were refused. The count lives in RAM and restarts at 0 after any reboot, which a lower `uptime` in the same `Pong` shows.

The fork fields come after `uptime` and `rssi`, so a gateway built from the upstream schema reads the heartbeat as before and ignores them. `softproxy/schemas/HubToGatewayMessage.fbs` holds the fork schema. The `schemas` submodule still points at upstream commit `aaceae18`. `include/serialization/_fbs/HubToGatewayMessage_generated.h` is byte-identical to the output of `scripts/flatc` (version 25.9.23) for that fork schema, laid over the submodule, with the C++ flags in `scripts/generate_schemas.py`. `scripts/generate_schemas.py` itself reads only the submodule, so running it unchanged would drop the fork fields.

## Who can set the caps

The caps live in their own NVS namespace (`spcaps`), outside the config file. `rawconfig`, `jsonconfig`, the captive portal and the gateway cannot read or change them, and with OTA installs refused the gateway cannot replace the code that reads them. Only the local serial port can:

| Serial command | Effect |
| --- | --- |
| `safetycaps` | print both channels, whether each is set and the stages counted since boot |
| `safetycaps shock <maxIntensityPct> <maxPulseMs> <maxPulsesPerStage> <minGapSec> <maxPerHour> <maxPerDay> [stageJoinMs]` | store the shock caps |
| `safetycaps vibrate ...` | store the vibrate caps, same arguments |
| `safetycaps clear` | set every cap to 0 |

`factoryreset` also clears the caps. The operator sets the values at an attended calibration session (Softproxy SP-10). The real values live only in the Softproxy private registry and never in this repository. Softproxy's alarm service must never send `safetycaps shock` or `safetycaps vibrate`. It may read `safetycaps` and refuse when the hub caps differ from the registry.

## Config and flash

`jsonconfig` with no argument prints the config without the Wi-Fi passwords and the backend auth token (review H6), through upstream's own `GetAsJSON(false)`. Saving that output back with `jsonconfig <json>` stores empty Wi-Fi passwords and an empty auth token, because a missing field reads as empty. The token loss only removes the gateway path. The Wi-Fi loss leaves the hub with no network, so it opens its no-password access point, where port 81 refuses every command (SP-46). Do not write `jsonconfig` output back to change one setting. Set single fields instead, such as `rftxpin <n>` for the RF TX pin. `rawconfig` with no argument still prints the whole binary file, secrets included, because upstream has no path without them: keep no auth token on the hub.

An app-only write onto the vendor partition table is not supported. The fork mounts a LittleFS partition labelled `config` from its own table and cannot parse a vendor config. A vendor table with no such label panics at boot and restarts every 5 seconds (I, by reading `src/config/Config.cpp:144-145` and `include/Logging.h:85-90`, never run, review D2). The plan is the merged write at 0x0. The CI merged image holds 0xFF over the NVS and config partitions, so it erases the stored config and the caps (M, the R3 flashing read, review D3). The stock backup step runs first where the fork allows (SP-137).

## Code

| File | Role |
| --- | --- |
| `include/softproxy/CapLimiter.h` | decision logic, cold boot hold and the retained state seal, no Arduino or ESP-IDF dependency |
| `include/softproxy/Lockout.h` | the OTA install, gateway restart and captive portal lockouts, the local socket allow list and its message bound |
| `src/radio/RFTransmitter.cpp` | `SendCommand` takes the admitted start, so a command ends where the caps counted it |
| `src/WebSocketDeFragger.cpp`, `include/TinyVec.h`, `src/captiveportal/CaptivePortalInstance.cpp` | the message bound on port 81 and the disconnect after an overflow |
| `include/softproxy/RefusalReport.h` | the refusal count, the wire codes and the hub log line, no Arduino or ESP-IDF dependency |
| `include/softproxy/SafetyCaps.h`, `src/softproxy/SafetyCaps.cpp` | NVS store, mutex, the `Admit` gate, the hub clock and the RTC no-init copy of the counters |
| `src/OtaUpdateManager.cpp`, `src/message_handlers/websocket/gateway/Trigger.cpp`, `src/message_handlers/websocket/gateway/ShockerCommandList.cpp`, `src/captiveportal/Manager.cpp` | apply the gateway lockouts |
| `src/message_handlers/websocket/Local.cpp` | applies the local socket allow list |
| `src/serialization/WSGateway.cpp`, `softproxy/schemas/HubToGatewayMessage.fbs` | the two `Pong` fields and the deprecated slot |
| `src/CommandHandler.cpp` | `HandleCommand` calls `SafetyCaps::Admit` before the radio. Serial `rftransmit` commands and the gateway Stop and Sound pass here |
| `src/serial/command_handlers/safetycaps.cpp` | the `safetycaps` serial command |
| `src/config/Config.cpp` | `FactoryReset` clears the caps |
| `test/native/test_cap_limiter.cpp` | host test of the decision logic, the retained state and the lockout constants. Every cap at 0 is checked against all 256 intensities by all 65536 durations |
| `test/native/test_local_lockdown.cpp` | host test of the local socket allow list, the portal lockout, the refusal count, the wire codes and the log line |
| `test/native/test_command_paths.cpp`, `test/native/stubs/` | host test that links the real `HandleCommand`, `SafetyCaps`, gateway and local dispatchers, `Trigger`, `SetRfTxPinCommand`, serial `rftransmit` and `rftxpin` and the five encoders. Fakes: the hardware edge, and the firmware logic in `EStopManager`, `RFTransmitter`, `Config` and `CaptivePortal`. Its header gives the command |
| `test/native/test_radio_path.cpp`, `test/native/radio_stubs/` | host test of the real `HandleCommand`, `SafetyCaps`, `RFTransmitter` and its task, `Sequence` and `WebSocketDeFragger`, built once for each `Pishock-*` RF TX default. Fakes: RMT, FreeRTOS, NVS, `Config`, `EStopManager`, the number parser in `Convert` and `Chipset`. Its header gives the command |
| `.github/workflows/softproxy-host-test.yml` | runs all four host tests on pushes and PRs to `softproxy/**` and on PRs to the cap branch |

Run the first two host tests with:

```sh
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude test/native/test_cap_limiter.cpp -o cap_limiter_test && ./cap_limiter_test
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude test/native/test_local_lockdown.cpp -o local_lockdown_test && ./local_lockdown_test
```

## Not covered here

- The hub cannot see a request's purpose, schedule or arm state. Those stay Softproxy server checks 4 to 8.
- The output bound is per channel. Shock and vibrate each hold one run, so a sender that alternates them on one shocker can keep that shocker's output on longer than either bound, with the electrical part still bounded by the shock run. Two receivers on one channel each stay within the bound, and their sum is twice the counted time (review N4).
- The bound limits unbroken output, not the duty cycle or the dose (review N1). It allows a run of one full stage, then 300 milliseconds off, then another. In the review's model of the RF task, caps `maxPulseMs` 2000, `minGapSec` 2, `maxPulsesPerStage` 1 and `maxPerHour` 1800 kept a shocker on for 87.0 percent of an hour in 2000 ms runs with 300 ms breaks (M in the model, at the commit before review N2). Counting the frame lengthens each break by one frame, and the same model with a sender that retries every millisecond still reaches 85.3 percent with the review's 45 ms frame (M in the model). The hub has no hourly on-time cap. Output per hour is bounded only by `maxPerHour * N * (P + 73)` on the air. Jess ruled on the dose (TIN-7125 `7f50c8e5` item 1): the answer "Accept it as is", with the option text "No dose limit beyond the existing hourly count, pulses per stage and pulse length. You choose small caps at the session." By that ruling the hub bounds unbroken output and keeps no hourly on-time cap. The dose limit is the product of the hourly count, the pulses per stage and the pulse length that Jess sets at the attended session (SP-10), with the gap, the daily cap and Softproxy's server checks.
- Hourly and daily counters do not survive a power cycle. The cold boot hold spaces stages after one, and the server counters stay the authority (see Fail closed).
- RTC no-init retention across a software, panic and watchdog reset is ESP-IDF behaviour, not yet checked on the PiShock boards. Many boards reach EN from the serial adapter's DTR and RTS, so opening the serial port may reset the hub, and SP-112 makes serial the calibration path. The reset reason each board reports for that is unknown. This is general ESP32 knowledge, unchecked on these boards.
- The upstream live control gateway (`openshock/live-control-gateway:3.17.0`) does not read the fork `Pong` fields. They reach it on the wire, but Softproxy sees them only through a gateway change or by reading the serial log and `safetycaps`.
- The refusal count restarts after every reboot. A refusal just before a reboot shows in the hub log, not in the next heartbeat.
- The TypeScript code generated for the frontend (`frontend/src/lib/_fbs`) still has the upstream `Pong`. The frontend never reads it.
- The host test's fake `Chipset` accepts pins 0 to 39, so `CHIP_UNSAFE_GPIO`, the `OPENSHOCK_BYPASSED_GPIO` entries and the `int8_t` signature of the real functions do not run there. A scan pins GPIO 10 in the real ESP32 list.
- A serial `rftransmit` command in the host test comes from a fake parser, so `JsonSerial::ParseShockerCommand` is read, not run.
- Stack margin on `main_app` for the refusal path is not profiled. `SealRetained` and `RestoreRetained` no longer put the 2 kilobyte record on the stack. The path still runs `snprintf` and a log call on each refusal.
- Caps live in NVS, so a re-flash with no erase keeps them. Reading `safetycaps` after a flash is the only guard.
- The image can transmit at every cap 0. Stop becomes a vibrate frame at intensity 0, as do the keep-alive and every terminator, and Sound is uncapped up to 65535 milliseconds. The encoding is non-electrical by source reading only. What a receiver does with a vibrate frame at intensity 0 is unmeasured.

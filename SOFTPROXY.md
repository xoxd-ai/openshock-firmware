# Softproxy fork of OpenShock firmware (xoxd-ai)

This fork of `OpenShock/Firmware` starts from tag `1.5.2` (commit `5546c13eb745e4fb2b4ca7b0f4160de8b261872c`). Softproxy is the hub's gateway: it sends shock and vibrate to the hub over Wi-Fi through a public WSS name, and the hub authenticates with a token of its own (TIN-7125 `ac3567f4`). The hub runs every command it receives as upstream does. The fork adds no cap and no other gate to actuation, because Jess ruled that the PiShock hardware is the limit and the safety is that the shocker can be taken off (TIN-7125 `cf8437c7`, `41d24ddc`). The fork keeps three kinds of change: it refuses over-the-air installs and two gateway triggers, it closes the captive portal's local socket (SP-46) and it bounds the port 81 message size. Linear: TIN-7545, TIN-7125.

## Gateway path

The `Pishock-2023` env connects straight to Softproxy. Other envs keep upstream's three steps.

| Step | Upstream, other envs | `Pishock-2023` |
| --- | --- | --- |
| Check the token | `GET https://<domain>/1/device/self` with header `DeviceToken: <token>`. A 200 links the hub, a 401 waits 5 minutes (`FetchHubInfo`) | skipped. The domain serves no HTTP API, and the edge answers 404 off the hub path |
| Find the gateway | `GET https://<domain>/2/device/assignLCG?version=2`. The JSON answer names `host`, `port` and `path` (`StartConnectingToLCG`) | skipped. Host is the domain, port is `OPENSHOCK_GATEWAY_DIRECT_PORT` 443, path is `OPENSHOCK_GATEWAY_DIRECT_PATH` `/api/v0/hub/ws` |
| Connect | WSS with headers `Firmware-Version` and `Device-Token`, unverified | the same two headers, verified against the pinned roots (see TLS) |

The default domain of `Pishock-2023` is `OPENSHOCK_API_DOMAIN=softproxy-hub.clients.xoxd.ai`, so the hub opens `wss://softproxy-hub.clients.xoxd.ai:443/api/v0/hub/ws`. A stored config keeps its own domain until serial `domain` changes it. Serial `authtoken <token>` sets the hub token, which travels as `Device-Token` on every upgrade. With the direct path both serial commands save without an HTTP check, since the domain has no `/1` or `/1/device/self`. The local socket can no longer link an account (see Local socket), so serial is the only way to set the token.

The version carries `sp-gw1` in its build metadata on `Pishock-2023` (`SOFTPROXY_FW_BUILD_TAG` in `platformio.ini`, applied by `scripts/embed_env_vars.py`). It marks an image whose gateway Shock and Vibrate run. It reaches Softproxy in the `Firmware-Version` header and in `BootStatus.firmware_version.build`.

Keepalive: the hub answers the gateway's `Ping` message with an upstream `Pong`, `uptime` and `rssi` only (`src/message_handlers/websocket/gateway/Ping.cpp`). The WebSocket library answers a WebSocket ping frame with a pong frame on its own (`WSop_ping` in BadWebSockets `WebSockets.cpp`). The `Pishock-2023` env also sets `OPENSHOCK_GATEWAY_PING_INTERVAL_MS=30000`, so `GatewayClient` sends its own WebSocket ping every 30 seconds and drops the connection after two pongs missed by 10 seconds each. The manager then reconnects, at most once every 20 seconds.

A 401 on the upgrade gets no 5 minute backoff, unlike a 401 from `AssignLcg` upstream. The pinned BadWebSockets clears the HTTP status in `clientDisconnect` before it raises `WStype_DISCONNECTED`, so `GatewayClient` cannot tell a 401 from any other failed upgrade. A bad token therefore retries every 20 seconds. The fix needs a BadWebSockets change that keeps the status for the disconnect event.

Extra fixed headers on the upgrade, such as a proxy service token, would go into the `headers` string in `GatewayClient::GatewayClient` in `src/GatewayClient.cpp`, which `setExtraHeaders` sends on every connect. None is added.

### TLS

Upstream 1.5.2 verifies no server certificate. `GatewayClient::connect` calls `beginSSL` with no fingerprint, so the WebSocket library calls `setInsecure()`, and the HTTPS requests go through `HTTPClient::begin(url)` with no CA, which the arduino-esp32 2.x client runs without verification (I, from that library's source, not read in this tree). The build embeds `certificates/x509_crt_bundle`, made by `certificates/gen_crt_bundle.py` from curl's extract of the Mozilla roots (`certificates/cacert.pem`, Mozilla data of December 2, 2025, 144 roots), but no code loads it. Upstream's note in `src/GatewayConnectionManager.cpp` says loading it failed with an allocation error in `esp_crt_bundle_init`.

With `OPENSHOCK_GATEWAY_PINNED_ROOTS` set, as on `Pishock-2023`, `GatewayClient::connect` calls `beginSslWithCA` with `kGatewayRootsPem` from `include/softproxy/GatewayRoots.h`. It holds three roots, each copied byte for byte from `cacert.pem`, which the host test checks:

| Root | Covers | Expires | SHA-256 |
| --- | --- | --- | --- |
| GTS Root R4 | a Google Trust Services chain, such as the WE1 intermediate the proxy uses today | June 22, 2036 | `349DFA4058C5E263123B398AE795573C4E1313C83FE68F93556CD5E8031B3C7D` |
| GTS Root R1 | a Google Trust Services chain under R1 | June 22, 2036 | `D947432ABDE7B7FA90FC2E6B59101B1280E0E1C7E4E40FA3C6887FFF57A7F4CF` |
| ISRG Root X1 | a Let's Encrypt chain | June 4, 2035 | `96BCEC06264976F37460779ACF28C5A7CFE8A3C0AAE11A8FFCEE05C0BDDF08C6` |

No leaf or intermediate is pinned, so a renewal under any of these roots keeps working. The HTTPS requests stay unverified, and with the direct path the hub makes none to the gateway. Open points, all U: whether mbedtls on this board accepts the three concatenated PEM roots, whether it checks validity dates while the hub has no SNTP time, and the live chain of `softproxy-hub.clients.xoxd.ai`, which did not resolve on November 17, 2026. The first bench run settles them.

## Remote paths

`include/softproxy/Lockout.h` closes the paths that would replace the firmware or reboot the hub, whatever the stored config says. They protect which firmware runs, not actuation.

| Path | Upstream 1.5.2 | This fork |
| --- | --- | --- |
| Gateway `ShockerCommandList` Shock, Vibrate, Stop and Sound | runs | runs, as upstream |
| Gateway `OtaUpdateRequest` | queues an install of any version from the CDN | `TryStartFirmwareUpdate` refuses |
| OTA check on startup or on a timer, a changed CDN domain | installs the found version | the OTA task stops before any download |
| Gateway `Trigger` `Restart` | `esp_restart()` | ignored and logged |
| Gateway `Trigger` `CaptivePortalEnable` | holds the portal open while the gateway is connected | ignored and logged (SP-46). `CaptivePortal::SetAlwaysEnabled(true)` also refuses, whatever the caller |
| Gateway `Trigger` `EmergencyStop` | latches the E-Stop | reaches the unknown trigger branch, which logs a warning (see Removed: E-Stop) |
| Serial flash, serial `restart` | allowed | allowed, both need the local serial port |

None of these blocks the gateway path above. The gateway still sends `Stop` in a `ShockerCommandList`.

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

## Removed: E-Stop

Jess ruled on November 16, 2026 that the E-Stop work is removed everywhere (TIN-7125 `858126ce`, "Remove everywhere (Recommended)"). There is no physical E-Stop. In `41d24ddc` she added that no other gated safety feature is to be built, because the real safety is that the shocker can be taken off. This fork therefore drops the rules it had added to upstream's E-Stop: SP-113 (RF TX refused on the E-Stop GPIO, caps rise only with the E-Stop engaged), SP-136 and SP-47 (a button drives the E-Stop pin), SP-156 (latch sources), SP-158 (a cold boot starts engaged), SP-172 (the retained latch), SP-174 (the mushroom-up clear), SPR-362 (normally closed contact) and SPR-331. Their code, tests and mutation run are gone, and nothing replaces them.

Upstream 1.5.2's own optional E-Stop is removed as well, under the same ruling. `EStopManager`, `EStopConfig`, the `Config` E-Stop getters and setters, the serial `estop` command, the port 81 `SetEstopEnabledCommand` and `SetEstopPinCommand` and their results, the `OPENSHOCK_ESTOP_PIN` build flag, the E-Stop LED patterns and the frontend E-Stop pin selector are gone. `HandleCommand`, the gateway trigger handler and the radio task no longer ask an E-Stop state. The gateway's `EmergencyStop` trigger now reaches the unknown trigger branch, which logs a warning and does nothing.

Stored configs still load. `softproxy/schemas/HubConfig.fbs` keeps the `HubConfig.estop` slot as field 6, vtable offset 16, and marks it deprecated, so a config that upstream 1.5.2 saved on a hub verifies and loads, and its E-Stop part is ignored. The `EStopConfig` table stays in that schema only as the type of the slot. A JSON config with an `estop` key also loads, and the key is ignored. `softproxy/schemas/LocalToHubMessage.fbs` and `softproxy/schemas/HubToLocalMessage.fbs` drop the two E-Stop commands and their results. They were the last union members, so every other payload keeps its number. Types 18 and 19 to the hub and 9 and 10 from the hub are retired. The hub answers a retired type as an invalid message. The C++ code in `include/serialization/_fbs` and the TypeScript code in `frontend/src/lib/_fbs` for these three schemas come from `scripts/flatc` 25.9.23 over the submodule with the fork schemas laid on top. `scripts/generate_schemas.py` itself reads only the submodule, so running it unchanged would drop the fork changes.

## Removed: caps

The fork used to check every shock and vibrate against operator-set caps before the radio, refuse shock and vibrate from the gateway and report each refusal in the hub log, the gateway `Pong` and serial `safetycaps`. Jess ruled on November 17, 2026 UTC to remove every software gate, the caps included: "Remove all, caps too", and the caps answer "I am confident in removing the caps as we trust the harware we recived from pishock" (TIN-7125 `cf8437c7`). These are gone: `SafetyCaps`, `CapLimiter`, `RefusalReport`, the `spcaps` NVS namespace and its clear on factory reset, the serial `safetycaps` command, the admitted start that `HandleCommand` passed to `RFTransmitter::SendCommand`, the gateway actuation lockout and their host tests. `HandleCommand` and `SendCommand` are upstream's again, less the E-Stop.

The `Pong` is upstream's again: `uptime` and `rssi`. The fork had added `softproxy_cap_refusals` and `softproxy_last_refusal` and a deprecated slot that was `softproxy_estop`. No upstream gateway read them, and a hub built from this tree sends none of them. `include/serialization/_fbs/HubToGatewayMessage_generated.h` is upstream's file again, and `softproxy/schemas/HubToGatewayMessage.fbs` is removed. A hub that kept caps in NVS from an earlier fork image leaves them unread. The merged image erases NVS.

## Config and flash

`jsonconfig` with no argument prints the config without the Wi-Fi passwords and the backend auth token (review H6), through upstream's own `GetAsJSON(false)`. Saving that output back with `jsonconfig <json>` stores empty Wi-Fi passwords and an empty auth token, because a missing field reads as empty. The token loss removes the gateway path, so Softproxy can no longer reach the hub until serial `authtoken` sets it again. The Wi-Fi loss leaves the hub with no network, so it opens its no-password access point, where port 81 refuses every command (SP-46). Do not write `jsonconfig` output back to change one setting. Set single fields instead, such as `rftxpin <n>` for the RF TX pin. `rawconfig` with no argument still prints the whole binary file, secrets included, because upstream has no path without them. Anyone on the serial port can read the token that way.

An app-only write onto the vendor partition table is not supported. The fork mounts a LittleFS partition labelled `config` from its own table and cannot parse a vendor config. A vendor table with no such label panics at boot and restarts every 5 seconds (I, by reading `src/config/Config.cpp:144-145` and `include/Logging.h:85-90`, never run, review D2). The plan is the merged write at 0x0. The CI merged image holds 0xFF over the NVS and config partitions, so it erases the stored config (M, the R3 flashing read, review D3). The hub then starts with the build's default domain and no auth token. The stock backup step runs first where the fork allows (SP-137).

## Code

| File | Role |
| --- | --- |
| `include/softproxy/Lockout.h` | the OTA install, gateway restart and captive portal lockouts, the local socket allow list and its message bound |
| `src/OtaUpdateManager.cpp`, `src/message_handlers/websocket/gateway/Trigger.cpp`, `src/captiveportal/Manager.cpp` | apply the gateway lockouts |
| `src/message_handlers/websocket/Local.cpp` | applies the local socket allow list |
| `src/WebSocketDeFragger.cpp`, `include/TinyVec.h`, `src/captiveportal/CaptivePortalInstance.cpp` | the message bound on port 81 and the disconnect after an overflow |
| `src/GatewayClient.cpp` | the optional WebSocket ping and the pinned roots on connect |
| `src/GatewayConnectionManager.cpp`, `src/serial/command_handlers/authtoken.cpp`, `src/serial/command_handlers/domain.cpp` | the direct gateway path, `OPENSHOCK_GATEWAY_DIRECT_PATH` |
| `include/softproxy/GatewayRoots.h` | the three pinned roots |
| `scripts/embed_env_vars.py` | adds `SOFTPROXY_FW_BUILD_TAG` to the version's build metadata |
| `platformio.ini` | the `Pishock-2023` gateway settings |
| `softproxy/schemas/HubConfig.fbs`, `softproxy/schemas/LocalToHubMessage.fbs`, `softproxy/schemas/HubToLocalMessage.fbs` | the deprecated `HubConfig.estop` slot and the retired E-Stop payloads (see Removed: E-Stop) |
| `src/serial/command_handlers/jsonconfig.cpp` | prints the config without secrets (review H6) |
| `test/native/test_local_lockdown.cpp` | host test of the local socket allow list, the lockout constants and the pinned roots |
| `test/native/test_command_paths.cpp`, `test/native/stubs/` | host test that links the real `HandleCommand`, gateway and local dispatchers, `Trigger`, `SetRfTxPinCommand`, serial `rftransmit` and `rftxpin` and the five encoders. It checks that every path reaches the radio with the command as sent, and that the local socket and the gateway triggers stay closed. Its header gives the command |
| `test/native/test_radio_path.cpp`, `test/native/radio_stubs/` | host test of the real `HandleCommand`, `RFTransmitter` and its task, `Sequence` and `WebSocketDeFragger`, built once for each `Pishock-*` RF TX default. Its header gives the command |
| `.github/workflows/softproxy-host-test.yml` | runs the three host tests on pushes and PRs to `softproxy/**` and on PRs to the release branch |

Run the lockout and roots test from the repository root with:

```sh
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude test/native/test_local_lockdown.cpp -o local_lockdown_test && ./local_lockdown_test
```

## Not covered here

- Server certificate verification on the HTTPS requests, and the TLS open points, see TLS.
- The 5 minute backoff on a 401 from the upgrade, see Gateway path.
- The hub cannot see a request's purpose or schedule. Softproxy decides what to send.
- The host test's fake `Chipset` accepts pins 0 to 39, so `CHIP_UNSAFE_GPIO`, the `OPENSHOCK_BYPASSED_GPIO` entries and the `int8_t` signature of the real functions do not run there. A scan pins GPIO 10 in the real ESP32 list.
- A serial `rftransmit` command in the host test comes from a fake parser, so `JsonSerial::ParseShockerCommand` is read, not run.
- The direct path, the pinned roots and the WebSocket ping are built and unrun on a board. Whether the proxy counts a ping frame as activity is unmeasured (U).

#!/usr/bin/env bash
# The native host tests of .github/workflows/softproxy-host-test.yml, same sources and flags (TIN-7545).
# Keep this file and that workflow in step.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

fb_rev=8b02fe6178427b96aea25396b53ea4ae8cadd7d8 # the flatbuffers commit platformio.ini pins
fb=.deps/flatbuffers
if [ ! -d "$fb/.git" ]; then
  echo "host-test: fetching flatbuffers ${fb_rev} into ${fb}"
  git init -q "$fb"
  git -C "$fb" fetch -q --depth 1 https://github.com/google/flatbuffers "$fb_rev"
  git -C "$fb" checkout -q FETCH_HEAD
fi
if [ "$(git -C "$fb" rev-parse HEAD)" != "$fb_rev" ]; then
  echo "error: ${fb} is not at ${fb_rev}" >&2
  exit 1
fi

bin="$(mktemp -d "${TMPDIR:-/tmp}/openshock-host-test.XXXXXX")"
trap 'rm -rf "$bin"' EXIT
echo "host-test: $(g++ --version | head -n1)"

echo "host-test 1/3: local lockdown"
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude test/native/test_local_lockdown.cpp -o "$bin/local_lockdown_test"
"$bin/local_lockdown_test"

echo "host-test 2/3: command path"
WS=src/message_handlers/websocket
SRC="src/CommandHandler.cpp $WS/Local.cpp $WS/Gateway.cpp"
SRC="$SRC $WS/gateway/ShockerCommandList.cpp $WS/gateway/Trigger.cpp $WS/local/SetRfTxPinCommand.cpp"
SRC="$SRC src/serial/command_handlers/rftransmit.cpp src/serial/command_handlers/rftxpin.cpp src/radio/rmt/*Encoder.cpp"
# shellcheck disable=SC2086
g++ -std=c++20 -O1 -Itest/native/stubs -Iinclude -I"$fb/include" test/native/test_command_paths.cpp $SRC -o "$bin/command_paths_test"
"$bin/command_paths_test"

echo "host-test 3/3: radio path, RF TX 12 (Pishock-2023) and 15 (Pishock-Lite-2021)"
FLAGS="-std=c++20 -O1 -pthread -DSOFTPROXY_RADIO_TEST -DOPENSHOCK_FW_CHIP_ESP32 -DOPENSHOCK_LED_GPIO=2"
INC="-Itest/native/radio_stubs -Itest/native/stubs -Iinclude -I$fb/include"
SRC="src/CommandHandler.cpp src/WebSocketDeFragger.cpp src/radio/RFTransmitter.cpp"
SRC="$SRC src/radio/rmt/Sequence.cpp src/radio/rmt/*Encoder.cpp"
for TX in 12 15; do
  # shellcheck disable=SC2086
  g++ $FLAGS -DOPENSHOCK_RF_TX_GPIO=$TX $INC -Wl,--wrap=malloc test/native/test_radio_path.cpp $SRC -o "$bin/radio_path_test"
  "$bin/radio_path_test"
done

echo "host-test: all passed"

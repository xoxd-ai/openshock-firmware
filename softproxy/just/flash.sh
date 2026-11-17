#!/usr/bin/env bash
# Write a built merged image at 0x0 with the pinned esptool 5.0.0 (TIN-7545). No confirmation prompt: Jess gives
# the GO as a TIN-7125 comment, and its id is the third argument and goes in the receipt line.
# Usage: flash.sh <env> <port> <go-comment-id>
set -euo pipefail
env_name="${1:?usage: flash.sh <env> <port> <go>}"
port="${2:?usage: flash.sh <env> <port> <go>}"
go="${3:?usage: flash.sh <env> <port> <go>}"
cd "$(git rev-parse --show-toplevel)"

# A Linear comment id: the 8 hex characters the rulings cite, or the full UUID.
if ! [[ "$go" =~ ^[0-9a-f]{8}(-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})?$ ]]; then
  echo "error: go must be a TIN-7125 comment id (8 hex characters or a UUID), got '${go}'" >&2
  exit 2
fi
if [ ! -c "$port" ]; then
  echo "error: ${port} is not a character device" >&2
  exit 2
fi

dir="out/${env_name}"
shopt -s nullglob
images=("$dir"/OpenShock_"${env_name}"_*.bin)
if [ "${#images[@]}" -ne 1 ]; then
  echo "error: expected one ${dir}/OpenShock_${env_name}_*.bin, found ${#images[@]}. Run: just build ${env_name}" >&2
  exit 2
fi
image="${images[0]}"
name="$(basename "$image")"
sha="$(sha256sum "$image" | awk '{print $1}')"
want="$(awk -v n="$name" '$2 == n {print $1}' "$dir/SHA256SUMS")"
if [ "$sha" != "$want" ]; then
  echo "error: ${image} sha256 ${sha} does not match ${dir}/SHA256SUMS (${want:-missing})" >&2
  exit 1
fi
esptool_version="$(esptool version 2>/dev/null | tail -n1)"
if [ "$esptool_version" != "5.0.0" ]; then
  echo "error: esptool ${esptool_version}, want 5.0.0 from flake.nix" >&2
  exit 1
fi

echo "flash: image ${image}"
echo "flash: sha256 ${sha}"
echo "flash: size $(stat -c %s "$image") bytes, commit $(awk '$1 == "commit" {print $2}' "$dir/BUILDINFO")"
echo "flash: port ${port}, GO TIN-7125 ${go}, esptool ${esptool_version}"

started="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
result=ok
esptool --chip esp32 --port "$port" --baud 460800 write-flash 0x0 "$image" || result="failed($?)"

receipt="flash receipt: ${started} host=$(hostname -s) env=${env_name} image=${name} sha256=${sha} port=${port} esptool=${esptool_version} go=TIN-7125/${go} result=${result}"
echo "$receipt"
echo "$receipt" >> "$dir/FLASH_RECEIPTS"
[ "$result" = ok ]

#!/usr/bin/env bash
# Read the whole flash twice with the pinned esptool and keep the image only when both reads match (TIN-7545).
# The image holds the hub's NVS and config, so Wi-Fi credentials and the hub token: it is written 0600, never
# printed, and stays off git and off the tailnet's shared stores unless Jess says otherwise.
# Usage: backup.sh <port> <out-file>
set -euo pipefail
port="${1:?usage: backup.sh <port> <out>}"
out="${2:?usage: backup.sh <port> <out>}"
umask 077

if [ ! -c "$port" ]; then
  echo "error: ${port} is not a character device" >&2
  exit 2
fi
if [ -e "$out" ]; then
  echo "error: ${out} exists, refusing to overwrite a backup" >&2
  exit 2
fi
second="${out}.read2"

echo "backup: read 1 of 2 from ${port} to ${out}"
esptool --chip esp32 --port "$port" --baud 460800 read-flash 0 ALL "$out"
echo "backup: read 2 of 2 from ${port} to ${second}"
esptool --chip esp32 --port "$port" --baud 460800 read-flash 0 ALL "$second"

sha1="$(sha256sum "$out" | awk '{print $1}')"
sha2="$(sha256sum "$second" | awk '{print $1}')"
if [ "$sha1" != "$sha2" ]; then
  echo "backup: READS DIFFER, keeping both: ${out} ${sha1} and ${second} ${sha2}" >&2
  exit 1
fi
rm -f "$second"
printf '%s  %s\n' "$sha1" "$(basename "$out")" > "${out}.sha256"
echo "backup: reads match, ${out} $(stat -c %s "$out") bytes sha256 ${sha1}"

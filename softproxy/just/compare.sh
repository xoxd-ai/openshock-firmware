#!/usr/bin/env bash
# Compare the SHA256SUMS of two build dirs and name each file whose hash differs (TIN-7545).
set -euo pipefail
a="${1:?usage: compare.sh <dir-a> <dir-b>}"
b="${2:?usage: compare.sh <dir-a> <dir-b>}"

if cmp -s "$a/SHA256SUMS" "$b/SHA256SUMS"; then
  echo "identical: $a and $b"
  sed 's/^/  /' "$a/SHA256SUMS"
  exit 0
fi

echo "DIFFERENT: $a and $b"
join -1 2 -2 2 -a 1 -a 2 -e missing -o 0,1.1,2.1 \
  <(LC_ALL=C sort -k2 "$a/SHA256SUMS") <(LC_ALL=C sort -k2 "$b/SHA256SUMS") |
  while read -r name ha hb; do
    if [ "$ha" != "$hb" ]; then
      echo "  differs: $name"
      echo "    a $ha"
      echo "    b $hb"
      if [ -f "$a/$name" ] && [ -f "$b/$name" ]; then
        echo "    first differing bytes (offset, octal a, octal b), up to 10:"
        cmp -l "$a/$name" "$b/$name" 2>/dev/null | head -n 10 | sed 's/^/      /' || true
        echo "    differing byte count: $(cmp -l "$a/$name" "$b/$name" 2>/dev/null | wc -l)"
      fi
    fi
  done
exit 1

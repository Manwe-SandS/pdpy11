#!/bin/sh
# Hidden Micro-11 compatibility: NAME=%n, 'AJ', .#E .@E .B: .A: .R: .E
cd "$(dirname "$0")/micro11" || exit 1
tmp=$(mktemp -d); cp m11.mac "$tmp/"
(cd "$tmp" && "$OLDPWD/../../pdpy11" m11.mac --implicit-bin) >/dev/null 2>&1
fail=0; cmp -s "$tmp/m11.bin" m11.bin.expected || { echo "FAIL: m11"; fail=1; }
rm -rf "$tmp"; echo "micro11: $fail failures"; [ $fail = 0 ]

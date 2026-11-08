#!/bin/sh
# .sav output: make_sav, make_sav with a final address, --sav and -o x.sav.
cd "$(dirname "$0")/sav" || exit 1
P="../../pdpy11"; fail=0
tmp=$(mktemp -d); cp *.mac "$tmp/"
(cd "$tmp" && "$OLDPWD/$P" a.mac && "$OLDPWD/$P" b.mac && "$OLDPWD/$P" c.mac --sav && "$OLDPWD/$P" c.mac -o d.sav) >/dev/null 2>&1
for n in a b c; do cmp -s "$tmp/$n.sav" "$n.sav.expected" || { echo "FAIL: $n.sav"; fail=1; }; done
cmp -s "$tmp/d.sav" c.sav.expected || { echo "FAIL: d.sav"; fail=1; }
(cd "$tmp" && "$OLDPWD/$P" e.mac --link 2000 --bin) >/dev/null 2>&1
cmp -s "$tmp/e.bin" e.bin.expected || { echo "FAIL: --link"; fail=1; }
rm -rf "$tmp"; echo "sav: $fail failures"; [ $fail = 0 ]

#!/bin/sh
# Golden tests: each tests/golden/NAME.mac has NAME.bin.expected (+ optional NAME.out.expected with sorted bare diagnostics).
# Expected files are produced by the Python reference via tools/make_golden.sh; the test itself needs no Python.
cd "$(dirname "$0")/golden" || exit 1
BIN="../../pdpy11"
fail=0; n=0
for src in *.mac; do
  name="${src%.mac}"
  n=$((n+1))
  tmp=$(mktemp -d); tmp=$(cd "$tmp" && pwd -P)
  cp "$src" "$tmp/"
  cp "$name".inc "${name%_*}"_*.inc "$tmp/" 2>/dev/null
  (cd "$tmp" && "$OLDPWD/$BIN" --report-format bare --implicit-bin "$src" >out.txt 2>err.txt; echo $? > rc.txt)
  ok=1
  if [ -f "$name.bin.expected" ]; then cmp -s "$tmp/$name.bin" "$name.bin.expected" || ok=0; fi
  if [ -f "$name.out.expected" ]; then sed "s#$tmp#<W>#g" "$tmp/out.txt" | sort | cmp -s - "$name.out.expected" || ok=0; fi
  if [ -f "$name.rc.expected" ]; then cmp -s "$tmp/rc.txt" "$name.rc.expected" || ok=0; fi
  if [ $ok = 0 ]; then echo "FAIL: $name"; fail=$((fail+1)); fi
  rm -rf "$tmp"
done
echo "golden: $n tests, $fail failures"
[ $fail = 0 ]

#!/bin/sh
# Object files: the .obj written by --compile-obj and the result of --ld must be byte-identical to what Turbo8
# produced (the expected files were made by BKTurbo8.exe under Wine).
cd "$(dirname "$0")/.." || exit 1
tmp=$(mktemp -d)
cp tests/obj/mod_*.mac "$tmp/"
fail=0
for m in a b e; do
  ./pdpy11 --compile-obj --report-format bare "$tmp/mod_$m.mac" >/dev/null 2>&1
  cmp -s "$tmp/mod_$m.obj" "tests/obj/mod_$m.obj.expected" || { echo "obj: FAIL mod_$m.obj"; fail=1; }
done
./pdpy11 --ld "$tmp/ab.bin" --table="$tmp/ab" "$tmp/mod_a.obj" "$tmp/mod_b.obj" >/dev/null 2>&1
cmp -s "$tmp/ab.bin" tests/obj/ab.bin.expected || { echo "obj: FAIL link"; fail=1; }
cmp -s "$tmp/ab_tbl.obj" tests/obj/ab_tbl.obj.expected || { echo "obj: FAIL table"; fail=1; }
[ $fail = 0 ] && echo "objects: ok"
rm -rf "$tmp"
exit $fail

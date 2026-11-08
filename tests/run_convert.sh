#!/bin/sh
# convert-to round trip: the golden macro test, converted to PDPy11 style (macros expanded), must build to the same binary.
cd "$(dirname "$0")/.." || exit 1
tmp=$(mktemp -d)
cp tests/golden/macro_basic.mac tests/golden/macro_basic.inc "$tmp/"
./convert-to PDPY11 "$tmp/macro_basic.mac" 2>/dev/null || { echo "convert-to failed"; exit 1; }
cp "$tmp/macro_basic.mac.pdpy11" "$tmp/conv.mac"
./pdpy11 --implicit-bin "$tmp/conv.mac" >/dev/null 2>&1
if cmp -s "$tmp/conv.bin" tests/golden/macro_basic.bin.expected; then echo "convert-to: ok"; rc=0; else echo "convert-to: FAIL"; rc=1; fi
# Turbo8 source (register synonym, operator precedence, .la) converted to PDPy11 must keep its values
cp tests/convert/turbo_style.asm "$tmp/t.asm"
./convert-to pdpy11 "$tmp/t.asm" 2>/dev/null && cp "$tmp/t.asm.pdpy11" "$tmp/t.mac" && ./pdpy11 --implicit-bin "$tmp/t.mac" >/dev/null 2>&1
if cmp -s "$tmp/t.bin" tests/convert/turbo_style.bin.expected; then echo "convert-to turbo style: ok"; else echo "convert-to turbo style: FAIL"; rc=1; fi
# macros with conditional assembly are expanded to plain code
cp tests/golden/cond_basic.mac "$tmp/cb.mac"
./convert-to pdpy11 "$tmp/cb.mac" 2>/dev/null && cp "$tmp/cb.mac.pdpy11" "$tmp/cc.mac" && ./pdpy11 --implicit-bin "$tmp/cc.mac" >/dev/null 2>&1
if cmp -s "$tmp/cc.bin" tests/golden/cond_basic.bin.expected; then echo "convert-to conditionals: ok"; else echo "convert-to conditionals: FAIL"; rc=1; fi
# Micro-11 source: comments without ';', .+n, TYIN/TOUT, redefined labels
cp tests/convert/micro11.mac "$tmp/mm.mac"
./convert-to --from=micro11 pdpy11 "$tmp/mm.mac" 2>/dev/null && cp "$tmp/mm.mac.pdpy11" "$tmp/mc.mac" && ./pdpy11 --implicit-bin "$tmp/mc.mac" >/dev/null 2>&1
if cmp -s "$tmp/mc.bin" tests/convert/micro11.bin.expected; then echo "convert-to micro11: ok"; else echo "convert-to micro11: FAIL"; rc=1; fi
rm -rf "$tmp"
exit $rc

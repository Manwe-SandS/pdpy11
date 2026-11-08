#!/bin/sh
# play_wav, play_turbo_wav, "~speaker" and --play/--play-turbo: a fake player (PDPY11_PLAYER) copies the file it is given
cd "$(dirname "$0")/sound" || exit 1
P="$(cd ../.. && pwd)/pdpy11"; fail=0
tmp=$(mktemp -d); cp *.mac "$tmp/"
printf '#!/bin/sh\ncp "$1" "%s/played.wav"\n' "$tmp" > "$tmp/player.sh"; chmod +x "$tmp/player.sh"
export PDPY11_PLAYER="$tmp/player.sh"
cd "$tmp" || exit 1
"$P" r.mac >/dev/null 2>&1                                    # ref.wav written by make_wav
"$P" a.mac >/dev/null 2>&1; cmp -s played.wav ref.wav || { echo "FAIL: play_wav"; fail=1; }; rm -f played.wav
"$P" c.mac --play >/dev/null 2>&1; [ -s played.wav ] || { echo "FAIL: --play"; fail=1; }; rm -f played.wav
"$P" c.mac --play-turbo >/dev/null 2>&1; [ -s played.wav ] || { echo "FAIL: --play-turbo"; fail=1; }; rm -f played.wav
"$P" b.mac >/dev/null 2>&1; [ -s played.wav ] || { echo "FAIL: ~speaker"; fail=1; }
PDPY11_PLAYER=/nonexistent "$P" c.mac --play >/dev/null 2>&1 && { echo "FAIL: missing player must fail"; fail=1; }
cd /; rm -rf "$tmp"; echo "sound: $fail failures"; [ $fail = 0 ]

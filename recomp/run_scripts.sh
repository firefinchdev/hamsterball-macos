#!/bin/bash
# recomp/run_scripts.sh [--coverage] [binary]: play every recomp/scripts/*.txt scenario
# (menus, tournaments, party games, pause menu) in a fresh unlocked copy of the game.
# --coverage records HB_COVERAGE into recomp/coverage/<name>.txt (Unicorn build only);
# screenshots and logs go to tests/out/scripts/.
cd "$(dirname "$0")/.."
COV=0; [ "$1" = --coverage ] && { COV=1; shift; }
BIN=${1:-./hbwrap}
OUT=tests/out/scripts; mkdir -p "$OUT"
[ -d tests/out/unlocked ] || python3 tests/make_unlocked.py > /dev/null
for s in recomp/scripts/*.txt; do
  n=$(basename "$s" .txt)
  D="$OUT/game_$n"; rm -rf "$D"; cp -R tests/out/unlocked "$D"
  sed "s|SHOT\([0-9]\)|$OUT/${n}_\1.ppm|" "$s" > "$OUT/$n.script"
  ENV=(HB_WINDOWED=1 HB_SCRIPT="$OUT/$n.script")
  [ $COV = 1 ] && { rm -f "recomp/coverage/$n.txt"; ENV+=(HB_COVERAGE="$PWD/recomp/coverage/$n.txt"); }
  HB_BIN="$BIN" HB_LOG="$OUT/$n.log" HB_DIR="$D" tests/run_test.sh 120 "${ENV[@]}" > /dev/null &
done
wait
for s in recomp/scripts/*.txt; do
  n=$(basename "$s" .txt)
  if grep -q "FATAL" "$OUT/$n.log"; then echo "$n: FAILED: $(grep -m1 FATAL "$OUT/$n.log")"; else echo "$n: ok"; fi
done

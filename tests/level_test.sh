#!/bin/bash
# tests/level_test.sh N [seconds] : Time Trials -> entry N; screenshots + fps log in tests/out
cd "$(dirname "$0")/.."
N=$1; SECS=${2:-70}
OUT=tests/out; D=$OUT/run$N; rm -rf $D; cp -R $OUT/unlocked $D
S=$OUT/s$N.txt
{
echo "280 move 320 462"; echo "300 mdown"; echo "306 mup"
echo "400 key Return"; echo "500 key Down"; echo "520 key Return"
f=600
for i in $(seq 1 $N); do echo "$f key Down"; f=$((f+8)); done
echo "$((f+20)) key Return"
echo "$((f+300)) shot $OUT/l${N}_a.ppm"
echo "$((f+500)) shot $OUT/l${N}_b.ppm"
echo "$((f+520)) hold Up 150"
echo "$((f+700)) shot $OUT/l${N}_c.ppm"
echo "$((f+710)) quit"
} > $S
HB_LOG=$OUT/log$N.txt HB_DIR=$D tests/run_test.sh $SECS HB_WINDOWED=1 HB_SCRIPT=$S HB_FPS=1 > /dev/null
grep -q "FATAL\|invalid memory" $OUT/log$N.txt && echo "level $N: FAILED: $(grep -m1 'FATAL\|invalid memory' $OUT/log$N.txt)" || echo "level $N: ok  fps: $(grep fps $OUT/log$N.txt | awk '{print $2}' | tr '\n' ' ')"

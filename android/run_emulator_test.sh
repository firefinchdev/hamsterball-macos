#!/bin/bash
# android/run_emulator_test.sh [seconds]: build, install and start the app on the connected
# device/emulator, then print its log. Put VAR=value lines in android/hb_env.txt to pass test
# settings (HB_SCRIPT, HB_FPS, ...); they are pushed to the app's external files directory.
set -e
cd "$(dirname "$0")"
SECS=${1:-20}
ADB="${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb"
PKG=dev.firefinch.hamsterball
export JAVA_HOME=${JAVA_HOME:-$(/usr/libexec/java_home -v 17)}
./gradlew -q --no-daemon assembleDebug
"$ADB" install -r app/build/outputs/apk/debug/app-debug.apk >/dev/null
"$ADB" shell am force-stop $PKG
EXT=/sdcard/Android/data/$PKG/files
"$ADB" shell mkdir -p $EXT
if [ -f hb_env.txt ]; then "$ADB" push hb_env.txt $EXT/hb_env.txt >/dev/null; for f in $(grep -o 'HB_SCRIPT=.*' hb_env.txt | cut -d= -f2); do [ -f "$f" ] && "$ADB" push "$f" $EXT/$(basename "$f") >/dev/null; done; else "$ADB" shell rm -f $EXT/hb_env.txt; fi
"$ADB" logcat -c
"$ADB" shell am start -n $PKG/.HamsterBallActivity >/dev/null
sleep "$SECS"
"$ADB" logcat -d 'Hamsterball:*' 'AndroidRuntime:E' 'DEBUG:*' 'libc:F' '*:S' | grep -v '^---'

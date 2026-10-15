#!/bin/bash
# Build the Android APK: dist/HamsterBall-Android-<version>-arm64.apk
# Needs the Android SDK + NDK (Android Studio) and JDK 17. Third-party sources are
# downloaded and Unicorn is cross-built on first use.
set -e
cd "$(dirname "$0")"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
export JAVA_HOME="${JAVA_HOME:-$(/usr/libexec/java_home -v 17)}"
[ -d third_party/src/SDL2 ] || third_party/fetch_deps.sh
[ -f third_party/unicorn-android/lib/arm64-v8a/libunicorn.a ] || third_party/build_unicorn_android.sh
./gradlew -q --no-daemon assembleRelease
VERSION=$(cat ../VERSION)
mkdir -p ../dist
cp app/build/outputs/apk/release/app-release.apk "../dist/HamsterBall-Android-$VERSION-arm64.apk"
echo "built dist/HamsterBall-Android-$VERSION-arm64.apk"

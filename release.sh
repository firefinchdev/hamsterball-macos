#!/bin/bash
# release.sh: build every release artifact for the version in VERSION, into dist/:
#   HamsterBall-macOS-<version>-arm64.zip     (build.sh)
#   HamsterBall-Android-<version>-arm64.apk   (android/build_apk.sh)
#   HamsterBall-Web-<version>.zip             (web/build.sh; unzip and serve with web/serve.py)
# Needs macOS with Homebrew, Android Studio's SDK/NDK and JDK 17 (see the README).
set -e
cd "$(dirname "$0")"
VERSION=$(cat VERSION)
./build.sh
android/build_apk.sh
web/build.sh
rm -f "dist/HamsterBall-Web-$VERSION.zip"
(cd dist && mv web "HamsterBall-Web-$VERSION" && zip -qr "HamsterBall-Web-$VERSION.zip" "HamsterBall-Web-$VERSION" && mv "HamsterBall-Web-$VERSION" web)
echo
echo "release artifacts for $VERSION:"
ls -lh dist/HamsterBall-*"$VERSION"*

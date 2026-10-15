#!/bin/bash
# Assemble dist/Hamster Ball.app from the built ./hbwrap and the game files in game/.
set -e
cd "$(dirname "$0")/.."
VERSION=$(cat VERSION)
[ -f game/hamsterball.exe ] || { echo "game/ is missing (expected game/hamsterball.exe)"; exit 1; }
APP="dist/Hamster Ball.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"

cp hbwrap "$APP/Contents/MacOS/Hamsterball"
python3 packaging/bundle_dylibs.py "$APP" Hamsterball
# pristine game files; copied to ~/Library/Application Support/Hamsterball on first launch
cp -R game "$APP/Contents/Resources/game"
rm -f "$APP/Contents/Resources/game/registry.txt"
rm -rf "$APP/Contents/Resources/game/drive_c"
cp packaging/Hamsterball.icns "$APP/Contents/Resources/"
cp LICENSE THIRD_PARTY_NOTICES.md "$APP/Contents/Resources/"
sed "s/@VERSION@/$VERSION/g" packaging/Info.plist > "$APP/Contents/Info.plist"
codesign --force --deep --sign - "$APP" >/dev/null 2>&1
echo "built $APP ($VERSION)"

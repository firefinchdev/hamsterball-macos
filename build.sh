#!/bin/bash
# Build hamsterball-macos: the compatibility layer, the app bundle and the release zip.
#   ./build.sh            -> dist/Hamster Ball.app and dist/HamsterBall-macOS-<version>.zip
set -e
cd "$(dirname "$0")"

need=()
command -v brew >/dev/null || { echo "Homebrew is required: https://brew.sh"; exit 1; }
for f in sdl2-compat sdl3 libopenmpt cmake pkg-config; do
  brew list --versions $f >/dev/null 2>&1 || need+=($f)
done
if [ ${#need[@]} -gt 0 ]; then
  echo "installing: ${need[*]}"
  brew install "${need[@]}"
fi

[ -f third_party/unicorn/lib/libunicorn.a ] || third_party/build_unicorn.sh
make -j"$(sysctl -n hw.ncpu)"
packaging/make_app.sh

VERSION=$(cat VERSION)
ZIP="dist/HamsterBall-macOS-$VERSION-$(uname -m).zip"
rm -f "$ZIP"
ditto -c -k --keepParent "dist/Hamster Ball.app" "$ZIP"
echo "release artifact: $ZIP"

#!/bin/bash
# Builds a static, x86-only Unicorn 2.1.4 with hbwrap's performance changes.
# Output: third_party/unicorn/{include,lib/libunicorn.a}
# (Unicorn's bundled configure breaks on paths containing spaces, so the build runs in a temp dir.)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
VER=2.1.4
WORK="${TMPDIR:-/tmp}/hbwrap-unicorn-$VER"
mkdir -p "$WORK"
cd "$WORK"
if [ ! -d unicorn-$VER ]; then
  curl -L -o unicorn-$VER.tar.gz https://github.com/unicorn-engine/unicorn/archive/refs/tags/$VER.tar.gz
  tar xzf unicorn-$VER.tar.gz
fi
python3 "$HERE/patch_unicorn.py" unicorn-$VER
cmake -S unicorn-$VER -B build -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=x86 -DBUILD_SHARED_LIBS=OFF -DUNICORN_BUILD_TESTS=OFF >/dev/null
cmake --build build -j8 >/dev/null
rm -rf "$HERE/unicorn"; mkdir -p "$HERE/unicorn/lib"
cp -R unicorn-$VER/include "$HERE/unicorn/include"
# merge the component archives into one static library
libtool -static -o "$HERE/unicorn/lib/libunicorn.a" build/*.a 2>/dev/null
echo "built $HERE/unicorn/lib/libunicorn.a"

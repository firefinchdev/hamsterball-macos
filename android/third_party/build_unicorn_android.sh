#!/bin/bash
# Build the patched Unicorn (x86 only) as a static library for Android arm64-v8a.
# Output: android/third_party/unicorn-android/{include,lib/arm64-v8a/libunicorn.a}
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
NDK="${ANDROID_NDK_HOME:-$(ls -d "$SDK"/ndk/* | sort -V | tail -1)}"
ABI=arm64-v8a
[ -d "$HERE/src/unicorn" ] || "$HERE/fetch_deps.sh"
WORK="${TMPDIR:-/tmp}/hbwrap-unicorn-android"   # Unicorn's configure breaks on paths with spaces
rm -rf "$WORK"; mkdir -p "$WORK"
cp -R "$HERE/src/unicorn" "$WORK/src"
cmake -S "$WORK/src" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-26 \
  -DUNICORN_ARCH=x86 -DBUILD_SHARED_LIBS=OFF -DUNICORN_BUILD_TESTS=OFF >/dev/null
cmake --build "$WORK/build" -j8 >/dev/null
OUT="$HERE/unicorn-android"
rm -rf "$OUT"; mkdir -p "$OUT/lib/$ABI"
cp -R "$WORK/src/include" "$OUT/include"
# merge the component archives into one static library
AR="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-ar"
( cd "$WORK/build" && mkdir -p objs && cd objs && for a in ../*.a; do
    mkdir -p "$(basename $a .a)" && (cd "$(basename $a .a)" && "$AR" x "../$a"); done )
"$AR" rcs "$OUT/lib/$ABI/libunicorn.a" "$WORK"/build/objs/*/*.o
rm -rf "$WORK"
echo "built $OUT/lib/$ABI/libunicorn.a"

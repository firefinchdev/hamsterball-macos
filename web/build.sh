#!/bin/bash
# web/build.sh [ASYNCIFY|JSPI]: build the browser version into dist/web/ (index.html, .js, .wasm, .data).
# ASYNCIFY (default) runs in every current browser; JSPI (dist/web-jspi/) is smaller but needs a
# recent Chrome, Edge or Firefox.
# Needs the Emscripten SDK (build/emsdk, see the README) and the Python venv with Capstone
# that `make recomp-gen` uses. Serve dist/web with web/serve.py.
set -e
cd "$(dirname "$0")/.."
YIELD=${1:-ASYNCIFY}
source build/emsdk/emsdk_env.sh > /dev/null 2>&1
android/third_party/fetch_deps.sh
[ -f build/recomp/gen_table.c ] || make recomp-gen
B=build/web-$YIELD
emcmake cmake -S web -B "$B" -DCMAKE_BUILD_TYPE=Release -DHB_YIELD="$YIELD" > /dev/null
cmake --build "$B" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
OUT=dist/web
[ "$YIELD" = JSPI ] && OUT=dist/web-jspi
rm -rf "$OUT" && mkdir -p "$OUT"
cp "$B"/hamsterball.html "$OUT"/index.html
cp "$B"/hamsterball.js "$B"/hamsterball.wasm "$B"/hamsterball.data "$OUT"/
echo "built $OUT ($(du -sh "$OUT" | cut -f1))"

#!/bin/bash
# web/build.sh [ASYNCIFY|JSPI]: build the browser version into dist/web/ (index.html, .js, .wasm, .data).
# ASYNCIFY (default) runs in every current browser; JSPI (dist/web-jspi/) is smaller but needs a
# recent Chrome, Edge or Firefox.
# Installs what it needs on first use, inside build/: the Emscripten SDK (build/emsdk) and a
# Python venv with Capstone for the recompiler (build/venv). Serve the result with web/serve.py.
# Works on macOS and Linux (the GitHub Actions workflow runs it on Ubuntu).
set -e
cd "$(dirname "$0")/.."
YIELD=${1:-ASYNCIFY}
EMSDK_VERSION=4.0.10
CAPSTONE_VERSION=5.0.6

if [ ! -f build/emsdk/emsdk_env.sh ]; then
  git clone -q https://github.com/emscripten-core/emsdk.git build/emsdk
fi
if [ ! -f build/emsdk/.hb_installed_$EMSDK_VERSION ]; then
  build/emsdk/emsdk install $EMSDK_VERSION > /dev/null
  build/emsdk/emsdk activate $EMSDK_VERSION > /dev/null
  touch build/emsdk/.hb_installed_$EMSDK_VERSION
fi
source build/emsdk/emsdk_env.sh > /dev/null 2>&1

if [ ! -x build/venv/bin/python ] || ! build/venv/bin/python -c "import capstone" 2> /dev/null; then
  python3 -m venv build/venv
  build/venv/bin/pip install -q capstone==$CAPSTONE_VERSION
fi

android/third_party/fetch_deps.sh  # gl4es and libopenmpt sources, shared with the Android build
if [ ! -f build/recomp/gen_table.c ] || [ game/hamsterball.exe -nt build/recomp/gen_table.c ] || \
   [ recomp/recomp.py -nt build/recomp/gen_table.c ] || [ -n "$(find recomp/coverage -newer build/recomp/gen_table.c)" ]; then
  build/venv/bin/python recomp/recomp.py game/hamsterball.exe build/recomp recomp/coverage/*.txt
fi

B=build/web-$YIELD
emcmake cmake -S web -B "$B" -DCMAKE_BUILD_TYPE=Release -DHB_YIELD="$YIELD" > /dev/null
cmake --build "$B" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
OUT=dist/web
[ "$YIELD" = JSPI ] && OUT=dist/web-jspi
rm -rf "$OUT" && mkdir -p "$OUT"
cp "$B"/hamsterball.html "$OUT"/index.html
cp "$B"/hamsterball.js "$B"/hamsterball.wasm "$B"/hamsterball.data "$OUT"/
cp web/serve.py web/README.txt "$OUT"/  # so the folder (and the release zip) can serve itself
echo "built $OUT ($(du -sh "$OUT" | cut -f1))"

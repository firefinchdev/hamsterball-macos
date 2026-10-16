#!/bin/bash
# Download the pinned third-party sources the Android build compiles (into android/third_party/src).
set -e
cd "$(dirname "$0")"
mkdir -p src dl
fetch() {  # name url dir-in-archive
  local name=$1 url=$2 inner=$3
  [ -d "src/$name" ] && return
  curl -fsSL -o "dl/$name.tar.gz" "$url"
  tar xzf "dl/$name.tar.gz" -C dl
  mv "dl/$inner" "src/$name"
  echo "fetched $name"
}
fetch SDL2      https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz SDL2-2.32.10
fetch gl4es     https://github.com/ptitSeb/gl4es/archive/a025615bc79ac43ec911dced8fe5b8eb45a0b72d.tar.gz gl4es-a025615bc79ac43ec911dced8fe5b8eb45a0b72d
fetch libopenmpt https://lib.openmpt.org/files/libopenmpt/src/libopenmpt-0.8.9+release.makefile.tar.gz libopenmpt-0.8.9+release
fetch unicorn   https://github.com/unicorn-engine/unicorn/archive/refs/tags/2.1.4.tar.gz unicorn-2.1.4
python3 ../../third_party/patch_unicorn.py src/unicorn
python3 patch_sdl.py src/SDL2
rm -rf dl

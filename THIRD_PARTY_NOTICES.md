# Third-party software

The releases (macOS app, Android APK, web build) bundle the following libraries. Their sources are
available from the linked projects; in the macOS app, mpg123 (LGPL-2.1) is linked dynamically and can be
replaced in `Contents/Frameworks`.

| Component | Version | License | Source |
|---|---|---|---|
| Unicorn Engine (statically linked, patched by `third_party/patch_unicorn.py`) | 2.1.4 | GPL-2.0 | https://github.com/unicorn-engine/unicorn |
| SDL3 | 3.4 | Zlib | https://github.com/libsdl-org/SDL |
| sdl2-compat | 2.32 | Zlib | https://github.com/libsdl-org/sdl2-compat |
| libopenmpt | 0.8 | BSD-3-Clause | https://lib.openmpt.org |
| libogg / libvorbis | 1.3 | BSD-3-Clause | https://xiph.org |
| mpg123 | 1.33 | LGPL-2.1 | https://mpg123.org |
| SDL2 (Android app) | 2.32.10 | Zlib | https://github.com/libsdl-org/SDL |
| gl4es (Android app) | a025615 | MIT | https://github.com/ptitSeb/gl4es |
| stb_vorbis, minimp3 (bundled with libopenmpt, Android and web) | | Public domain / CC0 | https://lib.openmpt.org |
| gl4es (web build, patched by `android/third_party/patch_gl4es.py`) | a025615 | MIT | https://github.com/ptitSeb/gl4es |
| SDL2 Emscripten port, zlib (web build) | 2.32 / 1.3 | Zlib | https://emscripten.org |
| Emscripten runtime and libc (web build) | 4.0.10 | MIT / UIUC | https://github.com/emscripten-core/emscripten |
| Capstone (used by `recomp/recomp.py` at build time, not shipped) | 5.0.6 | BSD-3-Clause | https://www.capstone-engine.org |

Hamsterball and its game data (`game/`) are © Raptisoft Games.

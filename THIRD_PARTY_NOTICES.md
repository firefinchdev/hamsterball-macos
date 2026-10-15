# Third-party software

The release app bundles the following libraries. Their sources are available from the linked projects;
mpg123 (LGPL-2.1) is linked dynamically and can be replaced in `Contents/Frameworks`.

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
| stb_vorbis, minimp3 (bundled with libopenmpt, Android app) | | Public domain / CC0 | https://lib.openmpt.org |

Hamsterball and its game data (`game/`) are © Raptisoft Games.

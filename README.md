# Hamster Ball for macOS, Android and the web

Play **Hamsterball** (Raptisoft, 2004), the Windows game where you roll a hamster in a ball through
obstacle courses, on Apple Silicon Macs, Android phones and tablets, and in web browsers. It doesn't
use Wine or a virtual machine.

Instead, this project is a small purpose-built compatibility layer. It runs the original
`hamsterball.exe` and replaces the Windows APIs the game uses (Direct3D 8, DirectInput, DirectSound
and friends) with portable equivalents built on SDL, OpenGL and libopenmpt. On the Mac and Android the
game's x86 code runs in an x86 CPU emulator; for the browser it is translated to C ahead of time and
compiled to WebAssembly.

The game files are in `game/` and are embedded in every build. **This repository and everything built
from it contain the game, so they must stay private**: don't make the repository public, and only
publish the website privately (see [Publishing the website](#publishing-the-website)).

| Platform | Release artifact | Runs on |
|---|---|---|
| [macOS](#macos) | `HamsterBall-macOS-<version>-arm64.zip` | Apple Silicon Macs, recent macOS |
| [Android](#android) | `HamsterBall-Android-<version>-arm64.apk` | 64-bit ARM phones and tablets, Android 8.0+ |
| [Web](#web-browser) | `HamsterBall-Web-<version>.zip` | current Chrome, Edge, Firefox, Safari (desktop and mobile) |

Download them from [Releases](../../releases).

## Features

- Every race, Tournament, Time Trials, Mirror Tournament and Party Games (split-screen and 4-player Rodent Rumble)
- Full speed: ~88 FPS, the game's own simulation rate (shown in full on 120 Hz displays), with the 3D scenes
  rendered at up to 4× the original resolution (2× on Android and the web)
- Mouse steering (captured pointer, like the original), keyboard, touch (Android and web), and **gamepads**
  (PS5 DualSense, PS4, Xbox, MFi): left stick or d-pad steers and drives the menus, **Start** pauses
- Music (MO3 soundtrack) and sound effects
- All levels and game modes unlocked on first launch

## macOS

1. Download `HamsterBall-macOS-<version>-arm64.zip` and unzip it.
2. Move **Hamster Ball.app** to Applications and open it.
3. The app isn't notarized by Apple. If macOS blocks it, go to **System Settings → Privacy & Security → Open Anyway**,
   or run `xattr -dr com.apple.quarantine "/Applications/Hamster Ball.app"`.

On first launch the game files are copied to `~/Library/Application Support/Hamsterball/game`, where
saves and settings are kept. Delete that folder to start over. If something goes wrong, the log is in
`~/Library/Logs/Hamsterball.log`.

The release is built against current Homebrew libraries, so it needs a recent macOS. If it won't start on
your version, [build it yourself](#building).

## Android

Install `HamsterBall-Android-<version>-arm64.apk` by opening it on the device (allow installing from that
source), or with `adb install`.

- Landscape, letterboxed to the game's 4:3.
- **Drag a finger** to roll the ball; the screen acts like a trackball. **Tap** to select in menus.
- **Back** pauses (it's the game's Escape key). A **two-finger tap** toggles the on-screen keyboard, for typing high-score names.
- **Bluetooth/USB mouse and keyboard** work as on a PC. While you steer, the app captures the mouse pointer, so
  the ball keeps rolling past the screen edge. A physical keyboard can type high-score names directly.
- **Gamepads** paired with the device drive menus, races and Party Games. Pair them before starting the app.
- Game data and saves live in the app's private storage.

## Web browser

The browser version runs at full speed in any current browser, including on phones and tablets. You host
it yourself, privately: on your home network, or on a private GitHub Pages site.

To play on your home network, unzip `HamsterBall-Web-<version>.zip` (or build `dist/web` yourself) and run:

```sh
python3 serve.py             # in the unzipped folder; from a checkout: python3 web/serve.py
```

It prints the address to open, e.g. `http://192.168.1.41:8080/`. Open it on any device on the same Wi-Fi and
press **Play**.

- **Mouse** steers. The pointer is captured while you roll; **Esc** releases it and pauses. Click to capture it again.
- Arrow keys work too. On phones and tablets, **drag a finger** like on Android.
- **Fullscreen** button in the top-right corner.
- Saves, settings and high scores stay in that browser (IndexedDB), separately on each device.
- **Gamepads**: browsers only allow them on secure pages, so serve with `python3 serve.py --https`. It uses a
  self-signed certificate; each device shows a warning once, which you accept.

### Publishing the website

`.github/workflows/website.yml` builds the browser version on every push to `main` (and on demand from the
Actions tab) and can publish it with GitHub Pages. Because the site contains the game files:

- the build only runs while the repository is **private**, and keeps `dist/web` as a private workflow artifact;
- publishing is **off** until the repository variable `PUBLISH_PAGES` is set to `true`.

To publish, once the repository belongs to an organization on **GitHub Enterprise Cloud**:

1. **Settings → Pages**: set **Source** to **GitHub Actions** and **visibility** to **Private** (only people
   with read access to the repository can open the site, after signing in to GitHub).
2. **Settings → Secrets and variables → Actions → Variables**: add `PUBLISH_PAGES` = `true`.
3. Run the **Website** workflow (or push to `main`).

Don't enable Pages on a repository whose Pages visibility can't be private (personal accounts, or
organizations without Enterprise Cloud): the site would be public.

## Playing

| | Mac | Android | Web |
|---|---|---|---|
| Steer | Mouse or arrow keys | Drag a finger, mouse, arrow keys | Mouse, arrow keys, drag a finger |
| Pause | Esc (or right mouse button) | Back | Esc |
| Fullscreen | Cmd+F or Alt+Enter | always | Fullscreen button |
| Quit | Cmd+Q | Home | close the tab |
| Gamepad | Connect it before launching. Left stick or d-pad steers and navigates menus, A selects, **Start** (or Back/Select) pauses. In **Party Games**, select a player row and press Return to cycle that player's input to the controller. | same | same (over HTTPS) |

## Building

`./release.sh` builds all three release artifacts for the version in `VERSION` into `dist/`. Each platform
also builds on its own:

| Platform | Command | Output | Needs |
|---|---|---|---|
| macOS | `./build.sh` | `dist/Hamster Ball.app`, `dist/HamsterBall-macOS-<version>-arm64.zip` | Homebrew; installs `sdl2-compat sdl3 libopenmpt cmake pkg-config`, builds a patched Unicorn |
| Android | `android/build_apk.sh` | `dist/HamsterBall-Android-<version>-arm64.apk` | Android Studio's SDK + NDK 28, JDK 17; downloads SDL2, gl4es, libopenmpt and Unicorn |
| Web | `web/build.sh` | `dist/web/` | Python 3, CMake; installs the Emscripten SDK and Capstone into `build/` on first use |

```sh
git clone git@github.com:firefinchdev/hamsterball-macos.git
cd hamsterball-macos
./release.sh
```

For development on the Mac, run `make && ./hbwrap game`. That plays straight from `game/`, which writes
`registry.txt` and `drive_c/` there; both are git-ignored.

## How it works

| Layer | What it does | Source |
|---|---|---|
| CPU (Mac, Android) | Runs the 32-bit x86 code with [Unicorn](https://www.unicorn-engine.org) (QEMU's JIT), statically linked and patched for speed | `src/emu.cpp`, `third_party/` |
| CPU (web) | The game's code translated to C ahead of time (see below) | `recomp/`, `src/recomp_*` |
| Loader | Maps the PE image and points its import table at host "thunks" | `src/emu.cpp` |
| kernel32 | Memory/heap, files (sandboxed to the game folder), resources, TLS, time | `src/kernel32.cpp` |
| user32/gdi32 | One SDL window, message queue, mouse/keyboard/touch messages, timers, message boxes | `src/user32.cpp` |
| Direct3D 8 | Fixed-function pipeline on OpenGL 2.1 (on Android and the web through [gl4es](https://github.com/ptitSeb/gl4es) to OpenGL ES 2 / WebGL): textures (incl. DXT), lighting, texture-stage combiners, fog, state blocks, supersampling | `src/d3d8.cpp` |
| DirectInput 8 | Keyboard and mouse from SDL; gamepads via SDL's GameController API | `src/dinput.cpp` |
| DirectSound + BASS | Software mixer; the MO3 soundtrack is played with libopenmpt | `src/audio.cpp` |
| Registry, Winsock, … | Registry persisted to `registry.txt`; networking reports "offline" | `src/misc.cpp` |
| Browser glue | Yielding to the page, saves in IndexedDB | `src/web.cpp`, `web/` |

Each Windows function the game imports is a small C++ function registered with the
`API(dll, name, nargs)` or `COM(Interface, Method, nargs)` macros. The thunk runs it and returns to
the game with a normal `ret n`. Callbacks into the game (window procedures, enumeration callbacks) use
nested emulation.

Unicorn is patched (`third_party/patch_unicorn.py`) in three ways: it no longer checks for an exit request
after every guest memory access, it skips self-modifying-code tracking on writes, and it starts with a larger
TLB. Together these make the heaviest levels about 6× faster.

### Android specifics

- SDL2's Android activity hosts the native code (`android/`).
- The game files ship as APK assets and are extracted on first launch.
- `android/third_party/patch_sdl.py` keeps full keyboards that advertise a d-pad from being treated as gamepads.

### The browser build: static recompilation

A browser can't run Unicorn: Unicorn translates x86 code into machine code while the game runs, and web pages
aren't allowed to generate machine code. So for the browser the game's code is **translated ahead of time**:

- `recomp/recomp.py` (the static recompiler) finds every function in `hamsterball.exe` and writes each one as a C
  function, in `build/recomp/`. Registers and flags become C locals (carried through calls in a shared struct),
  so the compiler drops flag computations nobody reads. Calls through pointers go via a lookup table, and
  Windows API calls go straight to the same C++ implementations the other versions use.
- It finds code by following calls and jumps from the entry point, scanning the data sections for pointers into
  the code (C++ vtables, callbacks), decoding switch jump tables, and using coverage recorded by playing the game
  under Unicorn (`recomp/coverage/`, recorded by `recomp/run_scripts.sh --coverage` and `tests/level_test.sh`).
- The translated build reports an x87-only CPU, so the game's MMX/SSE/3DNow! code paths are never taken.
  Guest `setjmp`/`longjmp` map onto host `setjmp`/`longjmp`.
- [Emscripten](https://emscripten.org) compiles everything to WebAssembly. Rendering goes through gl4es to WebGL
  (`android/third_party/patch_gl4es.py` adds WebGL's packed depth/stencil), music through libopenmpt, and the
  game's endless main loop yields to the page at each frame and `Sleep()` (Asyncify; `web/build.sh JSPI` builds a
  smaller variant for recent Chrome, Edge and Firefox).

The translated code also builds natively, which is how it's tested: `make recomp-gen && make hbwrap-recomp` makes
`./hbwrap-recomp`, a Mac build without Unicorn.

## Testing and debugging

| Tool | What it does |
|---|---|
| `tests/level_test.sh N` | plays Time Trials race N with scripted input, logs FPS, takes screenshots (`HB_BIN=./hbwrap-recomp` for the translated build) |
| `recomp/run_scripts.sh [binary]` | plays the menu, tournament, Party Games and pause scenarios in `recomp/scripts/` |
| `tests/web_test.py` | runs the browser build in headless Chrome with scripted input, screenshots and a CPU profile |
| `android/run_emulator_test.sh` | installs a debug APK on a connected device or emulator and prints its log |

| Variable | Effect |
|---|---|
| `HB_WINDOWED=1` | ignore the game's fullscreen setting |
| `HB_VSYNC=1` | sync presentation to the display (default off: the game draws ~88 FPS, its simulation rate) |
| `HB_RENDER_SCALE=n` | internal render scale (default: matches the display, max 4) |
| `HB_SCALE=n` | initial window size multiplier |
| `HB_MOUSE_SENS=f` | extra mouse sensitivity factor during play |
| `HB_TRACE=1` | log every API call |
| `HB_FPS=1` | log frames per second |
| `HB_STATS=1` | print API call counts on exit |
| `HB_SCRIPT=file` | scripted input and screenshots for automated tests (see `src/script.cpp`) |
| `HB_VIRTUAL_PAD=1` | attach a virtual gamepad, driven by `pad` script commands |
| `HB_AUDIO_DUMP=file` | write the mixed audio as raw float32 stereo |
| `HB_COVERAGE=file` | record executed code for the recompiler (Unicorn builds) |
| `HB_X87_ONLY=1` | make the Unicorn build report an x87-only CPU, like the translated build |

The tests use a copy of `game/` with everything unlocked (`python3 tests/make_unlocked.py`). On Android the
variables go in `android/hb_env.txt`; on the web, as `?hbenv=VAR=value` URL parameters.

## Legal

The compatibility layer is licensed under the [GNU GPL v2](LICENSE), because it statically links the GPL-2.0
Unicorn engine. Bundled third-party libraries are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Hamsterball and its game data in `game/` are © Raptisoft Games.

# Hamster Ball for macOS

Play **Hamsterball** (Raptisoft, 2004), the Windows game where you roll a hamster in a ball through
obstacle courses, natively on Apple Silicon Macs. It doesn't use Wine or a virtual machine.

Instead, this project is a small purpose-built compatibility layer. It runs the original
`hamsterball.exe` with an x86 CPU emulator and replaces the Windows APIs the game uses (Direct3D 8,
DirectInput, DirectSound and friends) with macOS equivalents.

The game files are in `game/` and are embedded in the app.

## Features

- Every race, Tournament, Time Trials, Mirror Tournament and Party Games (split-screen and 4-player Rodent Rumble)
- Full speed on Apple Silicon (~88 FPS, the game's own simulation rate; shown in full on 120 Hz ProMotion displays), with the 3D scenes rendered at up to 4× the original resolution for sharp Retina output
- Mouse steering (captured relative mouse, like the original), keyboard, and **gamepads paired with macOS**
  (PS5 DualSense, PS4, Xbox, MFi) as Party Games players
- Music (MO3 soundtrack) and sound effects
- Fullscreen or windowed (**Cmd+F** / Alt+Enter); **Cmd+Q** quits
- All levels and game modes unlocked on first launch

## Install

1. Download `HamsterBall-macOS-<version>-arm64.zip` from [Releases](../../releases) and unzip it.
2. Move **Hamster Ball.app** to Applications and open it.
3. The app isn't notarized by Apple. If macOS blocks it, go to **System Settings → Privacy & Security → Open Anyway**,
   or run `xattr -dr com.apple.quarantine "/Applications/Hamster Ball.app"`.

On first launch the game files are copied to `~/Library/Application Support/Hamsterball/game`, where
saves and settings are kept. Every level and game mode starts unlocked.

**Requirements:** an Apple Silicon Mac. The release is built against current Homebrew libraries,
so it needs a recent macOS. If it won't start on your version, [build it yourself](#build-from-source).

## Android

`HamsterBall-Android-<version>-arm64.apk` runs the same compatibility layer on 64-bit ARM Android phones and
tablets (Android 8.0 or newer). Install it by opening the APK on the device (allow installing from that source),
or with `adb install`.

- Landscape, letterboxed to the game's 4:3.
- **Drag a finger** to roll the ball; the screen acts like a trackball. **Tap** to select in menus.
- **Back** pauses (it's the game's Escape key). A **two-finger tap** toggles the on-screen keyboard, for typing high-score names.
- **Bluetooth/USB mouse and keyboard** work as on a PC. While you steer, the app captures the mouse pointer, so the ball keeps rolling past the screen edge. A physical keyboard can type high-score names directly.
- **Gamepads** (PS5 DualSense, PS4, Xbox, …) paired with the device drive menus, races and Party Games, as on the Mac. Pair them before starting the app.
- All levels and modes start unlocked. Game data and saves live in the app's private storage.

How the port differs from macOS:
- The renderer's desktop OpenGL calls go through [gl4es](https://github.com/ptitSeb/gl4es) to OpenGL ES 2.
- SDL2's Android activity hosts the native code.
- The game files ship as APK assets and are extracted on first launch.

Build it with `android/build_apk.sh`. It needs Android Studio's SDK + NDK and JDK 17, downloads SDL2, gl4es,
libopenmpt and Unicorn, and writes the APK to `dist/`. `android/run_emulator_test.sh` installs a debug build on a
connected device or emulator and prints its log.

## Web browser

The browser version runs the game in any current browser (Chrome, Edge, Firefox, Safari, including phones and
tablets) at full speed. You host it yourself on your home network: the build contains the game files, so keep it
on your own machines and don't put it on a public website.

```sh
web/build.sh             # builds dist/web/ (about 33 MB)
python3 web/serve.py     # prints the address to open, e.g. http://192.168.1.41:8080/
```

Open the printed address on any device on the same Wi-Fi and press **Play**.

- **Mouse** steers. The pointer is captured while you roll; **Esc** releases it and pauses. Click to capture it again.
- Arrow keys work too. On phones and tablets, **drag a finger** like on Android.
- **Fullscreen** button in the top-right corner.
- Saves, settings and high scores stay in that browser (IndexedDB), separately on each device.
- **Gamepads**: browsers only allow them on secure pages, so serve with `python3 web/serve.py --https`. It uses a
  self-signed certificate; each device shows a warning once, which you accept.

`web/build.sh JSPI` builds a smaller variant (`dist/web-jspi/`) that needs a recent Chrome, Edge or Firefox.

### How the browser build works

A browser can't run Unicorn: Unicorn translates x86 code into machine code while the game runs, and web pages
aren't allowed to generate machine code. So for the browser the game's code is **translated ahead of time**:

- `recomp/recomp.py` (the static recompiler) finds every function in `hamsterball.exe` and writes each one as a C
  function, in `build/recomp/`. Registers and flags become C locals, so the compiler drops flag computations
  nobody reads. Calls through pointers go via a lookup table, and Windows API calls go straight to the same C++
  implementations the other versions use.
- It finds code by following calls and jumps from the entry point, scanning the data sections for pointers into
  the code (C++ vtables, callbacks), decoding switch jump tables, and using coverage recorded by playing the game
  under Unicorn (`recomp/coverage/`, recorded by `recomp/run_scripts.sh --coverage` and `tests/level_test.sh`).
- The translated build reports an x87-only CPU, so the game's MMX/SSE/3DNow! code paths are never taken.
  Guest `setjmp`/`longjmp` map onto host `setjmp`/`longjmp`.
- [Emscripten](https://emscripten.org) compiles everything to WebAssembly. Rendering goes through gl4es to WebGL,
  music through libopenmpt, and the game's endless main loop yields to the page at each frame and `Sleep()`
  (Asyncify, or JSPI in the variant build).

The translated code also builds natively, which is how it's tested: `make recomp-gen && make hbwrap-recomp` makes
`./hbwrap-recomp`, a Mac build without Unicorn. `HB_BIN=./hbwrap-recomp tests/level_test.sh N` and
`recomp/run_scripts.sh ./hbwrap-recomp` play races, menus, tournaments and Party Games on it.

Building needs the Emscripten SDK in `build/emsdk`:

```sh
git clone https://github.com/emscripten-core/emsdk.git build/emsdk
build/emsdk/emsdk install 4.0.10 && build/emsdk/emsdk activate 4.0.10
python3 -m venv build/venv && build/venv/bin/pip install capstone==5.0.6   # for recomp/recomp.py
```

`tests/web_test.py` runs the browser build in headless Chrome with scripted input and takes screenshots.

## Playing

| | |
|---|---|
| Steer | Mouse, or the arrow keys |
| Pause | Esc (or right mouse button) |
| Fullscreen (Mac) | Cmd+F or Alt+Enter |
| Quit | Cmd+Q |
| Gamepad | Connect it before launching. Left stick or d-pad steers and navigates menus, A selects, **Start** (or Back/Select) pauses. In **Party Games**, select a player row and press Return to cycle that player's input to the controller. |

Saves, settings and high scores live in `~/Library/Application Support/Hamsterball/game`.
Delete that folder to start over. If something goes wrong,
the log is in `~/Library/Logs/Hamsterball.log`.

## Build from source

```sh
git clone git@github.com:firefinchdev/hamsterball-macos.git
cd hamsterball-macos
./build.sh
```

`build.sh` installs the Homebrew dependencies it needs (`sdl2-compat sdl3 libopenmpt cmake pkg-config`),
downloads and builds a patched Unicorn, compiles the compatibility layer, and produces
`dist/Hamster Ball.app` (with `game/` embedded) and the release zip.

For development, run `make && ./hbwrap game`. That plays straight from `game/`, which writes
`registry.txt` and `drive_c/` there; both are git-ignored.

## How it works

| Layer | What it does | Source |
|---|---|---|
| CPU | Runs the 32-bit x86 code with [Unicorn](https://www.unicorn-engine.org) (QEMU's JIT), statically linked and patched for speed | `src/emu.cpp`, `third_party/` |
| Loader | Maps the PE image and points its import table at host "thunks" | `src/emu.cpp` |
| kernel32 | Memory/heap, files (sandboxed to the game folder), resources, TLS, time | `src/kernel32.cpp` |
| user32/gdi32 | One SDL window, message queue, mouse/keyboard messages, timers, message boxes | `src/user32.cpp` |
| Direct3D 8 | Fixed-function pipeline on OpenGL 2.1: textures (incl. DXT), lighting, texture-stage combiners, fog, state blocks, supersampling | `src/d3d8.cpp` |
| DirectInput 8 | Keyboard and mouse from SDL; gamepads via SDL's GameController API | `src/dinput.cpp` |
| DirectSound + BASS | Software mixer; the MO3 soundtrack is played with libopenmpt | `src/audio.cpp` |
| Registry, Winsock, … | Registry persisted to `registry.txt`; networking reports "offline" | `src/misc.cpp` |

Each Windows function the game imports is a small C++ function registered with the
`API(dll, name, nargs)` or `COM(Interface, Method, nargs)` macros. The thunk runs it and returns to
the game with a normal `ret n`. Callbacks into the game (window procedures, enumeration callbacks) use
nested emulation.

Unicorn is patched (`third_party/patch_unicorn.py`) in three ways:
- it no longer checks for an exit request after every guest memory access,
- it skips self-modifying-code tracking on writes,
- it starts with a larger TLB.

Together these make the heaviest levels about 6× faster.

## Debugging

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

`tests/level_test.sh N` plays Time Trials race N in a copy of `game/` with everything unlocked
(create that copy with `python3 tests/make_unlocked.py`).

## Legal

The macOS compatibility layer is licensed under the [GNU GPL v2](LICENSE), because it statically links
the GPL-2.0 Unicorn engine. Bundled third-party libraries are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Hamsterball and its game data in `game/` are © Raptisoft Games.

# Hamster Ball for macOS

Play **Hamsterball** (Raptisoft, 2004), the Windows game where you roll a hamster in a ball through
obstacle courses, natively on Apple Silicon Macs. It doesn't use Wine or a virtual machine.

Instead, this project is a small purpose-built compatibility layer. It runs the original
`hamsterball.exe` with an x86 CPU emulator and replaces the Windows APIs the game uses (Direct3D 8,
DirectInput, DirectSound and friends) with macOS equivalents.

The game files are in `game/` and are embedded in the app. This is a private repository.

## Features

- Every race, Tournament, Time Trials, Mirror Tournament and Party Games (split-screen and 4-player Rodent Rumble)
- 60 FPS on Apple Silicon, with the 3D scenes rendered at up to 4× the original resolution for sharp Retina output
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

## Playing

| | |
|---|---|
| Steer | Mouse, or the arrow keys |
| Pause | Esc (or right mouse button) |
| Fullscreen | Cmd+F or Alt+Enter |
| Quit | Cmd+Q |
| Gamepad | Connect it before launching. In **Party Games**, select a player row and press Return to cycle that player's input to the controller. Left stick or d-pad steers. |

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

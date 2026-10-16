"""Patch SDL2's Android Java glue (idempotent). Usage: python3 patch_sdl.py <SDL2 source dir>

SDL treats any input device with a d-pad source as a joystick. Full keyboards often report one
(the emulator's keyboard and many Bluetooth keyboards do), so their arrow keys turned into gamepad
d-pad presses and the keyboard showed up in the game as a gamepad. Keep alphabetic keyboards that
aren't also a gamepad or joystick on the keyboard path."""
import os
import sys

path = os.path.join(sys.argv[1], "android-project/app/src/main/java/org/libsdl/app/SDLControllerManager.java")
src = open(path).read()
marker = "hbwrap: full keyboards are not joysticks"
if marker not in src:
    old = """        int sources = device.getSources();

        /* This is called for every button press, so let's not spam the logs */"""
    new = """        int sources = device.getSources();

        // %s
        if (device.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC &&
            (sources & InputDevice.SOURCE_CLASS_JOYSTICK) == 0 &&
            (sources & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD) {
            return false;
        }

        /* This is called for every button press, so let's not spam the logs */""" % marker
    assert src.count(old) == 1, "SDLControllerManager.isDeviceSDLJoystick not found"
    open(path, "w").write(src.replace(old, new))
    print("patched", path)

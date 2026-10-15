package dev.firefinch.hamsterball;

import org.libsdl.app.SDLActivity;

/** Hamster Ball: the SDL activity that hosts the native compatibility layer (libmain.so). */
public class HamsterBallActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }
}

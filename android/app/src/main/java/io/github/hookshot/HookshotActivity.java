package io.github.hookshot;

import org.libsdl.app.SDLActivity;

/** SDL's activity with the game's libraries: libmain.so is nx2/build.py's (RX_TARGET=android), its entry SDL_main. */
public class HookshotActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "main"};
    }
}

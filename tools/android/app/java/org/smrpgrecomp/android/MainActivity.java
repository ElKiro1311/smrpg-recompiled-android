// The SMRPG Recomp activity for Android: SDL3's (org.libsdl.app.SDLActivity), which loads libSDL3.so and libmain.so
// (the program: runtime/, `make PLATFORM=android`) and calls its SDL_main. The program does the rest
// (runtime/platform/android.c): the folders, the resources, the game code.
package org.smrpgrecomp.android;

import android.os.Bundle;
import android.view.KeyEvent;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // the screen does not turn off while playing (with the controller it is not touched)
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    // The Back button (the one on the AYN Thor and other handhelds, or the gesture / button of the Android bar) opens
    // and closes the settings menu, like Esc on PC (runtime/ui/menu.mm). Without this, SDL gave it to the game as the
    // controller's Select, which already has its own button, and the one on the bar closed the app.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                if (event.getRepeatCount() == 0) SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_ESCAPE);
            } else if (event.getAction() == KeyEvent.ACTION_UP) {
                SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_ESCAPE);
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }
}

// La actividad de SMRPG Recomp para Android: la de SDL3 (org.libsdl.app.SDLActivity), que carga libSDL3.so y
// libmain.so (el programa: runtime/, `make PLATFORM=android`) y llama a su SDL_main. Lo demás lo hace el programa
// (runtime/platform/android.c): las carpetas, los recursos, el código del juego.
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
        // la pantalla no se apaga mientras se juega (con el mando no se toca)
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    // El botón Atrás (el de la AYN Thor y otras consolas, o el gesto / botón de la barra de Android) abre y cierra el
    // menú de ajustes, como Esc en el PC (runtime/ui/menu.mm). Sin esto, SDL lo daba al juego como el Select del mando
    // —que ya tiene su botón—, y el de la barra cerraba la app.
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

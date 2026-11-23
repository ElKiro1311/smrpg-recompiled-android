#pragma once
// (SMRPG) El nombre de la aplicación en lo que ve quien juega: la ventana, los menús, los avisos y los errores.
#define APP_NAME "SMRPG Recomp"
// (SMRPG) La versión de SMRPG Recomp: x.y.z-alfa / -beta (Semantic Versioning: VERSIONES.txt). Se ve en el menú
// (Salir) y en el registro. La de la base (LARECOMP_VERSION, runtime/ui/launcher.mm) es la etiqueta de las cachés de
// shaders: no cambia con esta (cambiarla vaciaría las cachés de quien juega).
// (SMRPG Android) en Android, la del APK (VERSIONES-ANDROID.txt, que el Makefile pasa como ANDROID_APP_VERSION)
#if defined(__ANDROID__) && defined(ANDROID_APP_VERSION)
#define APP_VERSION ANDROID_APP_VERSION
#else
#define APP_VERSION "0.48.0-beta"
#endif

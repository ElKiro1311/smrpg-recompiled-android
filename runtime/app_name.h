#pragma once
// (SMRPG) The application's name wherever the player sees it: the window, the menus, the notices and the errors.
#define APP_NAME "SMRPG Recomp"
// (SMRPG) SMRPG Recomp's version: x.y.z-alfa / -beta (Semantic Versioning: CHANGELOG.txt). Shown in the menu (Quit) and
// in the log. The base's version (LARECOMP_VERSION, runtime/ui/launcher.mm) is the tag of the shader caches: it does
// not change along with this one (changing it would empty the player's caches).
// (SMRPG Android) on Android, the APK's (CHANGELOG-ANDROID.txt, which the Makefile passes as ANDROID_APP_VERSION)
#if defined(__ANDROID__) && defined(ANDROID_APP_VERSION)
#define APP_VERSION ANDROID_APP_VERSION
#else
#define APP_VERSION "0.48.0-beta"
#endif

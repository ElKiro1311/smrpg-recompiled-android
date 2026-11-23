# SMRPG Recomp for Android

**English** · [Español](README.es.md)

An unofficial port of **Super Mario RPG** (Nintendo Switch, 2023, version 1.0.0) to **Android** through static
recompilation: the game's program is translated to C, compiled for the phone or handheld's processor, and runs on top of
the [LASRecomp](https://github.com/peiikai/LASRecomp) runtime (system services, kernel and NVN → Vulkan).

The Windows port lives in its own repository:
[**smrpg-recompiled**](https://github.com/ElKiro1311/smrpg-recompiled).

> **Legal notice.** This is a non-profit fan project, **not affiliated with or endorsed by Nintendo or Square Enix**.
> *Super Mario RPG* and *Mario* are trademarks of Nintendo; the game is © Nintendo / SQUARE ENIX. Neither this
> repository nor its releases contain any of the game's files, code, graphics, music or text. To play you need the files
> of **your own copy** of the game, already extracted (the `exefs` and `romfs` folders of version 1.0.0); there are no
> instructions here on how to obtain them. Don't share your installation, the compiled game code (`lasgame.so`) or any
> textures the game dumps: they belong to the game.

## Status

Alpha. It plays at 60 FPS on a Snapdragon 8 Gen 2 handheld (AYN Thor, Adreno 740): movies, title screen, menus, exploring,
battles, saving and loading, in everything tested so far. It needs **Android 13 or newer on a 64-bit ARM device with
Vulkan 1.3**; other GPUs are untested.

What it has: internal resolutions from 720p up, Snapdragon GSR 1 and GSR 2 and AMD FSR 1 upscaling, Lossless Scaling
frame generation (with **your** copy of it), HD textures, button remapping, background pause and menus in Spanish (the
English version is on its way). What changed in each version: [`VERSIONES-ANDROID.txt`](VERSIONES-ANDROID.txt).

## Installing

1. Download `smrpg-recomp-android-<version>.apk` from the
   [latest release](https://github.com/ElKiro1311/smrpg-recompiled-android/releases) and install it.
2. The APK has nothing from the game in it, so you also need the game's code compiled for your device (`lasgame.so`).
   For now that step needs a Windows PC: see **Building** below (steps 1 and 2 are enough if you use the APK from the
   release).
3. Copy your extracted game and that code to the device, so that
   `Android/data/org.smrpgrecomp.android/files/extracted/` holds `exefs/`, `romfs/`, `lasgame.so` and `lasgame.so.id`.

The settings menu opens with the Back button (or the Home button on a controller). Frame generation uses the
`Lossless.dll` of your own copy of Lossless Scaling (a paid program on Steam), which you copy to
`Android/data/org.smrpgrecomp.android/files/`: neither the repository nor the APK includes or downloads it.

## Building

On Windows, with MSYS2 UCRT64. You need the Android SDK with `ndk;29.0.14206865`, `build-tools;36.0.0`,
`platforms;android-36` and `cmake;4.1.2`, plus JDK 17 or newer. Their paths go in a `local.env` file, which git ignores
(see [`local.env.ejemplo`](local.env.ejemplo)). With your extracted game in `extracted/`:

1. `tools/android/deps.sh arm64-v8a` builds SDL3, FFmpeg (LGPL), Opus and shaderc in `../deps/arm64-v8a`;
2. `make gen PLATFORM=android ABI=arm64-v8a` and `make -j8 PLATFORM=android ABI=arm64-v8a` build `libmain.so` and the
   game code, `lasgame.so`, in `build/android-arm64-v8a/`;
3. `tools/android/apk.sh arm64-v8a` builds the APK in `build/apk/`; install it with `adb install`.

## Bugs and suggestions

Open an [issue here](https://github.com/ElKiro1311/smrpg-recompiled-android/issues) and attach `lasrecomp.log` from the
game's data folder. Please don't report them to LASRecomp: this port isn't theirs.

## Credits and licenses

- [LASRecomp](https://github.com/peiikai/LASRecomp), by Peiikai and contributors (MIT): the recompiler, the runtime and
  the renderer. Its original README is in [`docs/README-LASRecomp.md`](docs/README-LASRecomp.md).
- [nxnative](https://github.com/seedds/nxnative), a reference for how this game behaves.
- [Snapdragon GSR](https://github.com/SnapdragonStudios/snapdragon-gsr) by Qualcomm, AMD FSR 1,
  [lsfg-vk-android](https://github.com/GameNative/lsfg-vk-android) (Lossless Scaling frame generation), Vulkan Memory
  Allocator, Dear ImGui, SMAA, SDL3, FFmpeg (LGPL), Opus, shaderc and others, each with its own license in
  `third_party/` and inside the APK (`assets/licencias`).
- The code of LASRecomp and of this port is under the MIT license in [`LICENSE`](LICENSE). That license does not cover
  Super Mario RPG or anything else of Nintendo's, nor what is in `third_party/`, which keeps its own license.

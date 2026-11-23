// (SMRPG Android) The Android platform: the Linux one (runtime/platform/linux.c: bionic, since API 33, has what it
// uses), plus what an app requires (android_prepare, which main calls first):
//   - the data folder (LARECOMP_DATA_ROOT) is the app's external one, /sdcard/Android/data/<package>/files: the game
//     (extracted/: exefs, romfs and the game code, lasgame.so), the saves and the settings go there; the game can be
//     copied to it over adb or with the file manager, without permissions;
//   - what ships with the program (the fonts, the icon) comes out of the APK's assets into the internal folder
//     (SMRPG_RES_DIR, platform/linux.c plat_exe_path);
//   - the game code cannot be loaded from external storage (it is mounted without execute permission): it is copied to
//     the internal folder (with its .id and its .cpu) and loaded from there (LARECOMP_GAME_LIB, runtime/gamelib.c);
//   - <data folder>/env.txt: VARIABLE=value lines that are put in the environment (tests: LARECOMP_INPUT,
//     LARECOMP_SHOT_AT...), as on PC from the console;
//   - the log is lasrecomp.log, in the data folder (platform/linux.c); what is here goes out through logcat (SMRPG).
#include "linux.c"

#include <android/log.h>
#include <sys/stat.h>

static void a_mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
    mkdir(p, 0755);
}

// src -> dst if it is not already the same (same size and not older); through a .tmp that is renamed
static bool a_copy_if_changed(const char *src, const char *dst) {
    struct stat s, d;
    if (stat(src, &s)) return false;
    if (!stat(dst, &d) && d.st_size == s.st_size && d.st_mtime >= s.st_mtime) return true;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    FILE *in = fopen(src, "rb"), *out = in ? fopen(tmp, "wb") : NULL;
    bool ok = in && out;
    static char buf[1 << 20];
    for (size_t n; ok && (n = fread(buf, 1, sizeof buf, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
    if (in) fclose(in);
    if (out && fclose(out)) ok = false;
    if (ok) ok = rename(tmp, dst) == 0;
    else remove(tmp);
    if (ok) __android_log_print(ANDROID_LOG_INFO, "SMRPG", "copied %s -> %s (%lld bytes)", src, dst, (long long)s.st_size);
    return ok;
}

// An APK asset (SDL_IOFromFile reads the assets with relative paths) to dst, if it is not already there with that size
static void a_asset(const char *name, const char *dst) {
    SDL_IOStream *in = SDL_IOFromFile(name, "rb");
    if (!in) { __android_log_print(ANDROID_LOG_WARN, "SMRPG", "missing asset %s", name); return; }
    Sint64 size = SDL_GetIOSize(in);
    struct stat d;
    if (!stat(dst, &d) && d.st_size == size) { SDL_CloseIO(in); return; }
    FILE *out = fopen(dst, "wb");
    static char buf[1 << 16];
    for (size_t n; out && (n = SDL_ReadIO(in, buf, sizeof buf)) > 0;) fwrite(buf, 1, n, out);
    if (out) fclose(out);
    SDL_CloseIO(in);
}

void android_prepare(void) {
    const char *in = SDL_GetAndroidInternalStoragePath(), *ex = SDL_GetAndroidExternalStoragePath();
    if (!in || !ex) { __android_log_print(ANDROID_LOG_ERROR, "SMRPG", "no app folders: %s", SDL_GetError()); return; }
    char path[1024], res[1024];
    // landscape only: the program's window is resizable, and for SDL that is "any orientation" (the game ended up in
    // portrait, with aspect 0.56)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    a_mkdirs(ex);
    setenv("LARECOMP_DATA_ROOT", ex, 1);
    // env.txt (tests; entorno.txt, its name up to 0.9.1)
    snprintf(path, sizeof path, "%s/env.txt", ex);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(path, sizeof path, "%s/entorno.txt", ex);
        f = fopen(path, "r");
    }
    for (char line[4096]; f && fgets(line, sizeof line, f);) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        __android_log_print(ANDROID_LOG_INFO, "SMRPG", "environment: %s=%s", line, eq + 1);
    }
    if (f) fclose(f);
    // what ships with the program
    snprintf(res, sizeof res, "%s/res", in);
    a_mkdirs(res);
    static const char *const assets[] = {"Baloo2-SemiBold.ttf", "BIZUDPGothic-Regular.ttf", "DejaVuSans.ttf",
                                         "DejaVuSans-Bold.ttf", "icon.png"};
    for (size_t i = 0; i < sizeof assets / sizeof *assets; i++) {
        snprintf(path, sizeof path, "%s/%s", res, assets[i]);
        a_asset(assets[i], path);
    }
    setenv("SMRPG_RES_DIR", res, 1);
    // the game code, to the internal folder
    char lib[1024], src[1024];
    snprintf(lib, sizeof lib, "%s/game", in);
    snprintf(src, sizeof src, "%s/juego", in);  // (its name up to 0.9.1: the copy that is there is kept)
    rename(src, lib);
    a_mkdirs(lib);
    static const char *const parts[] = {"lasgame.so", "lasgame.so.id", "lasgame.so.cpu"};
    bool have = true;
    for (int i = 0; i < 3; i++) {
        snprintf(src, sizeof src, "%s/extracted/%s", ex, parts[i]);
        snprintf(path, sizeof path, "%s/%s", lib, parts[i]);
        if (!a_copy_if_changed(src, path)) {
            if (i < 2) have = false;
            else remove(path);  // (no .cpu outside (arm64's has none): none inside either, an old x86_64 one would reject it)
        }
    }
    snprintf(path, sizeof path, "%s/lasgame.so", lib);
    if (have) setenv("LARECOMP_GAME_LIB", path, 1);
    // the test screenshots (LARECOMP_SHOT_AT) go to build/frames in the data folder
    snprintf(path, sizeof path, "%s/build/frames", ex);
    a_mkdirs(path);
    // (the program's log is opened by plat_init in the data folder, lasrecomp.log: stderr is not a terminal)
    __android_log_print(ANDROID_LOG_INFO, "SMRPG", "data: %s; resources: %s; game code: %s", ex, res,
                        have ? lib : "(not there: extracted/lasgame.so)");
}

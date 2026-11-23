// (SMRPG Android) La plataforma Android: la de Linux (runtime/platform/linux.c: bionic, desde la API 33, tiene lo que
// usa), más lo que pide una app (android_prepare, que main llama lo primero):
//   - la carpeta de datos (LARECOMP_DATA_ROOT) es la externa de la app, /sdcard/Android/data/<paquete>/files: ahí van
//     el juego (extracted/: exefs, romfs y el código del juego, lasgame.so), las partidas y los ajustes; se le puede
//     copiar el juego por adb o con el explorador de archivos, sin permisos;
//   - lo que va con el programa (las fuentes, el icono) sale de los assets del APK a la carpeta interna (SMRPG_RES_DIR,
//     platform/linux.c plat_exe_path);
//   - el código del juego no se puede cargar desde el almacenamiento externo (se monta sin permiso de ejecución): se
//     copia a la carpeta interna (con su .id y su .cpu) y se carga de ahí (LARECOMP_GAME_LIB, runtime/gamelib.c);
//   - <carpeta de datos>/entorno.txt: líneas VARIABLE=valor que se ponen en el entorno (pruebas: LARECOMP_INPUT,
//     LARECOMP_SHOT_AT...), como en el PC desde la consola;
//   - el registro es lasrecomp.log, en la carpeta de datos (platform/linux.c); esto de aquí sale por logcat (SMRPG).
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

// src -> dst si no está ya igual (mismo tamaño y no más vieja); por un .tmp que se renombra
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
    if (ok) __android_log_print(ANDROID_LOG_INFO, "SMRPG", "copiado %s -> %s (%lld bytes)", src, dst, (long long)s.st_size);
    return ok;
}

// Un asset del APK (SDL_IOFromFile lee los assets con rutas relativas) a dst, si no está ya con ese tamaño
static void a_asset(const char *name, const char *dst) {
    SDL_IOStream *in = SDL_IOFromFile(name, "rb");
    if (!in) { __android_log_print(ANDROID_LOG_WARN, "SMRPG", "falta el asset %s", name); return; }
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
    if (!in || !ex) { __android_log_print(ANDROID_LOG_ERROR, "SMRPG", "sin carpetas de la app: %s", SDL_GetError()); return; }
    char path[1024], res[1024];
    // solo en horizontal: la ventana del programa es redimensionable, y para SDL eso es «cualquier orientación» (el
    // juego quedaba en vertical, con aspecto 0,56)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    a_mkdirs(ex);
    setenv("LARECOMP_DATA_ROOT", ex, 1);
    // entorno.txt (pruebas)
    snprintf(path, sizeof path, "%s/entorno.txt", ex);
    FILE *f = fopen(path, "r");
    for (char line[4096]; f && fgets(line, sizeof line, f);) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        __android_log_print(ANDROID_LOG_INFO, "SMRPG", "entorno: %s=%s", line, eq + 1);
    }
    if (f) fclose(f);
    // lo que va con el programa
    snprintf(res, sizeof res, "%s/res", in);
    a_mkdirs(res);
    static const char *const assets[] = {"Baloo2-SemiBold.ttf", "BIZUDPGothic-Regular.ttf", "DejaVuSans.ttf",
                                         "DejaVuSans-Bold.ttf", "icon.png"};
    for (size_t i = 0; i < sizeof assets / sizeof *assets; i++) {
        snprintf(path, sizeof path, "%s/%s", res, assets[i]);
        a_asset(assets[i], path);
    }
    setenv("SMRPG_RES_DIR", res, 1);
    // el código del juego, a la carpeta interna
    char lib[1024], src[1024];
    snprintf(lib, sizeof lib, "%s/juego", in);
    a_mkdirs(lib);
    static const char *const parts[] = {"lasgame.so", "lasgame.so.id", "lasgame.so.cpu"};
    bool have = true;
    for (int i = 0; i < 3; i++) {
        snprintf(src, sizeof src, "%s/extracted/%s", ex, parts[i]);
        snprintf(path, sizeof path, "%s/%s", lib, parts[i]);
        if (!a_copy_if_changed(src, path)) {
            if (i < 2) have = false;
            else remove(path);  // (sin .cpu fuera —el de arm64 no lo lleva—, tampoco dentro: uno viejo de x86_64 lo rechazaría)
        }
    }
    snprintf(path, sizeof path, "%s/lasgame.so", lib);
    if (have) setenv("LARECOMP_GAME_LIB", path, 1);
    // las capturas de las pruebas (LARECOMP_SHOT_AT) van a build/frames de la carpeta de datos
    snprintf(path, sizeof path, "%s/build/frames", ex);
    a_mkdirs(path);
    // (el registro del programa lo abre plat_init en la carpeta de datos, lasrecomp.log: stderr no es una terminal)
    __android_log_print(ANDROID_LOG_INFO, "SMRPG", "datos: %s; recursos: %s; código del juego: %s", ex, res,
                        have ? path : "(no está: extracted/lasgame.so)");
}

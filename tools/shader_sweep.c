// tools/shader_sweep.c: el traductor de shaders contra todos los del juego, sin jugar. Lee los paquetes que saca
// nxnative (dm/gfx/tools/shader_split.py: registros "BTCDBTCD" + clave de 8 bytes + cabecera hasta 0x38 con las
// longitudes del control y del código en 0x28, luego el código y el control), traduce cada shader a GLSL
// (runtime/nvn/maxwell/translate.c) y lo compila a SPIR-V con shaderc como el renderizador. Cuenta los fallos por
// motivo y escribe uno de muestra de cada (y su código en <salida>/<clave>.bin, para desensamblarlo aparte).
// Compilar (MSYS2 UCRT64, en la carpeta del repositorio):
//   clang -O2 -Iruntime -o build/shader_sweep.exe tools/shader_sweep.c runtime/nvn/maxwell/translate.c -lshaderc_shared
// Uso: build/shader_sweep.exe <carpeta de paquetes> <carpeta de salida> [máximo por paquete]
#include <dirent.h>
#include <shaderc/shaderc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double t_trad, t_comp;

#include "nvn/maxwell/shader.h"

typedef struct { char why[160]; int n; char key[17]; } Fallo;
static Fallo fallos[4096];
static int nfallos;

static const char *g_pkg = "";
static void anota(const char *why, const char *key, const uint8_t *code, uint32_t len, const char *out) {
    char w[160];
    snprintf(w, sizeof w, "%s", why);
    for (char *p = w; *p; p++) if (*p == '\n' || *p == '\r') { *p = 0; break; }
    fprintf(stderr, "[fallo] %s %s: %s\n", g_pkg, key, w);
    // (agrupar: una instrucción desconocida por su código de operación, los 12 bits altos; el resto sin las cifras de
    // registro y desplazamiento, que cambian de un shader a otro)
    char g[160];
    if (!strncmp(w, "unknown instruction ", 20) && strlen(w) >= 23) snprintf(g, sizeof g, "unknown instruction %.3s...", w + 20);
    else {
        size_t k = 0;
        for (const char *p = w; *p && k + 1 < sizeof g; p++)
            if (!((*p >= '0' && *p <= '9') || (*p == 'x' && p > w && p[-1] == '0'))) g[k++] = *p;
        g[k] = 0;
    }
    for (int i = 0; i < nfallos; i++)
        if (!strcmp(fallos[i].why, g)) { fallos[i].n++; return; }
    if (nfallos == 4096) return;
    Fallo *f = &fallos[nfallos++];
    snprintf(f->why, sizeof f->why, "%s", g);
    snprintf(f->key, sizeof f->key, "%s", key);
    f->n = 1;
    char path[600];
    snprintf(path, sizeof path, "%s/%s.bin", out, key);
    FILE *b = fopen(path, "wb");
    if (b) { fwrite(code, 1, len, b); fclose(b); }
    fprintf(stderr, "[nuevo] %s (%s): %s\n", key, w, g);
}

static shaderc_shader_kind kind_of(int st) {
    return st == STAGE_VERTEX ? shaderc_vertex_shader : st == STAGE_FRAGMENT ? shaderc_fragment_shader
         : st == STAGE_GEOMETRY ? shaderc_geometry_shader : shaderc_compute_shader;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: shader_sweep <paquetes> <salida> [máximo por paquete]\n"); return 2; }
    int maxper = argc > 3 ? atoi(argv[3]) : 0;
    mkdir(argv[2]);
    DIR *d = opendir(argv[1]);
    if (!d) { fprintf(stderr, "no puedo abrir %s\n", argv[1]); return 1; }
    shaderc_compiler_t comp = shaderc_compiler_initialize();
    long total = 0, bien = 0, mal_trad = 0, mal_glsl = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        size_t ln = strlen(n);
        if (ln < 5 || strcmp(n + ln - 4, ".bin")) continue;
        int st = !strncmp(n, "vs_", 3) ? STAGE_VERTEX : (!strncmp(n, "ps_", 3) || !strncmp(n, "fs_", 3)) ? STAGE_FRAGMENT
               : !strncmp(n, "cs_", 3) ? STAGE_COMPUTE : !strncmp(n, "gs_", 3) ? STAGE_GEOMETRY : -1;
        if (st < 0) { fprintf(stderr, "(salto %s: etapa desconocida)\n", n); continue; }
        char path[600];
        snprintf(path, sizeof path, "%s/%s", argv[1], n);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc((size_t)sz + 16);
        if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); continue; }
        fclose(f);
        int hechos = 0;
        for (long o = 0; o + 0x38 <= sz;) {
            if (memcmp(buf + o, "BTCDBTCD", 8)) { o++; continue; }
            char key[17];
            for (int i = 0; i < 8; i++) snprintf(key + 2 * i, 3, "%02x", buf[o + 8 + i]);
            uint32_t ctl = *(uint32_t *)(buf + o + 0x28), cl = *(uint32_t *)(buf + o + 0x2c);
            const uint8_t *code = buf + o + 0x38;
            o += 0x38 + cl + ctl;
            g_pkg = n;
            if (maxper && hechos >= maxper) continue;
            hechos++, total++;
            ShaderKey k = {0};
            k.glsl = 1;
            k.vs_out_mask = 0xffffffffu;
            // (como el renderizador: el código acaba donde empiezan las constantes del compilador, control + 0x700;
            // nvn/objects.c const_offset; sin él, el traductor leía las constantes como instrucciones)
            uint32_t coff = ctl >= 0x704 ? *(const uint32_t *)(code + cl + 0x700) : 0;
            size_t maxs = coff && coff <= cl ? coff : cl;
            double t0 = now_ms();
            ShaderInfo *si = maxwell_translate(code, maxs, st, &k);
            t_trad += now_ms() - t0;
            if (!si || !si->msl) {
                mal_trad++;
                anota(si && si->error[0] ? si->error : "traducción sin mensaje", key, code, cl, argv[2]);
                if (si) maxwell_free(si);
                continue;
            }
            shaderc_compile_options_t opt = shaderc_compile_options_initialize();
            shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
            double t1 = now_ms();
            shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, si->msl, strlen(si->msl), kind_of(st), key, "main", opt);
            t_comp += now_ms() - t1;
            if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
                mal_glsl++;
                char why[200];
                const char *m = shaderc_result_get_error_message(r);
                const char *c2 = strstr(m, "error:");
                snprintf(why, sizeof why, "GLSL: %s", c2 ? c2 + 6 : m);
                anota(why, key, code, cl, argv[2]);
                char gp[600];
                snprintf(gp, sizeof gp, "%s/%s.glsl", argv[2], key);
                FILE *g = fopen(gp, "wb");
                if (g) { fputs(si->msl, g); fputs("\n/* ", g); fputs(m, g); fputs(" */\n", g); fclose(g); }
            } else bien++;
            shaderc_result_release(r);
            shaderc_compile_options_release(opt);
            maxwell_free(si);
            if (total % 2000 == 0) fprintf(stderr, "... %ld shaders (%ld bien)\n", total, bien);
        }
        free(buf);
    }
    closedir(d);
    printf("tiempo: traducir %.0f ms (%.2f ms por shader), compilar GLSL %.0f ms (%.2f ms por shader)\n", t_trad, t_trad / (total ? total : 1), t_comp, t_comp / (total ? total : 1));
    printf("shaders %ld: bien %ld, fallan al traducir %ld, fallan al compilar el GLSL %ld\n", total, bien, mal_trad, mal_glsl);
    // de más a menos frecuente
    for (int i = 0; i < nfallos; i++)
        for (int j = i + 1; j < nfallos; j++)
            if (fallos[j].n > fallos[i].n) { Fallo t = fallos[i]; fallos[i] = fallos[j]; fallos[j] = t; }
    for (int i = 0; i < nfallos; i++) printf("%6d  %s  (p. ej. %s)\n", fallos[i].n, fallos[i].why, fallos[i].key);
    return 0;
}

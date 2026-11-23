// (SMRPG) Paquetes de texturas: volcar las texturas del juego a PNG y sustituirlas por otras, normalmente más grandes
// (texturas HD), desde una carpeta. Ajustes: Menú > Vídeo > Imagen > «Texturas HD» y «Volcar texturas».
//
// - Huella de una textura: texpack_key, 64 bits, sobre los datos del nivel 0 tal como el juego los da (bloques ASTC,
//   o los texels RGBA8) con su ancho, alto y formato de NVN. No depende de dónde esté la textura en memoria ni de
//   cómo llegue (desde la CPU o con una copia de la GPU). Escrita en hexadecimal (16 cifras) da el nombre del archivo.
// - Volcado: <datos>/texturas-volcadas/<huella>_<ancho>x<alto>_<formato>.png, una vez por textura (RGBA, el color tal
//   como lo guarda el juego: sRGB en las de color).
// - Sustitución: cualquier PNG de <datos>/texturas o sus subcarpetas cuyo nombre empiece por la huella (lo que siga
//   da igual: «<huella>.png», «<huella>_1024x1024_astc6x6srgb_x4.png», ...). Debe tener las proporciones de la
//   original y al menos su tamaño; los mipmaps se generan. El juego sigue viendo el tamaño original (consultas de
//   tamaño y lecturas por texel van por la escala de la textura, como las de las texturas a otra resolución).
// La sustitución y el volcado los hace vulkan/textures.c (upload_texture) y exec.c (copy_buffer_to_texture).
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "../../paths.h"
#include "../../rt.h"

int settings_tex_pack(void), settings_tex_dump(void);  // runtime/settings.c
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);  // device.c

static uint64_t fmix64(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

uint64_t texpack_key(const uint8_t *data, uint32_t row_bytes, uint32_t nrows, uint64_t pitch, uint32_t w, uint32_t h,
                     uint32_t fmt) {
    uint64_t k = 0x534d525054583031ull ^ ((uint64_t)w << 40) ^ ((uint64_t)h << 16) ^ fmt;  // («SMRPTX01»)
    for (uint32_t y = 0; y < nrows; y++) {
        const uint8_t *r = data + (uint64_t)y * pitch;
        uint32_t i = 0;
        for (; i + 8 <= row_bytes; i += 8) {
            uint64_t v;
            memcpy(&v, r + i, 8);
            k = (k ^ v) * 0x9e3779b97f4a7c15ull;
            k ^= k >> 29;
        }
        if (i < row_bytes) {
            uint64_t v = 0;
            memcpy(&v, r + i, row_bytes - i);
            k = (k ^ v) * 0x9e3779b97f4a7c15ull;
            k ^= k >> 29;
        }
    }
    return fmix64(k);
}

// ---------------------------------------------------------------- el índice de <datos>/texturas
typedef struct { uint64_t key; char *path; } Entry;
static Entry *g_tab;
static uint32_t g_cap, g_n;
static bool g_scanned;
static SDL_Mutex *g_lock;

static void tab_put(uint64_t key, const char *path) {
    if ((g_n + 1) * 2 > g_cap) {
        uint32_t nc = g_cap ? g_cap * 2 : 1024;
        Entry *nt = calloc(nc, sizeof *nt);
        for (uint32_t i = 0; i < g_cap; i++)
            if (g_tab[i].path)
                for (uint32_t j = (uint32_t)fmix64(g_tab[i].key) & (nc - 1);; j = (j + 1) & (nc - 1))
                    if (!nt[j].path) { nt[j] = g_tab[i]; break; }
        free(g_tab);
        g_tab = nt;
        g_cap = nc;
    }
    for (uint32_t j = (uint32_t)fmix64(key) & (g_cap - 1);; j = (j + 1) & (g_cap - 1)) {
        if (!g_tab[j].path) { g_tab[j].key = key; g_tab[j].path = SDL_strdup(path); g_n++; return; }
        if (g_tab[j].key == key) return;  // (dos archivos con la misma huella: vale el primero)
    }
}

static bool hex16(const char *s, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        int c = tolower((unsigned char)s[i]), d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | (uint64_t)d;
    }
    if (isxdigit((unsigned char)s[16])) return false;  // (más de 16 cifras: no es una huella)
    *out = v;
    return true;
}

static SDL_EnumerationResult scan_cb(void *ud, const char *dir, const char *name) {
    int *depth = ud;
    char full[1024];
    snprintf(full, sizeof full, "%s%s", dir, name);
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(full, &pi)) return SDL_ENUM_CONTINUE;
    if (pi.type == SDL_PATHTYPE_DIRECTORY) {
        if (*depth < 8) {
            (*depth)++;
            char sub[1040];
            snprintf(sub, sizeof sub, "%s/", full);
            SDL_EnumerateDirectory(sub, scan_cb, ud);
            (*depth)--;
        }
        return SDL_ENUM_CONTINUE;
    }
    size_t n = strlen(name);
    uint64_t key;
    if (n >= 20 && !SDL_strcasecmp(name + n - 4, ".png") && hex16(name, &key)) tab_put(key, full);
    return SDL_ENUM_CONTINUE;
}

static void scan(void) {
    if (g_scanned) return;
    g_scanned = true;
    const char *root = rt_data_root();
    if (!root || !*root) return;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/texturas/", root);
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(dir, &pi) || pi.type != SDL_PATHTYPE_DIRECTORY) {
        LOG("[texturas] sin carpeta de texturas HD (%s)\n", dir);
        return;
    }
    int depth = 0;
    uint64_t t0 = SDL_GetTicks();
    SDL_EnumerateDirectory(dir, scan_cb, &depth);
    LOG("[texturas] %u texturas HD en %s (%llu ms)\n", g_n, dir, (unsigned long long)(SDL_GetTicks() - t0));
}

// La ruta del PNG que sustituye a la textura de esa huella, o NULL.
const char *texpack_lookup(uint64_t key) {
    if (!settings_tex_pack()) return NULL;
    if (!g_lock) g_lock = SDL_CreateMutex();
    SDL_LockMutex(g_lock);
    scan();
    const char *r = NULL;
    if (g_n)
        for (uint32_t j = (uint32_t)fmix64(key) & (g_cap - 1); g_tab[j].path; j = (j + 1) & (g_cap - 1))
            if (g_tab[j].key == key) { r = g_tab[j].path; break; }
    SDL_UnlockMutex(g_lock);
    return r;
}

// ¿Hay que calcular la huella de las texturas que se cargan? Con el volcado, o con texturas HD en la carpeta.
bool texpack_active(void) {
    if (settings_tex_dump()) return true;
    if (!settings_tex_pack()) return false;
    if (!g_lock) g_lock = SDL_CreateMutex();
    SDL_LockMutex(g_lock);
    scan();
    bool any = g_n > 0;
    SDL_UnlockMutex(g_lock);
    return any;
}

// El PNG en RGBA8 (filas seguidas), o NULL.
uint8_t *texpack_load(const char *path, uint32_t *w, uint32_t *h) {
    SDL_Surface *s = SDL_LoadPNG(path);
    if (!s) { LOG("[texturas] no se puede leer %s: %s\n", path, SDL_GetError()); return NULL; }
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);  // (en memoria: R, G, B, A)
    SDL_DestroySurface(s);
    if (!c) return NULL;
    uint8_t *px = malloc((size_t)c->w * c->h * 4);
    if (px)
        for (int y = 0; y < c->h; y++) memcpy(px + (size_t)y * c->w * 4, (uint8_t *)c->pixels + (size_t)y * c->pitch, (size_t)c->w * 4);
    *w = (uint32_t)c->w;
    *h = (uint32_t)c->h;
    SDL_DestroySurface(c);
    return px;
}

// ---------------------------------------------------------------- volcado
static uint64_t *g_dumped;
static uint32_t g_dcap, g_dn;

static const char *fmt_name(uint32_t f) {
    static char b[16];
    switch (f) {
    case 0x25: return "rgba8";
    case 0x38: return "rgba8srgb";
    default:
        if (f >= 0x79 && f <= 0x94) {
            static const char *dims[] = {"4x4", "5x4", "5x5", "6x5", "6x6", "8x5", "8x6", "8x8", "10x5", "10x6", "10x8", "10x10", "12x10", "12x12"};
            snprintf(b, sizeof b, "astc%s%s", dims[(f - 0x79) % 14], f >= 0x87 ? "srgb" : "");
            return b;
        }
        snprintf(b, sizeof b, "fmt%x", f);
        return b;
    }
}

// Guarda el nivel 0 de una textura (RGBA8, filas seguidas) si el volcado está activado y no se guardó ya.
void texpack_dump_rgba(uint64_t key, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t fmt) {
    if (!settings_tex_dump() || !rgba || w < 4 || h < 4) return;
    if (!g_lock) g_lock = SDL_CreateMutex();
    SDL_LockMutex(g_lock);
    bool seen = false;
    if ((g_dn + 1) * 2 > g_dcap) {
        uint32_t nc = g_dcap ? g_dcap * 2 : 4096;
        uint64_t *nt = calloc(nc, sizeof *nt);
        for (uint32_t i = 0; i < g_dcap; i++)
            if (g_dumped[i])
                for (uint32_t j = (uint32_t)fmix64(g_dumped[i]) & (nc - 1);; j = (j + 1) & (nc - 1))
                    if (!nt[j]) { nt[j] = g_dumped[i]; break; }
        free(g_dumped);
        g_dumped = nt;
        g_dcap = nc;
    }
    uint64_t k = key ? key : 1;
    for (uint32_t j = (uint32_t)fmix64(k) & (g_dcap - 1);; j = (j + 1) & (g_dcap - 1)) {
        if (!g_dumped[j]) { g_dumped[j] = k; g_dn++; break; }
        if (g_dumped[j] == k) { seen = true; break; }
    }
    SDL_UnlockMutex(g_lock);
    if (seen) return;
    const char *root = rt_data_root();
    char dir[1024], path[1200];
    snprintf(dir, sizeof dir, "%s/texturas-volcadas", root ? root : ".");
    SDL_CreateDirectory(dir);
    snprintf(path, sizeof path, "%s/%016llx_%ux%u_%s.png", dir, (unsigned long long)key, w, h, fmt_name(fmt));
    SDL_PathInfo pi;
    if (SDL_GetPathInfo(path, &pi)) return;  // (de otra sesión)
    vk_save_png(path, rgba, w, h);
}

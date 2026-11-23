// (SMRPG) Los vídeos (las películas de CRI Sofdec) en una pantalla más ancha (o más alta) que 16:9.
//
// El juego los pone con MoviePlayer: su RawImage m_image (+0x18), con el material de CRI que pinta cada imagen del vídeo,
// y encima su Image m_fade_image (+0x50), el fundido a negro; las dos, recuadros 16:9 de 1280x720 que llenan su lienzo
// (m_canvas, +0x48, de referencia 1280x720) en la consola. Con la pantalla ancha (hle/unity_screen.c) el lienzo ocupa
// toda la pantalla, pero esas dos se quedan en el 16:9 del centro: el vídeo con bandas a los lados (y lo de detrás en
// ellas) y los fundidos sin llegar a los lados.
// «Vídeos en pantalla ancha» (Menú > Vídeo > Pantalla):
//   - Bandas negras: el vídeo entero en el centro (los lados, negros con «Negro puro»: vulkan/exec.c lut_black_fix);
//   - Llenar la pantalla: la RawImage crece con el aspecto del vídeo hasta cubrir el lienzo (en 21:9 se recorta arriba y
//     abajo; en 16:10, a los lados). Nada se estira. Solo las películas: los vídeos en bucle (el fondo del título,
//     movie_017.usm, con el logotipo pegado arriba y el castillo abajo, que se cortaban) se quedan en su 16:9. El bucle
//     es el _loop (+0x80) de su CriManaMovieControllerForUI (m_player, +0x20); su archivo, _moviePath (+0x78), va al
//     registro con cada vídeo nuevo (movie_001.usm: la apertura, sin bucle).
// El fundido cubre todo el lienzo en los dos modos, para que oscurezca también los lados.
// MoviePlayer.Update (main 0x7001c14dc0) solo se llama por puntero (Unity, cada imagen mientras hay un MoviePlayer
// activo): hle_override lo envuelve y, tras el original, se ajustan las medidas con RectTransform.
// SetSizeWithCurrentAnchors (crece por los dos lados, desde el centro). El tamaño del lienzo en sus unidades sale de
// Canvas.pixelRect / Canvas.scaleFactor. La medida de partida es la que tiene la primera vez que se ve como un recuadro
// 16:9 a toda la altura (o anchura) del lienzo; si el juego la cambia por su cuenta, la nueva pasa a ser la de partida.
// Así cambiar el ajuste, el aspecto de la ventana o la pantalla ancha se aplica en marcha.
// LARECOMP_LOG_MOVIE_FIT=1: las medidas que ve (las primeras veces).
//
// (SMRPG 0.49) El fundido de las transiciones, GS.Fade (el negro al entrar en una zona desde el mapa, al entrar en una
// casa...), tiene el mismo problema: su Image m_image (+0x30) es un recuadro 16:9 a toda la altura del lienzo, y en una
// pantalla ancha los lados quedaban sin cubrir: se veía el color de fondo de la cámara mientras carga la zona (caqui) o
// la zona nueva antes de tiempo (visto en un vídeo de una prueba del 2026-11-17; ruta de prueba: la partida en wld00, el mapa, y A).
// GS.Fade.Update (main 0x7001959d70, solo por puntero) se envuelve igual: tras el original, la Image cubre todo su
// lienzo (Graphic.get_canvas). En 16:9 no cambia nada.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"

#define MP_UPDATE GADDR(0x7001c14dc0ull)               // MoviePlayer.Update
#define GRAPHIC_RECT_TRANSFORM GADDR(0x7001c459f0ull)  // Graphic.get_rectTransform
#define RT_GET_RECT GADDR(0x70017a5670ull)             // RectTransform.get_rect -> (x, y, ancho, alto) en s0-s3
#define RT_SET_SIZE GADDR(0x70017a6f20ull)             // RectTransform.SetSizeWithCurrentAnchors(Axis eje, float tamaño)
#define CANVAS_PIXEL_RECT GADDR(0x70022fafd0ull)       // Canvas.get_pixelRect -> (x, y, ancho, alto) en píxeles, en s0-s3
#define CANVAS_SCALE GADDR(0x70022fb080ull)            // Canvas.get_scaleFactor -> s0
#define FADE_UPDATE GADDR(0x7001959d70ull)             // GS.Fade.Update
#define GRAPHIC_CANVAS GADDR(0x7001c451f0ull)          // Graphic.get_canvas

static GuestFn mp_update, f_rect_transform, f_get_rect, f_set_size, f_pixel_rect, f_scale, fade_update_orig, f_canvas;
static int dbg = -1;

static float sreg(Cpu *c, int n) {
    float f;
    memcpy(&f, &c->v[n], 4);
    return f;
}
static void set_sreg(Cpu *c, int n, float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    c->v[n] = b;
}
static void gcall(Cpu *c, GuestFn f) {
    c->x[30] = MP_UPDATE;  // (una dirección del juego como retorno, para las trazas)
    f(c);
}
static bool alive(uint64_t o) { return o && LD64(o + 0x10); }  // (un UnityEngine.Object con su objeto nativo: m_CachedPtr)

// El tamaño del lienzo en sus unidades (las de los RectTransform de dentro); false si no se puede saber
static bool canvas_size(Cpu *c, uint64_t canvas, float *cw, float *ch) {
    if (!alive(canvas)) return false;
    c->x[0] = canvas, c->x[1] = 0;
    gcall(c, f_pixel_rect);
    float pw = sreg(c, 2), ph = sreg(c, 3);
    c->x[0] = canvas, c->x[1] = 0;
    gcall(c, f_scale);
    float s = sreg(c, 0);
    if (!(pw > 0 && ph > 0 && s > 0)) return false;
    *cw = pw / s, *ch = ph / s;
    return true;
}

static void set_size(Cpu *c, uint64_t rt, int axis, float v) {
    c->x[0] = rt, c->x[1] = (uint32_t)axis, c->x[2] = 0;
    set_sreg(c, 0, v);
    gcall(c, f_set_size);
}

enum { FIT_MOVIE, FIT_COVER, FIT_FADE };  // (FIT_FADE: como FIT_COVER, el fundido de GS.Fade, con su línea en el registro)
typedef struct { uint64_t obj; float w0, h0, w1, h1; } Fit;  // (la medida de partida y la última puesta)
static Fit fits[16];
static int nfit, next_slot;
RT_ON_RESET(movie_fit_reset) { nfit = 0, next_slot = 0; }  // (objetos de antes de reiniciar el juego dentro del proceso)

static void fit(Cpu *c, uint64_t g, int kind, float cw, float ch, bool loop) {
    if (!alive(g)) return;
    c->x[0] = g, c->x[1] = 0;
    gcall(c, f_rect_transform);
    uint64_t rt = c->x[0];
    if (!alive(rt)) return;
    c->x[0] = rt, c->x[1] = 0;
    gcall(c, f_get_rect);
    float w = sreg(c, 2), h = sreg(c, 3);
    const double a = 16.0 / 9.0, ca = (double)cw / ch;
    bool wide = ca > a * 1.01, tall = ca < a * 0.99;
    // (un recuadro 16:9 que llena el lienzo a lo alto en una pantalla ancha, o a lo ancho en una más alta)
    bool box = h > 0 && fabs(w / h - a) < 0.01 * a && (tall ? fabsf(w - cw) < 0.02f * cw : fabsf(h - ch) < 0.02f * ch);
    if (dbg > 0) dbg--, LOG("[vídeo] %s %#llx: %.1fx%.1f en un lienzo de %.1fx%.1f\n", kind == FIT_MOVIE ? "imagen" : "fundido",
                            (unsigned long long)g, w, h, cw, ch);
    int k = -1;
    for (int i = 0; i < nfit; i++) if (fits[i].obj == g) k = i;
    if (k < 0) {
        if (!box || (!wide && !tall)) return;
        if (nfit < (int)(sizeof fits / sizeof fits[0])) k = nfit++;
        else k = next_slot, next_slot = (next_slot + 1) % (int)(sizeof fits / sizeof fits[0]);  // (la más vieja)
        fits[k] = (Fit){g, w, h, w, h};
    } else if (fabsf(w - fits[k].w1) > 1 || fabsf(h - fits[k].h1) > 1) {  // (la cambió el juego: esa es la de partida)
        if (!box) {
            fits[k] = fits[--nfit];
            if (next_slot >= nfit) next_slot = 0;
            return;
        }
        fits[k].w0 = w, fits[k].h0 = h;
    }
    float tw = fits[k].w0, th = fits[k].h0;
    // (la medida de partida debe llenar el lienzo de ahora: al cambiar de vídeo, un fotograma da el lienzo sin su escala,
    // 2560x1080 en vez de 1707x720, y con él el vídeo salía 1,5 veces más grande ese fotograma)
    if (tall ? fabsf(tw - cw) > 0.02f * cw : fabsf(th - ch) > 0.02f * ch) return;
    if (kind == FIT_COVER || kind == FIT_FADE) {  // el fundido: todo el lienzo
        if (wide || tall) tw = cw, th = ch;
    } else if (g_settings.movie_fit && !loop) {  // el vídeo: cubrir el lienzo con su aspecto (no el fondo del título)
        if (wide) tw = cw, th = (float)(cw / a);
        else if (tall) th = ch, tw = (float)(ch * a);
    }
    bool change = fabsf(tw - w) > 0.5f || fabsf(th - h) > 0.5f;
    if (fabsf(tw - w) > 0.5f) set_size(c, rt, 0, tw);
    if (fabsf(th - h) > 0.5f) set_size(c, rt, 1, th);
    if (change && kind == FIT_MOVIE) {
        static int nlog;
        if (nlog++ < 12)
            LOG("[vídeo] %s: %.0fx%.0f -> %.0fx%.0f (lienzo de %.0fx%.0f)\n",
                g_settings.movie_fit && !loop && (wide || tall) ? "llenar la pantalla" : "en su 16:9", w, h, tw, th, cw, ch);
    }
    if (change && kind == FIT_FADE) {
        static int nlog;
        if (nlog++ < 6) LOG("[fundido] GS.Fade: %.0fx%.0f -> %.0fx%.0f (todo el lienzo)\n", w, h, tw, th);
    }
    fits[k].w1 = tw, fits[k].h1 = th;
}

static void movie_update(Cpu *c) {
    uint64_t self = c->x[0], ret = c->x[30];
    mp_update(c);
    float cw, ch;
    bool loop = false;
    if (alive(self)) {  // (cada vídeo nuevo, una línea: su archivo y si va en bucle)
        static uint64_t last_path;
        uint64_t pl = LD64(self + 0x20), path = pl ? LD64(pl + 0x78) : 0;
        loop = pl && (LD8(pl + 0x80) & 1);  // (CriManaMovieMaterial._loop: el fondo del título, movie_017)
        if (path != last_path) {
            last_path = path;
            char s[96] = {0};
            int n = path ? (int)LD32(path + 0x10) : 0;
            for (int i = 0; i < n && i < 95; i++) s[i] = (char)LD16(path + 0x14 + 2 * i);
            LOG("[vídeo] %s, bucle %d\n", s, pl ? LD8(pl + 0x80) : -1);
        }
    }
    if (alive(self) && canvas_size(c, LD64(self + 0x48), &cw, &ch)) {
        uint64_t img = LD64(self + 0x18), fade = LD64(self + 0x50);
        if (img) fit(c, img, FIT_MOVIE, cw, ch, loop);
        if (fade) fit(c, fade, FIT_COVER, cw, ch, loop);
    }
    c->x[30] = ret;
}

// GS.Fade.Update: tras el original, su Image (m_image, +0x30) cubre todo su lienzo
static void fade_update(Cpu *c) {
    uint64_t self = c->x[0], ret = c->x[30];
    fade_update_orig(c);
    uint64_t img = alive(self) ? LD64(self + 0x30) : 0;
    if (alive(img)) {
        c->x[0] = img, c->x[1] = 0;
        gcall(c, f_canvas);
        uint64_t canvas = c->x[0];
        float cw, ch;
        if (canvas_size(c, canvas, &cw, &ch)) fit(c, img, FIT_FADE, cw, ch, false);
    }
    c->x[30] = ret;
}

// (main.c, tras dispatch_init: antes de que el juego corra)
void smrpg_movie_fit_init(void) {
    dbg = getenv("LARECOMP_LOG_MOVIE_FIT") ? 40 : 0;
    mp_update = dispatch_lookup(MP_UPDATE);
    f_rect_transform = dispatch_lookup(GRAPHIC_RECT_TRANSFORM);
    f_get_rect = dispatch_lookup(RT_GET_RECT);
    f_set_size = dispatch_lookup(RT_SET_SIZE);
    f_pixel_rect = dispatch_lookup(CANVAS_PIXEL_RECT);
    f_scale = dispatch_lookup(CANVAS_SCALE);
    fade_update_orig = dispatch_lookup(FADE_UPDATE);
    f_canvas = dispatch_lookup(GRAPHIC_CANVAS);
    if (!mp_update || !f_rect_transform || !f_get_rect || !f_set_size || !f_pixel_rect || !f_scale) {
        LOG("[vídeo] faltan funciones del juego: los vídeos se quedan en su 16:9\n");
        return;
    }
    static bool registered;  // (boot() se repite en un reinicio dentro del proceso; hle_override solo añade)
    if (!registered) {
        registered = true;
        hle_override(MP_UPDATE, movie_update, "MoviePlayer.Update");
        if (fade_update_orig && f_canvas) hle_override(FADE_UPDATE, fade_update, "GS.Fade.Update");
        else LOG("[fundido] faltan funciones del juego: el fundido de las transiciones se queda en su 16:9\n");
    }
}

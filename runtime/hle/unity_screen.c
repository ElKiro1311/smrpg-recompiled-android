// (SMRPG) Pantalla ancha de verdad en Unity: la pantalla del juego (su backbuffer, sus destinos de render y el aspecto de
// su cámara y de su interfaz) toma el aspecto de la de quien juega, en vez de dibujar 16:9 con bandas.
//
// Unity decide su resolución en una sola función (main 0x70006b7204: 1280x720, o 1920x1080 en modo TV); lasgame.dll la
// sustituye (tools/game_overrides.c) por una que da 1920x1080 y llama aquí. Con «Pantalla ancha» activada y una pantalla
// más ancha que 16:9 (o más alta, como 16:10), el ancho (o el alto) crece hasta su aspecto, en múltiplos de 4 (con 8,
// en 3440x1440 quedaban 2584 de ancho y dos filas de bandas). El renderizador escala desde ese tamaño (g_guest_w /
// g_guest_h: runtime/nvn/output_scale.c, vulkan/textures.c).
// Al arrancar y en marcha: si la ventana cambia de aspecto (otro modo de pantalla, otra resolución, la ventana estirada
// a mano), Unity vuelve a pedir su resolución (rt_unity_screen_follow) y la escena pasa a la nueva, sin estirarse.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../hle.h"
#include "../settings.h"
#include "../nvn/output_scale.h"
#include "../platform.h"

double renderer_display_aspect(void);  // runtime/nvn/vulkan/device.c

// La pantalla del juego para la que Unity eligió (gw x gh) y una de aspecto a: más ancha (o más alta) hasta él
static void screen_for(int gw, int gh, double a, int *w, int *h) {
    *w = gw, *h = gh;
    double g = (double)gw / (double)gh;
    if (g_settings.aspect != ASPECT_UNLOCKED || !(a > 0)) return;
    if (a > g * 1.01) {  // más ancha: el mismo alto, más ancho
        int nw = (int)lround(gh * a / 4.0) * 4;
        if (nw > gw && nw <= 4 * gw) *w = nw;
    } else if (a < g * 0.99) {  // más alta (16:10, 4:3): el mismo ancho, más alto
        int nh = (int)lround(gw / a / 4.0) * 4;
        if (nh > gh && nh <= 2 * gh) *h = nh;
    }
}

static int calls, base_w = 1920, base_h = 1080;  // (las veces que Unity la pidió; lo que eligió la última)

void rt_unity_screen_size(int32_t *w, int32_t *h) {
    if (!w || !h || *w <= 0 || *h <= 0) return;
    int gw = *w, gh = *h, nw, nh;
    double a = renderer_display_aspect();
    screen_for(gw, gh, a, &nw, &nh);
    *w = nw, *h = nh;
    base_w = gw, base_h = gh;
    g_guest_w = *w;
    g_guest_h = *h;
    calls++;
    {   // (al arrancar y en cada cambio: pocas veces)
        extern uint64_t renderer_present_count(void);
        LOG("[unity] pantalla del juego %dx%d (Unity eligió %dx%d; aspecto de la pantalla %.3f, pantalla ancha %s; "
            "llamada %d, fotograma %llu)\n", *w, *h, gw, gh, a, g_settings.aspect == ASPECT_UNLOCKED ? "sí" : "no", calls,
            (unsigned long long)renderer_present_count());
    }
}

// La pantalla del juego que tocaría ahora (con el aspecto de la ventana y el ajuste de ahora); false si es la que tiene.
bool rt_unity_screen_wanted(int *w, int *h) {
    screen_for(base_w, base_h, renderer_display_aspect(), w, h);
    return calls > 0 && (*w != g_guest_w || *h != g_guest_h);
}

// La pantalla del juego sigue a la ventana en marcha. Cuando la ventana cambia de aspecto y se queda quieta un momento,
// se avisa a Unity de un cambio del modo de rendimiento (el mensaje 31 de AM, como al sacar la Switch de su base): su
// manejador (main 0x700062ed24) vuelve a pedir la resolución (main 0x70006b7204, sustituida: rt_unity_screen_size, que
// la ensancha) y la aplica con su SetResolution (0x700062f1d0). La original la recorta a 1920x1080, el máximo de la
// Switch (la escena seguía a 1920 de ancho y salía estirada un 33 %, probado el 11-16): lasgame.dll la sustituye por una
// sin ese recorte (tools/game_overrides.c). Las texturas de ventana de Unity siguen solas (t->screen:
// runtime/nvn/vulkan/textures.c). Hilo principal (device.c ui_run), en cada vuelta.
static bool follow_failed;
void rt_unity_screen_follow(void) {
    static int pend_w, pend_h, asked_w, asked_h, tries;
    static uint64_t pend_since, asked_at;
    int w, h;
    if (!rt_unity_screen_wanted(&w, &h)) {
        pend_w = pend_h = 0;
        follow_failed = false;
        return;
    }
    uint64_t now = plat_now_ns() / 1000000;
    if (w != pend_w || h != pend_h) {  // (otra medida: esperar a que la ventana se quede quieta)
        pend_w = w, pend_h = h, pend_since = now;
        return;
    }
    extern bool am_message_pending(uint32_t msg);
    if (now - pend_since < 300 || am_message_pending(31)) return;  // (aún cambiando; o el juego no lo recogió: pausa)
    if (asked_w == w && asked_h == h) {
        if (now - asked_at < 2000) return;  // (lo aplica en su siguiente imagen)
        if (tries >= 3) {
            if (!follow_failed) LOG("[unity] la pantalla del juego no siguió a la ventana (%dx%d): queda con bandas\n", w, h);
            follow_failed = true;
            return;
        }
    } else tries = 0;
    tries++;
    asked_w = w, asked_h = h, asked_at = now;
    LOG("[unity] la ventana cambió de aspecto: se pide a Unity la pantalla %dx%d (ahora %dx%d)\n", w, h, g_guest_w, g_guest_h);
    extern void am_push_message(uint32_t msg);
    am_push_message(31);
}

// ¿Unity no la siguió? (el menú lo dice: la imagen se queda con bandas hasta volver a abrir el juego)
bool rt_unity_screen_follow_failed(void) { return follow_failed; }

// El aspecto de la pantalla del juego (tools/game_overrides.c: las capturas de pantalla de fondo de los menús)
double rt_unity_screen_aspect(void) { return g_guest_h > 0 ? (double)g_guest_w / (double)g_guest_h : 16.0 / 9.0; }

// ¿URP debe dibujar sus vectores de movimiento? (tools/game_overrides.c, GetRenderPassInputs). Los necesitan AMD FSR
// (Antialiasing: AMD FSR, vulkan/vk_fsr.cpp) y Snapdragon GSR 2 (vulkan/exec.c gsr2_dispatch); LARECOMP_FORCE_MV=1 los
// fuerza para pruebas.
int rt_force_motion_vectors(void) {
    static int env = -1;
    if (env < 0) env = getenv("LARECOMP_FORCE_MV") ? atoi(getenv("LARECOMP_FORCE_MV")) : 0;
    extern bool vk_fsr_available(void);
    return env || (g_settings.antialias == AA_FSR && vk_fsr_available()) || g_settings.antialias == AA_GSR2;
}

// La interfaz en pantalla ancha. El CanvasScaler de Unity (modo «escalar con la pantalla») calcula la escala de cada
// lienzo a partir del tamaño de la pantalla y de su resolución de referencia; el juego está diseñado para 16:9 y con
// el ajuste «ancho» (el habitual) en 21:9 la escala sale del ancho: la interfaz crece y se corta por arriba y por abajo
// (la pantalla de título). tools/game_overrides.c sustituye CanvasScaler.HandleScaleWithScreenSize y, antes de calcular,
// pasa aquí el tamaño: si es el de la pantalla del juego ensanchada, se cambia por el 16:9 que cabe dentro (lo que se
// vería en la consola). Así cada elemento conserva el tamaño que tiene en 16:9 y el lienzo ocupa toda la pantalla: lo
// anclado a los bordes va a los bordes. En 16:9 no cambia nada.
void rt_unity_ui_scale_screen(float *w, float *h) {
    if (!w || !h || *w <= 0 || *h <= 0) return;
    if (fabsf(*w - (float)g_guest_w) > 1.f || fabsf(*h - (float)g_guest_h) > 1.f) return;  // (otro destino: igual)
    float vw = *w, vh = *h;
    if (vw * 9.f > vh * 16.f + 8.f) vw = vh * 16.f / 9.f;
    else if (vh * 16.f > vw * 9.f + 8.f) vh = vw * 9.f / 16.f;
    else return;
    static int logged;
    if (!logged++) LOG("[unity] interfaz: escala calculada para %.0fx%.0f (16:9 dentro de %.0fx%.0f)\n", vw, vh, *w, *h);
    *w = vw;
    *h = vh;
}

// (SMRPG Android) El ritmo de presentación con VK_GOOGLE_display_timing: cada imagen del juego en pantalla un número fijo
// de refrescos. El juego marca sus imágenes con el reloj de hle/vi.c (60 por segundo, o el «Límite de FPS»), que no iba
// con los refrescos de la pantalla: en MAILBOX cada imagen salía en el primer refresco tras acabarse en la GPU, y con
// 60 fps en una pantalla de 120 Hz las que se acababan cerca de un refresco salían unas antes y otras después: 8 ms en
// pantalla unas, 25 otras (AYN Thor, 10 minutos: 1.080 y 1.081 de 32.367 imágenes, unos 2 tirones por segundo).
// Ahora:
// - cada presentación pide su momento (VkPresentTimesInfoGOOGLE: «no antes de») en la cuadrícula de refrescos que dan
//   las presentaciones pasadas (vkGetPastPresentationTimingGOOGLE: cuándo salió de verdad cada una), a paso fijo: el
//   periodo del vsync del juego en refrescos (2 a 60 fps y 120 Hz; si no es un número entero de refrescos —90 fps a
//   120 Hz—, sin momento, como antes). Llega antes de su momento (el adelanto) y la pantalla la retiene hasta él; una que
//   llega hasta medio refresco más tarde de lo que pide el adelanto aún va en su sitio. Si salen tarde 3 en 60 imágenes
//   (después de lo normal en esa pantalla —en la Thor, un refresco antes del pedido: su compositor presenta pronto— y no
//   porque las retuviera el compositor; una suelta no, que rehacer el paso por ella daría otro tirón), la siguiente
//   rehace el paso con más adelanto, hasta 2 imágenes (también al bajar el paso: el adelanto de los 30 fps no pasa a un
//   combate a 60); las de justo después de un parón (una carga, volver del segundo plano) no cuentan.
// - el vsync del juego cae en un refresco de la pantalla (vk_pacing_vsync, desde vi.c): el reloj de la CPU y el de la
//   pantalla no van exactos (un 0,01 % ya es una imagen de desfase cada minuto y pico), y sin esto el paso se rehacía
//   (un tirón) cada vez que se desfasaban un refresco. Solo si la pantalla va a menos de un 0,5 % de ese ritmo: el juego
//   va así un 0,5 % más rápido o más lento como mucho.
// - (Android) se pide a la pantalla la frecuencia del ritmo del juego (vote_rate: 60 Hz con 60 fps), porque a 120 Hz el
//   compositor de la Thor va tan justo que una imagen de cada ~5 s salía un refresco tarde.
// Solo el camino normal: con la generación de Lossless Scaling (FIFO, vk_lsfg.c) cada imagen ya va a un refresco.
// Por omisión solo en Android (donde se probó). LARECOMP_VK_DISPLAY_TIMING=0 lo quita (=1 lo prueba en otro sistema con la
// extensión); LARECOMP_VK_PACING_VSYNC=0 deja el vsync con su reloj; LARECOMP_VK_PACING_LOG=<n>: de las <n> primeras
// imágenes con ritmo, en crudo, el refresco pedido y cuándo salieron (líneas [ritmo] d y p).
#include "vk.h"
#include "../../settings.h"
#include <stdlib.h>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif

uint64_t plat_now_ns(void);
extern volatile uint64_t g_vsync_period_ns;  // (hle/vi.c) el periodo del vsync del juego ahora (0 antes del primero)
extern uint64_t (*volatile g_vsync_follow)(uint64_t next, uint64_t period);  // (hle/vi.c)

#define LD(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define ST(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)

static struct {
    bool on;
    PFN_vkGetRefreshCycleDurationGOOGLE refresh_fn;
    PFN_vkGetPastPresentationTimingGOOGLE past_fn;
    // (el hilo que presenta; device.c los pone a cero con él parado. refresh, anchor y paced_at los lee también el hilo
    // del vsync: LD / ST)
    VkSwapchainKHR sc;
    uint64_t refresh;     // ns por refresco de la pantalla (lo que mide el compositor)
    uint64_t refresh_at;  // cuándo se preguntó por él
    uint64_t anchor;      // el momento real de un refresco (el de la última presentación pasada); 0: aún no se sabe
    uint64_t last;        // el refresco pedido para la última imagen con ritmo; 0: la siguiente rehace el paso
    uint64_t steps;       // refrescos por imagen
    uint64_t lead;        // ns: cuánto antes de su refresco tiene que llegar una imagen (adelanto)
    uint64_t paced_at;    // cuándo se presentó la última imagen con ritmo (el vsync solo lo sigue mientras haya)
    uint64_t prev_now;    // cuándo se pidió el último momento (0: la cadena es nueva)
    // la última presentada; desde cuál cuentan las tardías (las de antes ya no dicen nada); hasta cuál no suben el adelanto
    // (las de justo después de un parón)
    uint32_t id, rephase_id, grace_id;
    uint32_t late_ids[2];  // las dos últimas tardías sueltas (0: ninguna)
    int early;  // > 0: la pantalla saca las imágenes un refresco antes del pedido (-64..64: las últimas mandan)
    // (Android) la frecuencia que se pide a la pantalla (vote_rate): la ventana nativa de la cadena, lo pedido (-1: nada
    // aún en esta ventana) y hasta cuándo se pregunta a menudo por el refresco (tras pedir otra frecuencia)
    bool match;
    void *anw;
    float voted;
    float sys_hz;  // la frecuencia de la pantalla al empezar, antes de pedir nada (la del sistema; 0: aún no se sabe)
    uint64_t fast_until;
    int log_n;
    uint64_t n_paced, n_late, n_held, n_rephase, log_at;  // (para el registro, cada minuto)
} P;

static int env_int(const char *name, int def) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : def;
}

// (device.c, al elegir las extensiones) si se quiere: por omisión solo en Android
bool vk_pacing_wanted(void) {
#ifdef __ANDROID__
    return env_int("LARECOMP_VK_DISPLAY_TIMING", 1) != 0;
#else
    return env_int("LARECOMP_VK_DISPLAY_TIMING", 0) != 0;
#endif
}

// el vsync siguiente del juego (hle/vi.c, en su hilo: next = el anterior + period) en el refresco más cercano: el paso
// queda en un número entero de refrescos y en fase con la pantalla
static uint64_t vk_pacing_vsync(uint64_t next, uint64_t period) {
    uint64_t r = LD(P.refresh), anchor = LD(P.anchor), paced = LD(P.paced_at);
    if (!r || !anchor || !paced || (int64_t)(next - paced) > 500000000) return next;  // (sin imágenes con ritmo: su reloj)
    uint64_t steps = (period + r / 2) / r;
    int64_t off = (int64_t)(steps * r) - (int64_t)period;
    if (!steps || (uint64_t)(off < 0 ? -off : off) > period / 200) return next;
    int64_t d = (int64_t)(next - anchor);
    if (d < 0 || d > 2000000000) return next;
    return anchor + ((uint64_t)d + r / 2) / r * r;
}

// (device.c, tras crear el dispositivo) ext: VK_GOOGLE_display_timing activada (vk_pacing_wanted)
void vk_pacing_init(bool ext) {
    if (ext) {
        P.refresh_fn = (PFN_vkGetRefreshCycleDurationGOOGLE)vkGetDeviceProcAddr(g_vk.dev, "vkGetRefreshCycleDurationGOOGLE");
        P.past_fn = (PFN_vkGetPastPresentationTimingGOOGLE)vkGetDeviceProcAddr(g_vk.dev, "vkGetPastPresentationTimingGOOGLE");
        P.on = P.refresh_fn && P.past_fn;
    }
    P.log_n = env_int("LARECOMP_VK_PACING_LOG", 0);
#ifdef __ANDROID__
    P.match = env_int("LARECOMP_VK_MATCH_REFRESH", 1) != 0;
#endif
    bool follow = P.on && env_int("LARECOMP_VK_PACING_VSYNC", 1) != 0;
    if (follow) g_vsync_follow = vk_pacing_vsync;
    LOG("[vk] ritmo de presentación: %s%s\n", P.on ? "cada imagen con su momento (VK_GOOGLE_display_timing)"
                                         : ext ? "sin momentos (faltan las funciones de VK_GOOGLE_display_timing)"
                                               : "cada imagen en cuanto se acaba",
        P.on ? (follow ? "; el vsync del juego, en los refrescos de la pantalla" : "; el vsync del juego, con su reloj") : "");
}

static void ask_refresh(VkSwapchainKHR sc, uint64_t now) {
    VkRefreshCycleDurationGOOGLE d = {0};
    P.refresh_at = now;
    if (P.refresh_fn(g_vk.dev, sc, &d) != VK_SUCCESS || !d.refreshDuration) return;
    uint64_t old = P.refresh;
    if (old && (d.refreshDuration > old ? d.refreshDuration - old : old - d.refreshDuration) * 100 < old) {
        ST(P.refresh, d.refreshDuration);  // (lo que el compositor va midiendo: la misma cuadrícula)
        return;
    }
    LOG("[vk] ritmo: refresco de la pantalla %.4f ms (%.2f Hz)\n", d.refreshDuration / 1e6, 1e9 / (double)d.refreshDuration);
    if (!P.sys_hz) P.sys_hz = (float)(1e9 / (double)d.refreshDuration);
    ST(P.refresh, d.refreshDuration);
    ST(P.anchor, 0);  // (otra frecuencia —la que se pide, o el ajuste de 60 / 120 Hz del sistema—: otra cuadrícula)
    // y el adelanto de partida y lo normal de esa pantalla, de nuevo. El adelanto: la GPU (~12 ms en la Thor) y que el
    // compositor empieza a componer antes del refresco (en la Thor, 10,2 ms a 120 Hz: 1,2 refrescos): 22,4 ms a 120 Hz
    // (con 22, 46 tirones en 5 minutos; con 16, 362, aunque en esa prueba el compositor iba además más cargado)
    P.lead = 12000000 + d.refreshDuration + d.refreshDuration / 4;
    P.last = 0, P.early = 0, P.late_ids[0] = P.late_ids[1] = 0, P.prev_now = 0;
}

// (SMRPG Android) La frecuencia de la pantalla que se pide para el ritmo del juego (ANativeWindow_setFrameRate): con 60 fps,
// 60 Hz. A 120 Hz, en la Thor, el compositor empieza a componer solo 1,85 ms antes del refresco y presenta pronto si el
// envío al panel llega antes de ~0,7 ms del vsync: llega con 0,2-0,4 ms de sobra, y cuando el compositor, el servicio de
// pantalla o el kernel tardan un poco más (traza de atrace), la imagen sale un refresco tarde: el tirón que quedaba cada
// ~5 s. A 60 Hz empieza ~15 ms antes. Con la compatibilidad DEFAULT, porque solo así la capa en primer plano puede salir
// del rango que fija el sistema (en la Thor, 120 Hz fijos); Android vuelve a su frecuencia al salir del juego. Con la
// generación de Lossless Scaling, la que da (el ritmo por el multiplicador: 120 con x2; si no la hay, Android pone la más
// alta): retirar lo pedido no bastaba, la Thor se quedaba a 60 Hz y la generación no podía sacar sus imágenes. Con
// «Frecuencia de la pantalla: la del sistema» en el menú (match_refresh), la que tenía la pantalla al empezar, pedida
// también (por lo mismo: retirar lo pedido no la devuelve). LARECOMP_VK_MATCH_REFRESH=0: nada nunca.
static bool gen_rate(void) { return g_vk.sc_lsfg && g_settings.lsfg > 1; }
static float wanted_rate(uint64_t period) {
    if (!period) return 0.0f;
    float hz = (float)(1e9 / (double)period);
    if (gen_rate()) return hz * (float)g_settings.lsfg;
    return g_settings.match_refresh ? hz : P.sys_hz;
}
static void vote_rate(float hz, uint64_t now) {
#ifdef __ANDROID__
    if (!P.match || !P.anw || hz == P.voted) return;
    int r = ANativeWindow_setFrameRate((ANativeWindow *)P.anw, hz, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
    if (hz > 0) LOG("[vk] ritmo: pide a la pantalla %.0f Hz (%s)%s\n", hz, gen_rate() ? "la generación de Lossless Scaling"
                    : g_settings.match_refresh ? "el ritmo del juego" : "la del sistema", r ? " — rechazado" : "");
    else LOG("[vk] ritmo: deja a la pantalla su frecuencia%s\n", r ? " — rechazado" : "");
    P.voted = hz;
    P.fast_until = now + 3000000000ull;  // (el cambio de modo: se ve antes)
#else
    (void)hz, (void)now;
#endif
}

// (device.c create_swapchain y surface_drop, con el hilo que presenta parado) otra cadena (o ninguna): su refresco, y el
// paso y la cuadrícula de cero
void vk_pacing_swapchain(VkSwapchainKHR sc) {
    P.sc = sc, P.last = 0, P.prev_now = 0;
    ST(P.anchor, 0);
    ST(P.paced_at, 0);
    extern void *vk_android_window(void);
    P.anw = sc ? vk_android_window() : NULL, P.voted = -1;  // (otra ventana: se le pide de nuevo)
    if (!sc) return;
    if (P.on) ask_refresh(sc, plat_now_ns());  // (antes de pedir nada: la primera vez, la frecuencia del sistema)
    // (ya al crear la cadena: con la generación de Lossless Scaling sus presentaciones no pasan por vk_pacing_time, y
    // tienen que ir ya a su frecuencia)
    vote_rate(wanted_rate(LD(g_vsync_period_ns)), plat_now_ns());
}

static uint64_t grid_round(uint64_t t) {  // el refresco de la cuadrícula más cercano a t
    int64_t d = (int64_t)(t - P.anchor), r = (int64_t)P.refresh;
    return P.anchor + (uint64_t)((d >= 0 ? (d + r / 2) / r : -((-d + r / 2) / r)) * r);
}
static uint64_t grid_ceil(uint64_t t) {  // el primero que no es antes de t
    int64_t d = (int64_t)(t - P.anchor), r = (int64_t)P.refresh;
    return P.anchor + (uint64_t)((d >= 0 ? (d + r - 1) / r : -((-d) / r)) * r);
}

// (record.c present_run, en el hilo que presenta) el momento que pide esta presentación: true con *t lleno
// (desiredPresentTime 0 mientras no se sabe la cuadrícula: la presentación lleva su número para que las pasadas la den),
// false sin él
bool vk_pacing_time(VkSwapchainKHR sc, VkPresentTimeGOOGLE *t) {
    if (!P.on || sc != P.sc || !sc) return false;
    uint64_t now = plat_now_ns(), period = LD(g_vsync_period_ns);
    vote_rate(wanted_rate(period), now);
    if (g_vk.sc_lsfg) return false;
    // (cambiar de frecuencia no rehace la cadena: se pregunta cada 2 s, y cada 100 ms tras pedir otra)
    if (now - P.refresh_at > (now < P.fast_until ? 100000000ull : 2000000000ull)) ask_refresh(sc, now);
    uint64_t r = P.refresh;
    if (!r || !period) return false;
    uint64_t steps = (period + r / 2) / r;
    if (!steps) steps = 1;
    int64_t off = (int64_t)(steps * r) - (int64_t)period;
    if ((uint64_t)(off < 0 ? -off : off) > period / 25) { P.last = 0; return false; }  // (no son refrescos enteros)
    if (steps != P.steps) P.steps = steps, P.last = 0;
    // el adelanto, hasta 2 imágenes (más, y las retenidas no caben en la cadena); también al bajar el paso: de 30 fps a un
    // combate, que va a 60, o de 60 a 120
    uint64_t cap = 2 * steps * r;
    if (P.lead > cap) P.lead = cap;
    // las presentaciones pasadas (Android las da cuando hay 4 más nuevas): la cuadrícula y las que salieron tarde
    for (int round = 0; round < 4; round++) {
        VkPastPresentationTimingGOOGLE past[16];
        uint32_t n = 16;
        VkResult pr = P.past_fn(g_vk.dev, sc, &n, past);
        if (pr != VK_SUCCESS && pr != VK_INCOMPLETE) break;
        for (uint32_t i = 0; i < n; i++) {
            const VkPastPresentationTimingGOOGLE *p = &past[i];
            if (P.log_n > 0)  // (en crudo, ns: id, pedido, real, el más temprano posible, margen)
                LOG("[ritmo] p %u %llu %llu %llu %lld\n", p->presentID, (unsigned long long)p->desiredPresentTime,
                    (unsigned long long)p->actualPresentTime, (unsigned long long)p->earliestPresentTime,
                    (long long)p->presentMargin);
            if (!p->actualPresentTime) continue;  // (no salió, o sin sus tiempos)
            ST(P.anchor, p->actualPresentTime);
            if (!p->desiredPresentTime) continue;
            // dónde salió respecto a su refresco (desired + r/2), en refrescos. Lo normal no es 0 en todas las pantallas: la
            // de la Thor saca casi todas un refresco ANTES (su compositor empieza a componer 10,3 ms antes del refresco y, si
            // acaba en ~2 ms, presenta en el anterior: «Early Present»), así que lo normal se aprende (early)
            int64_t d = (int64_t)(p->actualPresentTime - (p->desiredPresentTime + r / 2)), ri = (int64_t)r;
            int64_t k = d >= 0 ? (d + ri / 2) / ri : -((-d + ri / 2) / ri);
            if (p->presentID > P.grace_id) P.early += k < 0 ? (P.early < 64) : -(P.early > -64);
            int64_t base = P.early > 0 ? -1 : 0;
            if (k <= base) continue;
            // salió después de lo normal. Si pudo salir antes (Android lo calcula: el compositor la tomó más de un refresco
            // antes de presentarla, con la GPU ya acabada), la retuvo el compositor (en la Thor, cuando se pasa de sus 2 ms):
            // nada que corregir aquí
            if (p->earliestPresentTime && p->earliestPresentTime < p->actualPresentTime) { P.n_held++; continue; }
            // si no, la GPU o la entrega no llegaron a tiempo. Una suelta no dice nada (en la Thor salen unas pocas por minuto
            // con cualquier adelanto, aun con el máximo: la CPU o el compositor) y rehacer el paso por ella daría otro tirón;
            // 3 en 60 imágenes sí: el adelanto no basta. Entonces la siguiente rehace el paso con más adelanto, y las
            // presentadas hasta ella, con el de antes, ya no cuentan. Las de justo después de un parón (una carga, volver del
            // segundo plano, la cadena nueva) no cuentan: salen tarde de paso (la GPU aún arranca), y si contaran el adelanto
            // quedaría alto toda la partida
            if (p->presentID < P.rephase_id) continue;
            bool grace = p->presentID <= P.grace_id, more = false;
            if (!grace) {
                P.n_late++;
                more = P.late_ids[0] && p->presentID - P.late_ids[0] <= 60;
                if (more) {
                    P.last = 0, P.rephase_id = P.id + 1, P.late_ids[0] = P.late_ids[1] = 0;
                    if (P.lead + r / 4 <= cap) P.lead += r / 4;
                } else {
                    P.late_ids[0] = P.late_ids[1], P.late_ids[1] = p->presentID;
                }
            }
            if (P.log_n > 0 || more)
                LOG("[vk] ritmo: la %u salió %lld refresco%s después de lo normal%s; adelanto %.2f ms\n", p->presentID,
                    (long long)(k - base), k - base == 1 ? "" : "s",
                    grace ? " (tras un parón: no cuenta)" : more ? " (3 en 60 imágenes: el paso de nuevo, con más adelanto)" : "",
                    P.lead / 1e6);
        }
        if (pr == VK_SUCCESS) break;
    }
    t->presentID = ++P.id;
    t->desiredPresentTime = 0;
    if (!P.prev_now || now - P.prev_now > 100000000ull) P.grace_id = P.id + 60;  // (un parón: 1 s a 60 fps)
    P.prev_now = now;
    if (!P.anchor) return true;
    // su refresco: el del paso (el anterior + steps refrescos) si le da tiempo —hasta medio refresco menos del adelanto— y
    // no le sobra una imagen entera (el juego se adelantó); si no, el primero con todo el adelanto
    uint64_t earliest = now + P.lead, natural = grid_ceil(earliest), target = natural;
    bool keep = false;
    if (P.last) {
        uint64_t want = grid_round(P.last + steps * r);
        keep = want + r / 2 >= earliest && want < natural + steps * r;
        if (keep) target = want;
    }
    if (!keep) P.n_rephase++, P.rephase_id = P.id;
    P.last = target;
    t->desiredPresentTime = target - r / 2;  // (sale en el refresco cuyo momento no es antes de este)
    P.n_paced++;
    ST(P.paced_at, now);
    if (P.log_n > 0) {  // (en crudo, ns: id, ahora, el último vsync del juego, el refresco pedido, la cuadrícula, paso de siempre)
        P.log_n--;
        extern volatile uint64_t g_vsync_ns;
        LOG("[ritmo] d %u %llu %llu %llu %llu %d\n", P.id, (unsigned long long)now, (unsigned long long)LD(g_vsync_ns),
            (unsigned long long)target, (unsigned long long)P.anchor, keep);
    }
    if (!P.log_at) P.log_at = now;
    if (now - P.log_at >= 60000000000ull) {
        LOG("[vk] ritmo: en 1 min %llu imágenes con su momento (%llu refrescos cada una), %llu tarde, %llu retenidas por el "
            "compositor, %llu veces el paso de nuevo; adelanto %.2f ms; la pantalla las saca %s; refresco %.4f ms\n",
            (unsigned long long)P.n_paced, (unsigned long long)steps, (unsigned long long)P.n_late,
            (unsigned long long)P.n_held, (unsigned long long)P.n_rephase, P.lead / 1e6,
            P.early > 0 ? "un refresco antes de lo pedido" : "en lo pedido", r / 1e6);
        P.n_paced = P.n_late = P.n_held = P.n_rephase = 0, P.log_at = now;
    }
    return true;
}

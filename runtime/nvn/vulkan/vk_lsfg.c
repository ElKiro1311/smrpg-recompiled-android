// (SMRPG) La generación de fotogramas de Lossless Scaling (LSFG 3.1) dentro del port, con la biblioteca aparte de
// tools/lsfg (lsfg.dll / liblsfg.so: lsfg-vk-android en modo de un solo dispositivo). Menú > Vídeo > «Generación
// Lossless Scaling»: x2, x3 o x4. Lo que cuesta (el panel de impacto del menú y el registro): marcas de tiempo de la GPU
// alrededor de su trabajo y la memoria de vídeo que toma, según el controlador.
//
// Cada fotograma real (vk_lsfg_frame_end, desde vk_frame_end, en el hilo que traduce): la imagen de la ventana se copia a
// una de las dos entradas de LSFG (por turnos) con sus mismos comandos, que se envían como siempre. Después, en el hilo
// grabador y en orden (rec_QueuePresentCall: es el de la cola), LSFG genera las N imágenes intermedias entre la real
// anterior y esta (envía su trabajo a la cola, detrás del fotograma), cada una se copia a una imagen de la cadena con un
// búfer de comandos propio y se presenta (como lsfg-vk-android en su capa, src/context.cpp, camino de Android); la real
// se presenta en el ls_run siguiente, antes de las generadas de entonces (pend, abajo: por qué). La cadena va en FIFO con
// N + 1 imágenes más (create_swapchain, vk_lsfg_extra_images): cada imagen se ve un refresco, y el juego va a la
// frecuencia de la pantalla / (N + 1).
// Sin la biblioteca, sin el Lossless.dll del jugador, sin lo que piden sus sombreadores, con HDR o con la generación de
// AMD: no se usa (vk_lsfg_status lo dice en el menú). Con el menú abierto, solo la real (y la primera de después, sin
// intermedias: la anterior sería de antes del menú).
// Lossless.dll: LARECOMP_LSFG_DLL; en Android, el de la carpeta de datos; en Windows, el que esté junto al ejecutable o
// el de Steam (la clave SteamPath del registro y las bibliotecas de su libraryfolders.vdf). Nunca va con el port.
// Pruebas: LARECOMP_SET lsfg / lsfg_flow / lsfg_perf; LARECOMP_LSFG_DUMP=<desde>[,<cuántos>[,<cada cuántos>]] (en
// fotogramas reales): la entrada anterior, las generadas y la real, en PNG (lsfg_<n>_0anterior, _1generada.., _<N+1>real);
// LARECOMP_LSFG_ORIG_MIPMAPS=1; LARECOMP_LSFG_FAIL_AT=<real>: una generación fallida de prueba (el contexto se rehace).
#include "vk.h"
#include "record.h"
#include "../../settings.h"
#include "../../../tools/lsfg/lsfg_smrpg.h"
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define LS_MAX_GEN 3  // (x4)
#define LS_SLOTS 8

extern bool menu_is_open(void);
extern bool vk_fg_active(void);
extern void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
extern void rec_drain(void);
extern void vk_queue_lock(void);  // (record.c) la cola, con su cerrojo
extern void vk_queue_unlock(void);

static struct {
    int lib_state;  // -1 sin probar, 0 no está, 1 cargada
    SDL_SharedObject *lib;
    PFN_lsfg_smrpg_init init;
    PFN_lsfg_smrpg_create create;
    PFN_lsfg_smrpg_present present;
    PFN_lsfg_smrpg_destroy destroy;
    PFN_lsfg_smrpg_finalize finalize;
    int dll_state;  // -1 sin buscar, 0 no está, 1 encontrado (dll)
    char dll[1024];
    int up_mult, up_flow, up_perf;  // con qué está iniciada la biblioteca (up_mult 0: no)
    int bad_mult, bad_flow, bad_perf;  // con qué falló (no se reintenta con lo mismo)
    char status[600];
    int ctx;          // el contexto de LSFG (-1: ninguno)
    uint32_t w, h;    // su tamaño
    int n;            // imágenes generadas por cada real
    VkImage img[2 + LS_MAX_GEN];  // 0, 1: las entradas; 2..: las salidas
    VmaAllocation mem[2 + LS_MAX_GEN];
    // del hilo grabador
    VkCommandPool pool;
    VkCommandBuffer cb[LS_SLOTS][LS_MAX_GEN];
    VkFence fence[LS_SLOTS][LS_MAX_GEN];
    VkSemaphore acq[LS_SLOTS][LS_MAX_GEN], gen[LS_SLOTS][LS_MAX_GEN];
    // (el grabador) lo que cuesta en la GPU: una marca de tiempo antes de su trabajo (cuando acaba lo anterior: el
    // fotograma) y otra después, por hueco; se leen LS_SLOTS fotogramas más tarde, ya terminadas
    VkQueryPool qp;
    VkCommandBuffer tcb[LS_SLOTS][2];
    bool ts_used[LS_SLOTS];
    double gpu_ms;    // media, por fotograma real (0: sin medir)
    uint64_t vram_lib, vram_ctx;  // la memoria de vídeo suya (no pasa por VMA) al iniciarse y con el contexto
    bool vram_measured;           // (con VK_EXT_memory_budget)
    uint64_t frames;  // (el que traduce) fotogramas reales dados a LSFG
    bool skip_next;   // (el que traduce) el siguiente, sin intermedias
    uint64_t st_real, st_gen, st_skip, st_fail;  // (el grabador)
    // (el grabador) falló su generación o el envío de una generada: su cuenta (frameIdx, que solo avanza si todo va
    // bien) o una de sus vallas ya no casan con las nuestras. El que traduce rehace el contexto (vk_lsfg_frame_end).
    atomic_bool broken;
    int rebuilds;      // (el que traduce) contextos rehechos por eso, seguidos
    uint64_t last_ns;  // (el que traduce) cuándo le llegó el último real
    uint64_t pw_last;  // (el grabador) el tiempo dentro de vkQueuePresentKHR hasta la última línea de cuentas
    double init_ms;    // (el que traduce) lo que tardó en iniciarse la biblioteca, para el registro
    // (el que traduce, battle_slow) en este combate, sin generar; y la cuenta de reales por segundos
    bool bt_slow;
    uint64_t bt_t0, bt_last;
    int bt_n, bt_low;
} L = {.lib_state = -1, .dll_state = -1, .ctx = -1};

// (el grabador) la real del último fotograma, aún sin presentar. En Android el cargador de Vulkan conecta la ventana como
// EGL, y para EGL su queueBuffer espera la valla de la imagen encolada justo antes (BufferQueueProducer, «Throttling EGL
// Production»): presentada detrás de su generada, la real dejaba el grabador parado dentro de vkQueuePresentKHR hasta que
// la GPU acababa el fotograma y la generación (14,9 ms por real en la Thor a 720p con x2; sin LSFG, 2,7): mientras, nadie
// enviaba el fotograma siguiente, y la GPU se quedaba al 75 % con el juego a 52 reales. Ahora se presenta en el ls_run
// siguiente (la generada de antes ya está hecha: no espera; se ve detrás de ella, casi siempre en el mismo refresco que
// antes: de media, unos 3 ms más tarde), o antes de una presentación normal (present_run: el menú abierto, LSFG apagada) o de rehacer o
// soltar la cadena (device.c, con el grabador parado). Su semáforo es el de su imagen (device.c done_sem[img]): no se
// vuelve a usar hasta que esa imagen se presente.
static struct { bool on; VkSemaphore wait; VkSwapchainKHR sc; uint32_t img; } pend;
// quiet: antes de rehacer o soltar la cadena (sin pedir otra si responde OUT_OF_DATE o SURFACE_LOST: su semáforo se
// consume igual, como dice vkQueuePresentKHR)
static void pend_present(bool quiet) {
    if (!pend.on) return;
    pend.on = false;
    if (!quiet) { rec_present_direct(pend.wait, pend.sc, pend.img); return; }
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &pend.wait,
                           .swapchainCount = 1, .pSwapchains = &pend.sc, .pImageIndices = &pend.img};
    vk_queue_lock();
    vkQueuePresentKHR(g_vk.queue, &pi);
    vk_queue_unlock();
}
void vk_lsfg_flush_pending(void) { pend_present(false); }  // (record.c present_run, en el grabador)
void vk_lsfg_drop_pending(void) { pend_present(true); }    // (device.c recreate_swapchain / surface_drop, tras rec_drain)

static void status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char s[sizeof L.status];
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    if (strcmp(s, L.status)) {
        snprintf(L.status, sizeof L.status, "%s", s);
        LOG("[lsfg] %s\n", s);
    }
}
const char *vk_lsfg_status(void) { return L.status[0] ? L.status : "sin usar todavía"; }

static bool lib_load(void) {
    if (L.lib_state >= 0) return L.lib_state > 0;
    L.lib_state = 0;
#ifdef _WIN32
    char path[1024];
    const char *base = SDL_GetBasePath();
    snprintf(path, sizeof path, "%slsfg.dll", base ? base : "");
#else
    const char *path = "liblsfg.so";
#endif
    L.lib = SDL_LoadObject(path);
    if (!L.lib) { status("no está la biblioteca %s (%s)", path, SDL_GetError()); return false; }
    PFN_lsfg_smrpg_abi abi = (PFN_lsfg_smrpg_abi)SDL_LoadFunction(L.lib, "lsfg_smrpg_abi");
    L.init = (PFN_lsfg_smrpg_init)SDL_LoadFunction(L.lib, "lsfg_smrpg_init");
    L.create = (PFN_lsfg_smrpg_create)SDL_LoadFunction(L.lib, "lsfg_smrpg_create");
    L.present = (PFN_lsfg_smrpg_present)SDL_LoadFunction(L.lib, "lsfg_smrpg_present");
    L.destroy = (PFN_lsfg_smrpg_destroy)SDL_LoadFunction(L.lib, "lsfg_smrpg_destroy");
    L.finalize = (PFN_lsfg_smrpg_finalize)SDL_LoadFunction(L.lib, "lsfg_smrpg_finalize");
    if (!abi || !L.init || !L.create || !L.present || !L.destroy || !L.finalize || abi() != LSFG_SMRPG_ABI) {
        status("la biblioteca %s no es de esta versión del port", path);
        SDL_UnloadObject(L.lib);
        L.lib = NULL;
        return false;
    }
    L.lib_state = 1;
    return true;
}

static bool file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
#ifdef _WIN32
// las bibliotecas de Steam: SteamPath del registro y las «path» de su steamapps/libraryfolders.vdf
static bool steam_find(char *out, size_t n) {
    char sp[512];
    DWORD sz = sizeof sp;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, sp, &sz) != ERROR_SUCCESS)
        return false;
    snprintf(out, n, "%s/steamapps/common/Lossless Scaling/Lossless.dll", sp);
    if (file_exists(out)) return true;
    char vdf[600];
    snprintf(vdf, sizeof vdf, "%s/steamapps/libraryfolders.vdf", sp);
    FILE *f = fopen(vdf, "rb");
    if (!f) return false;
    char line[1200];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        char *k = strstr(line, "\"path\"");
        if (!k) continue;
        char *a = strchr(k + 6, '"'), *b = a ? strchr(a + 1, '"') : NULL;
        if (!a || !b) continue;
        char lib[600];
        size_t m = 0;
        for (char *c = a + 1; c < b && m + 1 < sizeof lib; c++) {
            if (c[0] == '\\' && c[1] == '\\') c++;  // (el VDF escapa las barras)
            lib[m++] = *c;
        }
        lib[m] = 0;
        snprintf(out, n, "%s/steamapps/common/Lossless Scaling/Lossless.dll", lib);
        found = file_exists(out);
    }
    fclose(f);
    return found;
}
#endif
static bool dll_find(void) {
    if (L.dll_state >= 0) return L.dll_state > 0;
    L.dll_state = 0;
    const char *e = getenv("LARECOMP_LSFG_DLL");
    if (e && *e) snprintf(L.dll, sizeof L.dll, "%s", e);
#ifdef __ANDROID__
    else {
        const char *root = getenv("LARECOMP_DATA_ROOT");
        snprintf(L.dll, sizeof L.dll, "%s/Lossless.dll", root ? root : ".");
    }
#elif defined(_WIN32)
    else {
        const char *base = SDL_GetBasePath();
        snprintf(L.dll, sizeof L.dll, "%sLossless.dll", base ? base : "");
        if (!file_exists(L.dll) && !steam_find(L.dll, sizeof L.dll)) L.dll[0] = 0;
    }
#else
    else snprintf(L.dll, sizeof L.dll, "Lossless.dll");
#endif
    if (!L.dll[0] || !file_exists(L.dll)) {
#ifdef __ANDROID__
        status("falta Lossless.dll: cópialo de tu Lossless Scaling (Steam) a la carpeta de datos del juego");
#else
        status("no se encontró Lossless.dll: instala Lossless Scaling desde Steam (o ponlo junto al juego)");
#endif
        return false;
    }
    L.dll_state = 1;
    return true;
}

// ¿Se usa? (create_swapchain: FIFO y N imágenes más; vk_frame_begin rehace la cadena si cambia)
bool vk_lsfg_wanted(void) {
    int m = g_settings.lsfg;
    if (m < 2 || m > 4) { status("apagada"); return false; }  // (el estado, para el menú: por qué no va)
    if (g_settings.hdr) { status("no va con HDR (apágalo para usarla)"); return false; }
    if (vk_fg_active()) { status("no va a la vez que la generación de AMD"); return false; }
    if (!g_vk.lsfg_features) {
        status("la GPU no tiene lo que piden sus sombreadores (formatos de imagen extendidos, modelo de memoria de Vulkan)");
        return false;
    }
    if (!lib_load() || !dll_find()) return false;
    int flow = g_settings.lsfg_flow, perf = g_settings.lsfg_perf;
    return !(m == L.bad_mult && flow == L.bad_flow && perf == L.bad_perf);
}
// (una por cada generada, que se adquieren después de la real, otra porque la real espera al fotograma siguiente —pend—
// y otra de holgura: con una menos, en la Thor, el que traduce esperaba 3-7 ms por fotograma a que hubiera imagen)
uint32_t vk_lsfg_extra_images(void) { return vk_lsfg_wanted() ? (uint32_t)g_settings.lsfg + 1 : 0; }

static void barrier(VkCommandBuffer cb, bool rec, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
                    VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    if (rec) rec_CmdPipelineBarrier2(cb, &di);  // (en los comandos del fotograma: los graba el grabador)
    else vkCmdPipelineBarrier2(cb, &di);        // (en uno propio del grabador)
}

// todo lo del contexto fuera (con la cola parada: rec_drain + vkDeviceWaitIdle antes). La biblioteca envía a la cola al
// iniciarse, al crear un contexto, al borrarlo y al terminar (drainQueueBounded: un envío vacío con valla): siempre
// con el cerrojo de la cola, que usan también otros hilos.
static void ctx_free(void) {
    if (L.ctx >= 0) {
        vk_queue_lock();
        L.destroy(L.ctx);
        vk_queue_unlock();
    }
    L.ctx = -1;
    for (int i = 0; i < 2 + LS_MAX_GEN; i++)
        if (L.img[i]) vmaDestroyImage(g_vk.vma, L.img[i], L.mem[i]), L.img[i] = VK_NULL_HANDLE;
    for (int s = 0; s < LS_SLOTS; s++)
        for (int i = 0; i < LS_MAX_GEN; i++) {
            if (L.fence[s][i]) vkDestroyFence(g_vk.dev, L.fence[s][i], NULL), L.fence[s][i] = VK_NULL_HANDLE;
            if (L.acq[s][i]) vkDestroySemaphore(g_vk.dev, L.acq[s][i], NULL), L.acq[s][i] = VK_NULL_HANDLE;
            if (L.gen[s][i]) vkDestroySemaphore(g_vk.dev, L.gen[s][i], NULL), L.gen[s][i] = VK_NULL_HANDLE;
            L.cb[s][i] = VK_NULL_HANDLE;
        }
    if (L.qp) vkDestroyQueryPool(g_vk.dev, L.qp, NULL), L.qp = VK_NULL_HANDLE;
    memset(L.tcb, 0, sizeof L.tcb), memset(L.ts_used, 0, sizeof L.ts_used), L.gpu_ms = 0;
    if (L.pool) vkDestroyCommandPool(g_vk.dev, L.pool, NULL), L.pool = VK_NULL_HANDLE;
    L.w = L.h = 0, L.n = 0;
}
static void quiesce(void) {
    rec_drain();
    vk_queue_lock();
    vkQueueWaitIdle(g_vk.queue);
    vk_queue_unlock();
}
// la biblioteca terminada y todo lo suyo fuera (otros ajustes; apagada, o con HDR o la de AMD: su memoria se suelta)
static void lib_stop(void) {
    if (!L.up_mult) return;
    quiesce();
    ctx_free();
    vk_queue_lock();
    L.finalize();
    vk_queue_unlock();
    L.up_mult = 0;
}
// lo que usa ahora el proceso en los montones locales de la tarjeta, según el controlador (0 sin VK_EXT_memory_budget):
// la diferencia de antes a después de arrancar es lo de LSFG, también lo suyo interno (que no pasa por VMA)
static uint64_t vram_now(void) {
    if (!g_vk.budget_ext) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
    vkGetPhysicalDeviceMemoryProperties2(g_vk.phys, &mp);
    uint64_t u = 0;
    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount; i++)
        if (mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) u += bp.heapUsage[i];
    return u;
}

// La biblioteca iniciada con los ajustes de ahora y un contexto del tamaño de la ventana (en el hilo que traduce)
static bool setup(VkCommandBuffer cb) {
    int m = g_settings.lsfg, flow = g_settings.lsfg_flow, perf = g_settings.lsfg_perf ? 1 : 0;
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    if (L.up_mult && (L.up_mult != m || L.up_flow != flow || L.up_perf != perf)) lib_stop();  // (otros ajustes: de cero)
    if (!L.up_mult) {
        static int orig_mip = -1;
        if (orig_mip < 0) orig_mip = getenv("LARECOMP_LSFG_ORIG_MIPMAPS") && atoi(getenv("LARECOMP_LSFG_ORIG_MIPMAPS")) != 0;
        LsfgSmrpgInit in = {.gipa = vkGetInstanceProcAddr, .instance = g_vk.inst, .phys = g_vk.phys, .device = g_vk.dev,
                            .queue_family = g_vk.qfam, .queue = g_vk.queue, .dll_path = L.dll, .performance = perf,
                            .flow_scale = (float)flow / 100.0f, .generations = m - 1, .hdr = 0, .orig_mipmaps = orig_mip,
                            .cache_path = "cache/lsfg_pipelines.bin"};  // (junto a la de sus tuberías: device.c)
        char err[400] = {0};
        quiesce();
        uint64_t v0 = vram_now(), t0 = SDL_GetTicksNS();
        vk_queue_lock();
        int ir = L.init(&in, err, sizeof err);
        if (ir != 0) L.finalize();
        vk_queue_unlock();
        L.init_ms = (SDL_GetTicksNS() - t0) / 1e6;  // (traducir sus sombreadores: sus tuberías se crean con el contexto)
        if (ir != 0) {
            status("no arrancó: %s", err);
            L.bad_mult = m, L.bad_flow = flow, L.bad_perf = perf;
            return false;
        }
        uint64_t v1 = vram_now();
        L.vram_lib = v1 > v0 ? v1 - v0 : 0;
        L.up_mult = m, L.up_flow = flow, L.up_perf = perf;
    }
    if (L.ctx >= 0 && L.w == w && L.h == h && L.n == m - 1) return true;
    quiesce();  // (crear el contexto puede enviar trabajo a la cola: el grabador, parado)
    ctx_free();
    int n = m - 1;
    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;  // (sin HDR, como lsfg-vk: sus sombreadores trabajan en coma flotante)
    for (int i = 0; i < 2 + n; i++) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &ci, &ai, &L.img[i], &L.mem[i], NULL) != VK_SUCCESS) {
            L.img[i] = VK_NULL_HANDLE;
            status("sin memoria para sus imágenes (%ux%u)", w, h);
            ctx_free();
            L.bad_mult = m, L.bad_flow = flow, L.bad_perf = perf;
            return false;
        }
    }
    char err[400] = {0};
    uint64_t v0 = vram_now(), t0 = SDL_GetTicksNS();
    vk_queue_lock();
    int ctx = L.create(L.img, L.img + 2, n, w, h, fmt, err, sizeof err);
    vk_queue_unlock();
    double ctx_ms = (SDL_GetTicksNS() - t0) / 1e6;
    uint64_t v1 = vram_now();
    L.vram_ctx = v1 > v0 ? v1 - v0 : 0, L.vram_measured = v0 > 0;
    if (ctx < 0) {
        status("no se pudo crear su contexto: %s", err);
        ctx_free();
        L.bad_mult = m, L.bad_flow = flow, L.bad_perf = perf;
        return false;
    }
    // (sus imágenes a GENERAL en los comandos del fotograma: solo ahora, con el contexto hecho. Si algo falla antes, se
    // destruyen al momento, y este fotograma, que se envía igual por el camino de siempre, no debe nombrarlas)
    for (int i = 0; i < 2 + n; i++)
        barrier(cb, true, L.img[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    L.ctx = ctx, L.w = w, L.h = h, L.n = n;
    // (el contexto nuevo empieza su cuenta en 0 y lee la imagen nueva en la entrada frameIdx % 2: la nuestra, igual, o
    // las intermedias irían al revés con x3 / x4)
    L.frames = 0;
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &L.pool));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int s = 0; s < LS_SLOTS; s++)
        for (int i = 0; i < n; i++) {
            VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = L.pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
            VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, &L.cb[s][i]));
            VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &L.fence[s][i]));
            VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &L.acq[s][i]));
            VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &L.gen[s][i]));
        }
    if (g_vk.ts_bits && g_vk.props.limits.timestampPeriod > 0) {  // (las marcas de tiempo: grabadas una vez por hueco)
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = 2 * LS_SLOTS};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &L.qp) != VK_SUCCESS) L.qp = VK_NULL_HANDLE;
        for (int s = 0; L.qp && s < LS_SLOTS; s++)
            for (int j = 0; j < 2; j++) {
                VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = L.pool,
                                                  .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
                VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, &L.tcb[s][j]));
                VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                               .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
                vkBeginCommandBuffer(L.tcb[s][j], &bi);
                if (!j) vkCmdResetQueryPool(L.tcb[s][j], L.qp, 2 * s, 2);
                vkCmdWriteTimestamp2(L.tcb[s][j], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, L.qp, 2 * s + j);
                vkEndCommandBuffer(L.tcb[s][j]);
            }
    }
    L.skip_next = true;  // (la primera: sin real anterior)
    status("activa: x%d, flujo %d %%, %s, %ux%u, con %s", m, flow, perf ? "modo rendimiento" : "calidad", w, h, L.dll);
    extern float vk_display_hz(void);
    LOG("[lsfg] tiempo: %.0f ms al iniciarse (traducir sus sombreadores), %.0f ms su contexto (sus tuberías, con su caché); pantalla a %.0f Hz\n",
        L.init_ms, ctx_ms, vk_display_hz());
    L.init_ms = 0;  // (ya dicho: con otra ventana, solo el contexto)
    if (L.vram_measured)
        LOG("[lsfg] memoria de vídeo: %.1f MB suyos (%.1f al iniciarse, %.1f del contexto) y %.1f de sus imágenes\n",
            (L.vram_lib + L.vram_ctx) / 1048576.0, L.vram_lib / 1048576.0, L.vram_ctx / 1048576.0,
            (2.0 + n) * w * h * 8 / 1048576.0);
    return true;
}

// (LARECOMP_LSFG_DUMP) imágenes de LSFG (RGBA16F) a PNG, en el grabador, esperando a la GPU
static float half_to_float(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
    if (e == 0) {
        if (!m) f = s;
        else {
            e = 127 - 15 + 1;
            while (!(m & 0x400)) m <<= 1, e--;
            f = s | (e << 23) | ((m & 0x3ff) << 13);
        }
    } else if (e == 31) f = s | 0x7f800000 | (m << 13);
    else f = s | ((e + 127 - 15) << 23) | (m << 13);
    float r;
    memcpy(&r, &f, 4);
    return r;
}
static void dump(uint64_t frame, int k) {
    // la anterior, las generadas (en orden: la primera, la más cercana a la anterior) y la real
    int m = L.n + 2;
    VkImage imgs[2 + LS_MAX_GEN];
    char names[2 + LS_MAX_GEN][16];
    imgs[0] = L.img[k ^ 1], snprintf(names[0], sizeof names[0], "0anterior");
    for (int i = 0; i < L.n; i++) imgs[1 + i] = L.img[2 + i], snprintf(names[1 + i], sizeof names[1 + i], "%dgenerada", 1 + i);
    imgs[m - 1] = L.img[k], snprintf(names[m - 1], sizeof names[m - 1], "%dreal", m - 1);
    VkDeviceSize sz = (VkDeviceSize)L.w * L.h * 8;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz * (VkDeviceSize)m, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VkBuffer buf;
    VmaAllocation mem;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info) != VK_SUCCESS) return;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = L.pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &cbi);
    for (int i = 0; i < m; i++) {
        barrier(cb, false, imgs[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy c = {.bufferOffset = sz * i, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {L.w, L.h, 1}};
        vkCmdCopyImageToBuffer(cb, imgs[i], VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    }
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &f));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    vk_queue_lock();
    vkQueueSubmit(g_vk.queue, 1, &si, f);
    vk_queue_unlock();
    vkWaitForFences(g_vk.dev, 1, &f, VK_TRUE, 5000000000ull);
    vmaInvalidateAllocation(g_vk.vma, mem, 0, VK_WHOLE_SIZE);
    uint8_t *px = malloc((size_t)L.w * L.h * 4);
    for (int i = 0; px && i < m; i++) {
        const uint16_t *src = (const uint16_t *)((const uint8_t *)info.pMappedData + sz * i);
        for (size_t p = 0; p < (size_t)L.w * L.h; p++)
            for (int c = 0; c < 4; c++) {
                float v = c == 3 ? 1.0f : half_to_float(src[p * 4 + c]);
                px[p * 4 + c] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f);
            }
        char path[128];
        snprintf(path, sizeof path, "lsfg_%llu_%s.png", (unsigned long long)frame, names[i]);
        vk_save_png(path, px, L.w, L.h);
    }
    free(px);
    vkDestroyFence(g_vk.dev, f, NULL);
    vkFreeCommandBuffers(g_vk.dev, L.pool, 1, &cb);
    vmaDestroyBuffer(g_vk.vma, buf, mem);
}

// ---- en el hilo grabador: generar, presentar las generadas y la real
typedef struct { VkSemaphore done; uint32_t img; int k; bool skip; uint64_t frame; } LsRun;
static void ls_run(const void *p) {
    const LsRun *a = p;
    VkSwapchainKHR sc = g_vk.swapchain;
    int slot = (int)(a->frame % LS_SLOTS);
    char err[400] = {0};
    bool gen = false;
    // (la real anterior, antes que las generadas de esta y antes de enviar el trabajo de LSFG: la valla que el
    // controlador da a una presentación —la que espera la siguiente— va en el orden de la cola, y detrás de LSFG
    // haría esperar a la generada hasta que acabara: medido en la Thor, 6-10 ms por real en vkQueuePresentKHR)
    pend_present(false);
    if (L.ctx >= 0 && !atomic_load(&L.broken)) {  // (roto: ni generar ni esperar sus vallas hasta que lo rehagan)
        if (L.qp && L.ts_used[slot]) {  // (la medida de este hueco de hace LS_SLOTS fotogramas)
            uint64_t t[4];  // (por consulta: su valor y si está)
            if (vkGetQueryPoolResults(g_vk.dev, L.qp, 2 * slot, 2, sizeof t, t, 2 * sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS && t[1] && t[3]) {
                uint64_t t0 = t[0], t1 = t[2];
                if (g_vk.ts_bits < 64) { uint64_t m = (1ull << g_vk.ts_bits) - 1; t0 &= m, t1 &= m; }
                double ms = t1 >= t0 ? (double)(t1 - t0) * g_vk.props.limits.timestampPeriod / 1e6 : -1;
                if (ms >= 0 && ms < 1000) L.gpu_ms = L.gpu_ms > 0 ? L.gpu_ms * 0.9 + ms * 0.1 : ms;
            }
        }
        VkSubmitInfo ts[2];
        for (int j = 0; j < 2; j++)
            ts[j] = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &L.tcb[slot][j]};
        // (prueba del camino de un fallo: LARECOMP_LSFG_FAIL_AT=<real>, sin llamar a su present, como una excepción a
        // mitad: su cuenta no avanza)
        static long fail_at = -2;
        if (fail_at == -2) fail_at = getenv("LARECOMP_LSFG_FAIL_AT") ? atol(getenv("LARECOMP_LSFG_FAIL_AT")) : -1;
        bool fake_fail = fail_at >= 0 && (long)a->frame == fail_at;
        if (fake_fail) fail_at = -1;  // (una vez: el contexto nuevo vuelve a contar desde 0)
        vk_queue_lock();
        bool timed = L.qp && vkQueueSubmit(g_vk.queue, 1, &ts[0], VK_NULL_HANDLE) == VK_SUCCESS;
        int r = fake_fail ? (snprintf(err, sizeof err, "fallo de prueba"), -1) : L.present(L.ctx, err, sizeof err);
        L.ts_used[slot] = timed && vkQueueSubmit(g_vk.queue, 1, &ts[1], VK_NULL_HANDLE) == VK_SUCCESS;
        vk_queue_unlock();
        if (r == 0) gen = true;
        else {
            // (su cuenta no avanzó, y si falló entre crear una valla y enviarla, la espera la próxima vez: otro contexto)
            if (L.st_fail++ < 5) LOG("[lsfg] la generación falló (%d): %s\n", r, err);
            atomic_store(&L.broken, true);
        }
    }
    static long dump_at = -2, dump_n = 1, dump_step = 1;  // (LARECOMP_LSFG_DUMP=<desde>[,<cuántos>[,<cada cuántos>]])
    if (dump_at == -2) {
        const char *e = getenv("LARECOMP_LSFG_DUMP");
        dump_at = -1;
        if (e) sscanf(e, "%ld,%ld,%ld", &dump_at, &dump_n, &dump_step);
        if (dump_step < 1) dump_step = 1;
    }
    long fr = (long)a->frame;
    if (gen && dump_at >= 0 && fr >= dump_at && fr < dump_at + dump_n * dump_step && (fr - dump_at) % dump_step == 0)
        dump(a->frame, a->k);
    bool shown = false, cut = false;  // (alguna generada presentada; la cadena falló: la real, ya, para que lo diga)
    for (int i = 0; gen && !a->skip && i < L.n; i++) {
        VkFence f = L.fence[slot][i];
        if (vkWaitForFences(g_vk.dev, 1, &f, VK_TRUE, 1000000000ull) != VK_SUCCESS) { L.st_skip++; continue; }
        uint32_t gi;
        VkResult r = vkAcquireNextImageKHR(g_vk.dev, sc, 100000000ull, L.acq[slot][i], VK_NULL_HANDLE, &gi);
        if (r == VK_TIMEOUT || r == VK_NOT_READY) { L.st_skip++; continue; }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { L.st_skip++; cut = true; break; }  // (OUT_OF_DATE: la real lo dirá)
        vkResetFences(g_vk.dev, 1, &f);
        VkCommandBuffer cb = L.cb[slot][i];
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bi);
        VkImage out = L.img[2 + i], dst = g_vk.sc_images[gi];
        barrier(cb, false, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(cb, false, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int)L.w, (int)L.h, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int)L.w, (int)L.h, 1}}};
        vkCmdBlitImage(cb, out, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
        barrier(cb, false, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
        barrier(cb, false, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_BLIT_BIT, 0,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &L.acq[slot][i],
                           .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1,
                           .pSignalSemaphores = &L.gen[slot][i]};
        vk_queue_lock();
        VkResult sr = vkQueueSubmit(g_vk.queue, 1, &si, f);
        vk_queue_unlock();
        if (sr != VK_SUCCESS) {
            // (su valla quedó reiniciada sin envío y la imagen gi adquirida sin presentar: otro contexto y otra cadena)
            LOG("[lsfg] envío de una generada: %d\n", sr);
            atomic_store(&L.broken, true);
            rec_request_recreate();
            cut = true;
            break;
        }
        rec_present_direct(L.gen[slot][i], sc, gi);
        L.st_gen++;
        shown = true;
    }
    if (shown && !cut) pend.on = true, pend.wait = a->done, pend.sc = sc, pend.img = a->img;
    else rec_present_direct(a->done, sc, a->img);  // (sin generadas delante: no espera a nada)
    extern uint64_t g_vk_frame_waits[4];  // (device.c: [2], el tiempo dentro de vkQueuePresentKHR, de todas)
    uint64_t pw = __atomic_load_n(&g_vk_frame_waits[2], __ATOMIC_RELAXED);
    if (++L.st_real == 1) L.pw_last = pw;
    if (L.st_real % 600 == 0) {
        LOG("[lsfg] %llu fotogramas reales, %llu generados presentados, %llu saltados, %llu fallos; GPU %.2f ms por real; "
            "presentar %.2f ms por real\n",
            (unsigned long long)L.st_real, (unsigned long long)L.st_gen, (unsigned long long)L.st_skip, (unsigned long long)L.st_fail,
            L.gpu_ms, (double)(pw - L.pw_last) / 600 / 1e6);
        L.pw_last = pw;
    }
}

// (menú: el panel de impacto) lo que cuesta ahora: ms de GPU por fotograma real (0: sin medir; se mide con el menú
// cerrado y se guarda mientras el contexto siga) y los bytes de memoria de vídeo (0: sin contexto): sus imágenes y, si
// *measured, también lo suyo interno. Solo si el contexto es el de los ajustes y la ventana de ahora: con el menú
// abierto no se rehace, y tras cambiar x2/x3/x4, el flujo o el modo, lo medido sería de lo anterior (el menú estima).
static bool ctx_current(void) {
    return L.ctx >= 0 && L.up_mult == g_settings.lsfg && L.up_flow == g_settings.lsfg_flow &&
           L.up_perf == (g_settings.lsfg_perf ? 1 : 0) && L.w == g_vk.sc_extent.width && L.h == g_vk.sc_extent.height;
}
double vk_lsfg_gpu_ms(void) { return ctx_current() ? L.gpu_ms : 0; }
uint64_t vk_lsfg_vram(int *measured) {
    bool cur = ctx_current();
    *measured = cur && L.vram_measured;
    if (!cur) return 0;
    return (uint64_t)(2 + L.n) * L.w * L.h * 8 + (L.vram_measured ? L.vram_lib + L.vram_ctx : 0);
}

// (hle/battle_rate.c) En combate el juego tiene que ir a 60 reales: su lógica da un paso de 1/60 s por imagen (el vsync
// late a 60 Hz mientras dura), y sus animaciones y su sonido van por tiempo: más despacio, los golpes a tiempo se
// desfasan. Con la generación puede no llegar: por la pantalla (en FIFO cada imagen es un refresco: con x3 / x4 a 120 Hz,
// o x2 a 60 Hz, solo caben 40 / 30 reales, la mitad de deprisa con x4) o por la GPU (en la Thor a 1080p con x2, 39). Así
// que, en combate, la pantalla se mira en cada imagen y las reales se cuentan por segundos: dos seguidos por debajo de
// 56, y hasta que acaba el combate, solo las reales (como con el menú abierto). Se llama con la generación en marcha (el
// resto: battle_reset).
static void battle_reset(void) { L.bt_t0 = L.bt_last = 0, L.bt_n = L.bt_low = 0; }
static bool battle_slow(void) {
    extern bool rt_battle_vsync_60(void);
    extern float vk_display_hz(void);
    if (!rt_battle_vsync_60()) {
        if (L.bt_slow) LOG("[lsfg] combate acabado: generando otra vez\n");
        L.bt_slow = false;
        battle_reset();
        return false;
    }
    if (L.bt_slow) return true;
    float hz = vk_display_hz();
    if (hz / (float)g_settings.lsfg < 59.0f) {
        LOG("[lsfg] combate: sin generar mientras dure (con x%d a %.0f Hz caben %.0f reales; necesita 60)\n", g_settings.lsfg,
            hz, hz / g_settings.lsfg);
        return L.bt_slow = true;
    }
    uint64_t now = SDL_GetTicksNS();
    if (!L.bt_t0 || now - L.bt_last > 100000000ull) L.bt_t0 = now, L.bt_n = 0;  // (otra vez desde aquí tras un hueco)
    L.bt_last = now;
    if (now - L.bt_t0 >= 1000000000ull) {
        double fps = L.bt_n * 1e9 / (double)(now - L.bt_t0);
        L.bt_low = fps < 56 ? L.bt_low + 1 : 0;
        L.bt_t0 = now, L.bt_n = 0;
        if (L.bt_low >= 2) {
            LOG("[lsfg] combate: sin generar mientras dure (con la generación iba a %.0f reales; necesita 60)\n", fps);
            return L.bt_slow = true;
        }
    }
    L.bt_n++;
    return false;
}

// (device.c vk_frame_end) la ventana del fotograma real, img, ya dibujada (su renderizado cerrado, en
// COLOR_ATTACHMENT_OPTIMAL): si la generación está en marcha, la copia a la entrada de LSFG, la deja en PRESENT_SRC, envía
// el fotograma (acquired -> done) y encola la generación y las presentaciones: true. false: el camino de siempre.
bool vk_lsfg_frame_end(VkCommandBuffer cb, uint32_t img, VkSemaphore acquired, VkSemaphore done) {
    bool want = vk_lsfg_wanted();
    if (!want && L.up_mult) lib_stop();  // (apagada, o con HDR o la de AMD: su memoria se suelta ya)
    // (cualquier fotograma que no le llegue: el siguiente, sin intermedias, que la anterior que tiene ya no es la de
    // justo antes; también con el menú abierto, y en un combate en el que con la generación no llegaría a 60 reales)
    bool on = want && g_vk.sc_lsfg && !g_vk.hdr_active && !menu_is_open();
    if (!on) battle_reset();
    if (!on || battle_slow()) { L.skip_next = true; return false; }
    if (atomic_load(&L.broken)) {  // (falló en el grabador: otro contexto; a la cuarta seguida, apagada con estos ajustes)
        quiesce();
        ctx_free();
        atomic_store(&L.broken, false);
        LOG("[lsfg] contexto rehecho tras un fallo (%d seguidos)\n", L.rebuilds + 1);
        if (++L.rebuilds > 3) {
            status("falló varias veces seguidas (mira lasrecomp.log): apagada hasta cambiar sus ajustes");
            L.bad_mult = g_settings.lsfg, L.bad_flow = g_settings.lsfg_flow, L.bad_perf = g_settings.lsfg_perf ? 1 : 0;
            L.rebuilds = 0;
            lib_stop();
            L.skip_next = true;
            return false;
        }
    }
    if (!setup(cb)) { L.skip_next = true; return false; }
    if (L.frames == 600) L.rebuilds = 0;  // (600 reales seguidos bien: los fallos de antes ya no cuentan)
    // (un rato sin fotogramas reales —ventana minimizada, superficie perdida, carga larga—: lo mismo)
    uint64_t now = SDL_GetTicksNS();
    if (now - L.last_ns > 250000000ull) L.skip_next = true;
    L.last_ns = now;
    int k = (int)(L.frames & 1);
    VkImage sc_img = g_vk.sc_images[img];
    barrier(cb, true, sc_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cb, true, L.img[k], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int)L.w, (int)L.h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int)L.w, (int)L.h, 1}}};
    rec_CmdBlitImage(cb, sc_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, L.img[k], VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    barrier(cb, true, sc_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    barrier(cb, true, L.img[k], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    extern void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra);
    vk_submit_extra(acquired, done, VK_NULL_HANDLE);
    LsRun a = {.done = done, .img = img, .k = k, .skip = L.skip_next, .frame = L.frames};
    rec_QueuePresentCall(ls_run, &a, sizeof a);
    L.frames++;
    L.skip_next = false;
    return true;
}

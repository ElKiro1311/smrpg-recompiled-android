// (SMRPG) Generación de fotogramas de AMD FSR (SDK 2.4, amd_fidelityfx_framegeneration_vk.dll junto al ejecutable):
// FSR Frame Generation 4.0.2 (red neuronal) en las Radeon RX 9000, 3.1.6 en las demás. Cada fotograma que presenta el
// juego, la swapchain de AMD presenta antes uno generado entre él y el anterior, en su propia cola y con sus propios
// hilos de ritmo: en pantalla, el doble de imágenes por segundo.
//
// Adaptado de una integración propia anterior del SDK 2 de AMD (la aportada a NX2navi, vk_fg.cpp). Cómo encaja aquí:
// - device.c crea dos colas más de la familia gráfica (vk_fg_queue_count, vk_fg_queues) y, mientras se quiere la
//   generación (el ajuste, con AMD FSR como antialiasing), hace la swapchain con vk_fg_create_swapchain y adquiere con
//   vk_fg_acquire. La cola del juego es del hilo grabador (record.c): presenta con vk_fg_present, y AMD entrega en ella
//   su trabajo (la generación) dentro de esa misma llamada; submit_game lo hace con el cerrojo de la cola por si acaso.
// - El orden que exige FG 4 (configurar, preparar, generar) va en el del hilo grabador: exec.c, tras FSR, encola
//   vk_fg_prepare_record (configura el fotograma y graba la preparación con la profundidad y los vectores de FSR y la
//   cámara); antes de entregar la ventana, vk_fg_frame_end_record configura sin generación los fotogramas que no
//   llegaron a FSR (pantallas sin la pasada de movimiento); la presentación genera.
// - El objeto VkSwapchainKHR es el de AMD: otro tamaño lo recrea en su sitio; se destruye solo tras sus contextos. Antes
//   de que el contexto de generación se vaya o la swapchain se recree, la generación se apaga en ella (el orden de AMD).
//   Una recreación en el sitio conserva sus cuentas de imágenes adquiridas y presentadas; si no cuadran (una adquirida
//   que no se presentó), otro formato o una recreación fallida, se rehace todo.
// - La interfaz del juego va dentro de la imagen y se interpola con ella (en SMRPG casi no se mueve); el menú del
//   runtime pausa la generación.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include <atomic>
#include <chrono>

#include <vulkan/vulkan.h>

#include "../../../third_party/amd-fsr/api/include/ffx_api.h"
#include "../../../third_party/amd-fsr/api/include/vk/ffx_api_vk.h"
#include "../../../third_party/amd-fsr/framegeneration/include/ffx_framegeneration.h"
#include "../../../third_party/amd-fsr/framegeneration/include/vk/ffx_api_framegeneration_vk.h"
#include "vk_fg.h"

extern "C" void vk_queue_lock(void);    // (record.c) la cola del juego
extern "C" void vk_queue_unlock(void);
extern "C" void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);  // (device.c)

static void flog(const char *fmt, ...) {  // (una sola escritura por línea, como rt_log)
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    fwrite(b, 1, (size_t)n, stderr);
}

namespace {

struct Shot {  // (pruebas, LARECOMP_FG_PSHOT) una imagen presentada, copiada a un búfer visible desde la CPU
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *ptr = nullptr;
    uint32_t w = 0, h = 0;
    bool bgr = false;
};

struct {
    VkQueue game = VK_NULL_HANDLE, present = VK_NULL_HANDLE, acquire = VK_NULL_HANDLE;
    uint32_t family = 0;
    ffxContext sc = nullptr;  // el contexto de la swapchain
    ffxContext fg = nullptr;  // el de la generación
    ffxQueryDescSwapchainReplacementFunctionsVK fn = {};
    // el objeto swapchain de AMD (cur), el que device.c soltó (pending: la creación siguiente lo recrea en su sitio o
    // vk_fg_release lo destruye), el formato y el espacio de color con que se hizo, su tamaño
    VkSwapchainKHR cur = VK_NULL_HANDLE, pending = VK_NULL_HANDLE;
    VkFormat sc_fmt = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR sc_cs = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D sc_ext = {0, 0};
    // el contexto de generación: para qué pantalla, formato, tamaño de escena máximo, versión y profundidad se hizo
    VkExtent2D fg_ext = {0, 0}, fg_max = {0, 0};
    VkFormat fg_fmt = VK_FORMAT_UNDEFINED;
    int fg_version = -1;
    bool fg_inverted = false, fg4 = false;
    std::atomic<int> rebuild{0};  // (grabador → device.c) el contexto de generación no sirve: recrear la swapchain
    // imágenes adquiridas (hilo traductor) y presentadas (grabador) desde que se hizo el objeto: iguales cuando ninguna
    // espera su presentación (se comparan con el grabador quieto)
    std::atomic<uint64_t> acquired{0}, presented{0};
    // (grabador) el número del último fotograma configurado y en qué presentación; presentaciones hechas
    uint64_t frame = 0, configured_at = ~0ull, presents = 0;
    std::atomic<int> on{0};
    int failed = 0;
    FgCamera cam = {};  // la última cámara buena (un fotograma sin la suya usa esta)
    uint64_t generated = 0, prepared = 0, paused = 0, no_inputs = 0, no_camera = 0, hudless = 0;
    std::chrono::steady_clock::time_point last_prepare;
    char status[200] = "";
} F;

const FsrApi *api(void) { return vk_fsr_api(); }
// (SMRPG) la memoria de vídeo de la generación de fotogramas (su contexto y su swapchain, según AMD; se mide al crearlos):
// para el medidor del menú, runtime/ui/perf_meter.c
static uint64_t g_fg_vram_ctx, g_fg_vram_sc;
extern "C" uint64_t vk_fg_vram(void) { return g_fg_vram_ctx + g_fg_vram_sc; }

ffxReturnCode_t api_create(ffxContext *c, ffxCreateContextDescHeader *d) {
    return ((PfnFfxCreateContext)api()->create)(c, d, nullptr);
}
void api_destroy(ffxContext *c) {
    if (c && *c) ((PfnFfxDestroyContext)api()->destroy)(c, nullptr), *c = nullptr;
}
ffxReturnCode_t api_configure(ffxContext *c, ffxConfigureDescHeader *d) { return ((PfnFfxConfigure)api()->configure)(c, d); }
ffxReturnCode_t api_query(ffxContext *c, ffxQueryDescHeader *d) { return ((PfnFfxQuery)api()->query)(c, d); }
ffxReturnCode_t api_dispatch(ffxContext *c, ffxDispatchDescHeader *d) { return ((PfnFfxDispatch)api()->dispatch)(c, d); }

const char *provider(ffxContext *c) {
    ffxQueryGetProviderVersion v = {};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    return c && *c && api_query(c, &v.header) == FFX_API_RETURN_OK && v.versionName ? v.versionName : "?";
}

// La cola del juego es del hilo grabador; AMD entrega en ella desde la presentación, que corre en ese hilo. El cerrojo,
// por si alguna versión lo hiciera desde otro (record.c también lo toma en las suyas).
VkResult submit_game(uint32_t n, const VkSubmitInfo *si, VkFence fence) {
    vk_queue_lock();
    VkResult r = vkQueueSubmit(F.game, n, si, fence);
    vk_queue_unlock();
    return r;
}

VkImageLayout layout_of(uint32_t state) {
    if (state & FFX_API_RESOURCE_STATE_PRESENT) return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (state & FFX_API_RESOURCE_STATE_RENDER_TARGET) return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (state == FFX_API_RESOURCE_STATE_COPY_SRC) return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (state == FFX_API_RESOURCE_STATE_COPY_DEST) return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (state & (FFX_API_RESOURCE_STATE_UNORDERED_ACCESS | FFX_API_RESOURCE_STATE_COMMON)) return VK_IMAGE_LAYOUT_GENERAL;
    if (state & FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ) return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return VK_IMAGE_LAYOUT_GENERAL;
}

void layout_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// (pruebas) copia una imagen de AMD (en el estado que dice) a un búfer visible desde la CPU, dejándola como estaba
bool shot_copy(VkCommandBuffer cb, const FfxApiResource &r, Shot &s) {
    VkImage img = (VkImage)r.resource;
    uint32_t w = r.description.width, h = r.description.height;
    uint32_t f = r.description.format;
    if (!img || !w || !h) return false;
    if (f != FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM && f != FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM &&
        f != FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB && f != FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB &&
        f != FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM)
        return false;
    VkDevice dev = api()->dev;
    if (!s.buf) {
        VkBufferCreateInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = (VkDeviceSize)w * h * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(dev, &bi, nullptr, &s.buf) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, s.buf, &mr);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(api()->phys, &mp);
        uint32_t type = UINT32_MAX, want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
            if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) type = i;
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = type;
        if (type == UINT32_MAX || vkAllocateMemory(dev, &ai, nullptr, &s.mem) != VK_SUCCESS ||
            vkBindBufferMemory(dev, s.buf, s.mem, 0) != VK_SUCCESS || vkMapMemory(dev, s.mem, 0, VK_WHOLE_SIZE, 0, &s.ptr) != VK_SUCCESS)
            return false;
        s.w = w, s.h = h;
    }
    if (s.w != w || s.h != h) return false;
    s.bgr = f == FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM || f == FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
    VkImageLayout l = layout_of(r.state);
    layout_barrier(cb, img, l, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {};
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buf, 1, &rg);
    layout_barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l);
    return true;
}

void shot_save(const Shot &s, const char *path) {
    if (!s.ptr) return;
    size_t n = (size_t)s.w * s.h;
    uint8_t *px = (uint8_t *)malloc(n * 4);
    if (!px) return;
    memcpy(px, s.ptr, n * 4);
    for (size_t i = 0; i < n; i++) {
        if (s.bgr) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        px[i * 4 + 3] = 255;
    }
    vk_save_png(path, px, s.w, s.h);
    free(px);
}

// La llamada de AMD para generar (en la presentación, hilo grabador): graba la generación en la lista que da
ffxReturnCode_t generate(ffxDispatchDescFrameGeneration *d, void *user) {
    ffxContext *ctx = (ffxContext *)user;
    if (!ctx || !*ctx) {  // sin contexto (ya): nada generado, la swapchain no presenta una imagen vieja
        d->numGeneratedFrames = 0;
        return FFX_API_RETURN_ERROR;
    }
    F.generated++;
    ffxReturnCode_t r = api_dispatch(ctx, &d->header);
    if (r != FFX_API_RETURN_OK) {
        d->numGeneratedFrames = 0;
        return r;
    }
    return r;
}

// (pruebas, LARECOMP_FG_PSHOT=N) La composición de AMD hecha aquí, como la suya sin llamada (la imagen, real o generada,
// copiada a la de la swapchain real), y desde el fotograma N seis imágenes seguidas tal como se presentan
// (build/fg_pres_<i>_<real|generado>.png). Corre en el hilo de presentación de AMD, con su cola.
struct {
    long at = -1;
    int n = 0, logged = 0;
    bool gen[6] = {};
    uint64_t saved_at = 0;
    bool saved = false;
    Shot img[6];
} P;

ffxReturnCode_t present_cb(ffxCallbackDescFrameGenerationPresent *p, void *) {
    VkCommandBuffer cb = (VkCommandBuffer)p->commandList;
    const FfxApiResource &src = p->currentBackBuffer, &dst = p->outputSwapChainBuffer;
    VkImage si = (VkImage)src.resource, di = (VkImage)dst.resource;
    if (P.logged < 2) {
        P.logged++;
        flog("[fg] composición: imagen %ux%u (formato %u, estado %u) -> salida %ux%u (formato %u, estado %u), %s",
             src.description.width, src.description.height, src.description.format, src.state, dst.description.width,
             dst.description.height, dst.description.format, dst.state, p->isGeneratedFrame ? "generada" : "real");
    }
    if (!cb || !si || !di) return FFX_API_RETURN_ERROR;
    if (P.n < 6 && (long)F.presents + 1 >= P.at && shot_copy(cb, src, P.img[P.n])) {
        P.gen[P.n] = p->isGeneratedFrame;
        if (++P.n == 6) P.saved_at = F.presents + 8;
    }
    VkImageLayout sl = layout_of(src.state), dl = layout_of(dst.state);
    layout_barrier(cb, si, sl, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    layout_barrier(cb, di, dl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy rg = {};
    rg.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.extent = {src.description.width < dst.description.width ? src.description.width : dst.description.width,
                 src.description.height < dst.description.height ? src.description.height : dst.description.height, 1};
    vkCmdCopyImage(cb, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, di, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    layout_barrier(cb, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sl);
    layout_barrier(cb, di, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dl);
    return FFX_API_RETURN_OK;
}

// La generación apagada en la swapchain de AMD swc (sin llamadas ni imágenes): su presentador espera las
// presentaciones en curso y para; la próxima configuración con generación la vuelve a poner en marcha.
void fg_disable(VkSwapchainKHR swc) {
    F.on.store(0, std::memory_order_relaxed);
    if (!F.fg || !swc) return;
    ffxConfigureDescFrameGeneration cfg = {};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = (void *)swc;
    cfg.frameGenerationEnabled = false;
    cfg.frameID = ++F.frame;
    api_configure(&F.fg, &cfg.header);
}

void destroy_fg(void) {
    if (!F.fg) return;
    fg_disable(F.cur);
    api_destroy(&F.fg);
    g_fg_vram_ctx = 0;
    F.fg_ext = F.fg_max = {0, 0};
    F.fg_version = -1;
}

// Todo abajo en el orden de AMD: la generación apagada, su contexto, el de la swapchain y el objeto swapchain de AMD
// (el que device.c soltó o el actual)
void teardown(void) {
    VkSwapchainKHR obj = F.pending ? F.pending : F.cur;
    if (api()) {
        fg_disable(obj);
        api_destroy(&F.fg);
        api_destroy(&F.sc);
        if (obj && F.fn.pOutDestroySwapchainFFXAPI) F.fn.pOutDestroySwapchainFFXAPI(api()->dev, obj, nullptr, nullptr);
    }
    F.fg = F.sc = nullptr;
    g_fg_vram_ctx = g_fg_vram_sc = 0;
    F.pending = F.cur = VK_NULL_HANDLE;
    F.fg_ext = F.fg_max = {0, 0};
    F.fg_version = -1;
    F.acquired = 0, F.presented = 0;
    F.rebuild = 0;
}

void fail(const char *what, unsigned r) {
    flog("[fg] %s falló (%u): sin generación de fotogramas hasta reiniciar", what, r);
    snprintf(F.status, sizeof F.status, "error (%s: %u); se apaga hasta reiniciar el juego", what, r);
    F.failed = 1;
    // apagada en la swapchain de AMD al momento; las presentaciones siguen por ella (vk_fg_active) hasta que device.c
    // la cambia por una normal (vk_fg_usable es false tras un fallo)
    fg_disable(F.cur);
}

bool exe_file(const char *name) {
    char p[MAX_PATH + 64];
    DWORD k = GetModuleFileNameA(NULL, p, MAX_PATH);
    if (!k || k >= MAX_PATH) return false;
    while (k && p[k - 1] != '\\' && p[k - 1] != '/') k--;
    snprintf(p + k, sizeof p - k, "%s", name);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

FfxApiResource res(const FsrImage *im) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = im->format;
    ci.extent = {im->width, im->height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = im->usage;
    return ffxApiGetResourceVK((void *)im->image, ffxApiGetImageResourceDescriptionVK(im->image, ci, 0),
                               FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

bool hdr_format(VkFormat f) {
    return f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

void amd_message(uint32_t type, const wchar_t *m) {  // (los avisos del SDK, al registro; los 200 primeros)
    static int n;
    if (n++ >= 200) return;
    char b[600];
    size_t i = 0;
    for (; m && m[i] && i < sizeof b - 1; i++) b[i] = m[i] < 128 ? (char)m[i] : '?';
    b[i] = 0;
    flog("[fg] AMD %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "aviso", b);
}

// El contexto de generación para la pantalla de la swapchain y escenas de hasta max_render
bool create_fg(VkExtent2D max_render, int version, bool inverted) {
    static bool listed;
    if (!listed) {  // las versiones que ofrece el SDK en esta tarjeta (al registro, una vez)
        listed = true;
        VkDeviceContext dc = {api()->dev, api()->phys, api()->gdpa};
        uint64_t count = 16, ids[16] = {};
        const char *names[16] = {};
        ffxQueryDescGetVersions q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        q.device = &dc;
        q.outputCount = &count;
        q.versionIds = ids;
        q.versionNames = names;
        char list[300] = "";
        if (api_query(nullptr, &q.header) == FFX_API_RETURN_OK)
            for (uint64_t i = 0; i < count && i < 16; i++) {
                if (i) strncat(list, ", ", sizeof list - strlen(list) - 1);
                strncat(list, names[i] ? names[i] : "?", sizeof list - strlen(list) - 1);
            }
        flog("[fg] versiones de la generación de fotogramas en esta tarjeta: %s", list[0] ? list : "(ninguna)");
    }
    ffxCreateBackendVKDesc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext.vkDevice = api()->dev;
    be.deviceContext.vkPhysicalDevice = api()->phys;
    be.deviceContext.vkDeviceProcAddr = api()->gdpa;
    ffxCreateContextDescFrameGenerationVersion ver = {};  // (obligatorio desde el SDK 2.1)
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.header.pNext = &be.header;
    ver.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    d.header.pNext = &ver.header;
    ffxOverrideVersion ov = {};
    if (vk_fsr_pick_version(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, version, &ov))
        ov.header.pNext = d.header.pNext, d.header.pNext = &ov.header;
    d.flags = (inverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0) |
              (hdr_format(F.sc_fmt) ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0);
    static int dbg = -1;  // (pruebas) LARECOMP_FG_DEBUG=2: el comprobador de entradas de AMD, con sus avisos al registro
    if (dbg < 0) dbg = getenv("LARECOMP_FG_DEBUG") ? atoi(getenv("LARECOMP_FG_DEBUG")) : 0;
    if (dbg >= 2) d.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    d.displaySize = {F.sc_ext.width, F.sc_ext.height};
    d.maxRenderSize = {max_render.width, max_render.height};
    d.backBufferFormat = ffxApiGetSurfaceFormatVK(F.sc_fmt);
    ffxReturnCode_t r = api_create(&F.fg, &d.header);
    if (r != FFX_API_RETURN_OK) {
        F.fg = nullptr;
        fail("el contexto de generación", r);
        return false;
    }
    F.fg_ext = F.sc_ext, F.fg_fmt = F.sc_fmt, F.fg_max = max_render, F.fg_version = version, F.fg_inverted = inverted;
    {   // (SMRPG) su memoria de vídeo, según AMD, para el medidor del menú (runtime/ui/perf_meter.c)
        FfxApiEffectMemoryUsage mu = {};
        ffxQueryDescFrameGenerationGetGPUMemoryUsage q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
        q.gpuMemoryUsageFrameGeneration = &mu;
        g_fg_vram_ctx = api_query(&F.fg, &q.header) == FFX_API_RETURN_OK ? mu.totalUsageInBytes : 0;
    }
    if (dbg >= 2) {
        ffxConfigureDescGlobalDebug gd = {};
        gd.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
        gd.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
        gd.fpMessage = amd_message;
        gd.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
        ffxReturnCode_t gr = api_configure(&F.fg, &gd.header);
        flog("[fg] comprobador de entradas de AMD: %s (%u)", gr == FFX_API_RETURN_OK ? "activo" : "no se pudo activar", (unsigned)gr);
    }
    const char *pv = provider(&F.fg);
    F.fg4 = pv[0] == '4';
    snprintf(F.status, sizeof F.status, "activa (FSR Frame Generation %s)", pv);
    flog("[fg] contexto de generación %s para %ux%u%s, escena hasta %ux%u, profundidad %s", pv, F.sc_ext.width,
         F.sc_ext.height, hdr_format(F.sc_fmt) ? " (HDR)" : "", max_render.width, max_render.height,
         inverted ? "invertida" : "normal");
    return true;
}

}  // namespace

extern "C" bool vk_fg_dll(void) {
    static int have = -1;
    if (have < 0) {
        have = vk_fsr_available() && exe_file("amd_fidelityfx_framegeneration_vk.dll");
        if (!have) snprintf(F.status, sizeof F.status, "no disponible (falta amd_fidelityfx_framegeneration_vk.dll junto al programa)");
    }
    return have;
}

// Con la DLL, siempre las tres colas: el ajuste puede encenderse durante la partida y una cola quieta no cuesta nada
extern "C" uint32_t vk_fg_queue_count(void) { return vk_fg_dll() ? 3 : 1; }

extern "C" void vk_fg_queues(VkQueue game, uint32_t family, VkQueue present, VkQueue acquire) {
    F.game = game, F.family = family, F.present = present, F.acquire = acquire;
    if (vk_fg_dll() && !present)
        snprintf(F.status, sizeof F.status, "no disponible (la tarjeta no da una segunda cola gráfica)");
}

extern "C" bool vk_fg_usable(void) { return !F.failed && F.present && F.acquire && vk_fg_dll() && api(); }

// La swapchain de AMD está puesta: el VkSwapchainKHR de la ventana es su objeto, y toda llamada de swapchain tiene que
// ir por sus funciones (también tras un fallo, hasta que device.c la cambie: las de Vulkan nunca deben verlo)
extern "C" bool vk_fg_active(void) { return F.sc != nullptr; }

extern "C" bool vk_fg_generating(void) { return vk_fg_active() && !F.failed && F.on.load(std::memory_order_relaxed); }

extern "C" bool vk_fg_rebuild_wanted(void) { return vk_fg_active() && F.rebuild.load(std::memory_order_relaxed) != 0; }

extern "C" VkResult vk_fg_create_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc) {
    if (F.sc) {  // la misma swapchain de AMD, hecha de nuevo para el tamaño o el modo nuevos
        VkSwapchainKHR obj = F.pending ? F.pending : F.cur;
        F.pending = VK_NULL_HANDLE, F.cur = obj;
        bool same = ci->imageFormat == F.sc_fmt && ci->imageColorSpace == F.sc_cs;
        if (obj && same && F.acquired == F.presented && !F.failed) {
            fg_disable(obj);  // (el fotograma siguiente la vuelve a encender)
            // el contexto de generación es para la pantalla de antes, o no sirve (rebuild): fuera, con la GPU parada
            // (device.c recreate_swapchain esperó); el próximo fotograma con FSR hace otro
            if (F.fg && (F.rebuild || F.fg_ext.width != ci->imageExtent.width || F.fg_ext.height != ci->imageExtent.height))
                destroy_fg();
            F.rebuild = 0;
            VkSwapchainKHR out = obj;
            VkResult r = F.fn.pOutCreateSwapchainFFXAPI(api()->dev, ci, nullptr, &out, (void *)obj);
            if (r == VK_SUCCESS) {
                F.cur = out, *sc = out;
                F.sc_ext = ci->imageExtent;
                flog("[fg] swapchain de AMD rehecha a %ux%u", ci->imageExtent.width, ci->imageExtent.height);
                return VK_SUCCESS;
            }
            flog("[fg] la swapchain de AMD no se pudo rehacer a %ux%u (%d): se hace de nuevo", ci->imageExtent.width,
                 ci->imageExtent.height, (int)r);
        } else if (obj && F.acquired != F.presented) {
            // sus cuentas quedarían una imagen desfasadas para siempre (cada imagen presentada un fotograma tarde)
            flog("[fg] una imagen adquirida de la swapchain de AMD no se presentó: se hace de nuevo");
        } else if (obj && !same) {
            flog("[fg] otro formato o espacio de color: la swapchain de AMD se hace de nuevo");
        }
        teardown();  // y una nueva abajo, como la primera vez
    }
    if (!vk_fg_usable()) return VK_ERROR_FEATURE_NOT_PRESENT;
    ffxCreateContextDescFrameGenerationSwapChainVK d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    d.physicalDevice = api()->phys;
    d.device = api()->dev;
    VkSwapchainKHR none = VK_NULL_HANDLE;
    d.swapchain = &none;
    d.createInfo = *ci;
    d.createInfo.oldSwapchain = VK_NULL_HANDLE;
    d.gameQueue = {F.game, F.family, submit_game};
    d.asyncComputeQueue = {VK_NULL_HANDLE, F.family, nullptr};
    d.presentQueue = {F.present, F.family, nullptr};
    d.imageAcquireQueue = {F.acquire, F.family, nullptr};
    ffxCreateContextDescFrameGenerationSwapChainVersionVK ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
    ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
    d.header.pNext = &ver.header;
    ffxReturnCode_t r = api_create(&F.sc, &d.header);
    if (r != FFX_API_RETURN_OK) {
        F.sc = nullptr;
        fail("la swapchain de AMD", r);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    {
        FfxApiEffectMemoryUsage mu = {};
        ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageVK q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_VK;
        q.gpuMemoryUsageFrameGenerationSwapchain = &mu;
        g_fg_vram_sc = api_query(&F.sc, &q.header) == FFX_API_RETURN_OK ? mu.totalUsageInBytes : 0;
    }
    F.fn = {};
    F.fn.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    if (api_query(&F.sc, &F.fn.header) != FFX_API_RETURN_OK || !F.fn.pOutQueuePresentKHR || !F.fn.pOutAcquireNextImageKHR ||
        !F.fn.pOutGetSwapchainImagesKHR || !F.fn.pOutCreateSwapchainFFXAPI) {
        // con el contexto se va la swapchain que hizo (aún no se dio a nadie); el objeto de AMD sobrevive a su contexto
        // y va por su propia función, si la consulta llegó a darla
        api_destroy(&F.sc);
        if (none && F.fn.pOutDestroySwapchainFFXAPI) F.fn.pOutDestroySwapchainFFXAPI(api()->dev, none, nullptr, nullptr);
        F.cur = VK_NULL_HANDLE;
        fail("las funciones de la swapchain de AMD", 0);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *sc = none;
    F.cur = none, F.pending = VK_NULL_HANDLE, F.acquired = 0, F.presented = 0;
    F.sc_fmt = ci->imageFormat, F.sc_cs = ci->imageColorSpace, F.sc_ext = ci->imageExtent;
    F.rebuild = 0;
    if (!F.fg) snprintf(F.status, sizeof F.status, "lista (empieza con el primer fotograma con FSR)");
    const char *e = getenv("LARECOMP_FG_PSHOT");
    P.at = e ? atol(e) : -1;
    flog("[fg] swapchain de generación de fotogramas de AMD (%s), %ux%u, %u imágenes", provider(&F.sc),
         ci->imageExtent.width, ci->imageExtent.height, ci->minImageCount);
    return VK_SUCCESS;
}

extern "C" void vk_fg_destroy_swapchain(VkSwapchainKHR sc) {
    if (F.sc && sc) F.pending = sc;  // (se rehace en su sitio en vk_fg_create_swapchain, o se destruye en vk_fg_release)
}

// La de AMD no corrige la cuenta cuando recibe el arreglo (devolvía 8 para 3 imágenes: vistas de imágenes nulas, y el
// driver caía): primero cuántas hay, luego esas
extern "C" VkResult vk_fg_get_images(VkSwapchainKHR sc, uint32_t *n, VkImage *img) {
    uint32_t have = 0, cap = *n;
    VkResult r = F.fn.pOutGetSwapchainImagesKHR(api()->dev, sc, &have, nullptr);
    if (r != VK_SUCCESS) return *n = 0, r;
    if (have > cap) have = cap;
    for (uint32_t i = 0; i < cap; i++) img[i] = VK_NULL_HANDLE;
    r = F.fn.pOutGetSwapchainImagesKHR(api()->dev, sc, &have, img);
    uint32_t k = 0;
    while (k < have && img[k]) k++;
    *n = k;
    return r == VK_INCOMPLETE ? VK_SUCCESS : r;
}

// Las dos se cuentan devuelvan lo que devuelvan (vk_fg_create_swapchain compara las cuentas): la swapchain de AMD
// cuenta una adquisición aunque falle su propia entrega, y contar una de más solo cuesta rehacer el objeto.
extern "C" VkResult vk_fg_acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx) {
    F.acquired++;
    return F.fn.pOutAcquireNextImageKHR(api()->dev, sc, timeout, sem, VK_NULL_HANDLE, idx);
}

extern "C" VkResult vk_fg_present(VkQueue queue, const VkPresentInfoKHR *pi) {
    F.presented++;
    VkResult r = F.fn.pOutQueuePresentKHR(queue, pi);
    F.presents++;
    if (P.n == 6 && !P.saved && F.presents >= P.saved_at) {  // (pruebas) lo presentado, ya copiado
        P.saved = true;
        for (int i = 0; i < 6; i++) {
            char path[64];
            snprintf(path, sizeof path, "build/fg_pres_%d_%s.png", i, P.gen[i] ? "generado" : "real");
            shot_save(P.img[i], path);
        }
    }
    if (F.presents == 120 || F.presents % 3600 == 0) {
        uint64_t shown = F.fn.pOutGetLastPresentCountFFXAPI && F.cur ? F.fn.pOutGetLastPresentCountFFXAPI(F.cur) : 0;
        flog("[fg] %llu fotogramas del juego, %llu generados (%llu con el menú del runtime aparte), %llu presentados en total; "
             "sin generar: %llu en pausa (menú), %llu sin FSR, %llu sin cámara", (unsigned long long)F.presents,
             (unsigned long long)F.generated, (unsigned long long)F.hudless, (unsigned long long)shown,
             (unsigned long long)F.paused, (unsigned long long)F.no_inputs, (unsigned long long)F.no_camera);
    }
    return r;
}

static void configure(bool on, const FgPrepare *p) {
    F.on.store(on ? 1 : 0, std::memory_order_relaxed);
    F.frame++;
    ffxConfigureDescFrameGeneration cfg = {};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = (void *)F.cur;
    cfg.frameGenerationCallback = generate;
    cfg.frameGenerationCallbackUserContext = &F.fg;
    if (P.at > 0) cfg.presentCallback = present_cb;  // (pruebas: la composición, aquí)
    cfg.frameGenerationEnabled = on;
    cfg.allowAsyncWorkloads = false;
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("LARECOMP_FG_DEBUG") ? atoi(getenv("LARECOMP_FG_DEBUG")) : 0;
    if (dbg == 1) cfg.flags = FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS;
    // solo dentro de la imagen del juego: con bandas alrededor, generar en toda la ventana deformaba sus bordes
    if (p && p->pic[2] > 0 && p->pic[3] > 0 && (uint32_t)(p->pic[0] + p->pic[2]) <= F.sc_ext.width &&
        (uint32_t)(p->pic[1] + p->pic[3]) <= F.sc_ext.height)
        cfg.generationRect = {p->pic[0], p->pic[1], p->pic[2], p->pic[3]};
    else
        cfg.generationRect = {0, 0, (int32_t)F.sc_ext.width, (int32_t)F.sc_ext.height};
    // la ventana sin el menú del runtime: AMD no interpola lo que el menú dibuja encima (el contador de FPS)
    if (on && p && p->hudless.image && p->hudless.width == F.sc_ext.width && p->hudless.height == F.sc_ext.height &&
        p->hudless.format == F.sc_fmt) {
        cfg.HUDLessColor = res(&p->hudless);
        F.hudless++;
    }
    cfg.frameID = F.frame;
    F.configured_at = F.presents;
    ffxReturnCode_t r = api_configure(&F.fg, &cfg.header);
    if (r != FFX_API_RETURN_OK) fail("la configuración de la generación", r);
}

extern "C" void vk_fg_prepare_record(VkCommandBuffer cb, const void *data) {
    const FgPrepare *p = (const FgPrepare *)data;
    if (!vk_fg_active() || F.failed || !F.cur) return;
    bool have = p->depth.image && p->mv.image && p->render_w && p->render_h && p->render_w <= p->depth.width &&
                p->render_h <= p->depth.height;
    // el contexto se hizo para otra versión, otra profundidad o escenas más pequeñas: device.c rehace la swapchain en el
    // fotograma siguiente (con la GPU parada) y el contexto con ella; hasta entonces, sin generación
    if (F.fg && (F.fg_version != p->version || F.fg_inverted != p->depth_inverted || p->depth.width > F.fg_max.width ||
                 p->depth.height > F.fg_max.height)) {
        F.rebuild = 1;
        have = false;
    }
    if (!F.fg && !F.rebuild && have) {
        VkExtent2D mr = {F.sc_ext.width > p->depth.width ? F.sc_ext.width : p->depth.width,
                         F.sc_ext.height > p->depth.height ? F.sc_ext.height : p->depth.height};
        if (!create_fg(mr, p->version, p->depth_inverted)) return;
    }
    if (!F.fg) {  // (sin contexto todavía: este fotograma, sin generación)
        F.no_inputs++;
        return;
    }
    // la cámara: FG 4 la necesita (sin ella, la última buena; antes de la primera, sin generación); 3.1 la aprovecha
    FgCamera cam = p->cam;
    if (cam.ok) F.cam = cam;
    else if (F.cam.ok) cam = F.cam, F.no_camera++;
    else if (F.fg4) have = false, F.no_camera++;
    if (!cam.ok) {  // (3.1 sin cámara todavía: ejes unitarios, nunca vectores nulos)
        cam = FgCamera{};
        cam.right[0] = cam.up[1] = cam.fwd[2] = 1;
    }
    bool on = have && !p->paused;
    if (!have) F.no_inputs++;
    else if (!on) F.paused++;
    configure(on, p);
    if (F.failed || !on) return;
    auto now = std::chrono::steady_clock::now();
    double dt = F.prepared ? std::chrono::duration<double, std::milli>(now - F.last_prepare).count() : 16.67;
    F.last_prepare = now;
    F.prepared++;
    ffxDispatchDescFrameGenerationPrepareV2 d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    d.frameID = F.frame;
    d.commandList = cb;
    d.renderSize = {p->render_w, p->render_h};
    d.jitterOffset = {p->jitter_x, p->jitter_y};
    d.motionVectorScale = {p->mv_scale_x, p->mv_scale_y};
    {   // (pruebas) LARECOMP_FG_MV="x,y": multiplicadores de la escala de los vectores, solo para la generación
        static float mx = 0, my = 0;
        static int read;
        if (!read++) {
            mx = my = 1;
            if (getenv("LARECOMP_FG_MV")) sscanf(getenv("LARECOMP_FG_MV"), "%f,%f", &mx, &my);
        }
        d.motionVectorScale.x *= mx, d.motionVectorScale.y *= my;
    }
    d.frameTimeDelta = dt > 0 && dt < 100 ? (float)dt : 16.67f;
    d.reset = p->reset;
    d.cameraNear = cam.ok && cam.znear > 0 ? cam.znear : 0.3f;
    d.cameraFar = cam.ok && cam.zfar > d.cameraNear ? cam.zfar : 1000.0f;
    d.cameraFovAngleVertical = cam.ok && cam.fov_y > 0.01f && cam.fov_y < 3.0f ? cam.fov_y : 0.6f;
    d.viewSpaceToMetersFactor = 1.0f;  // (Unity: metros)
    d.depth = res(&p->depth);
    d.motionVectors = res(&p->mv);
    for (int i = 0; i < 3; i++)
        d.cameraPosition[i] = cam.pos[i], d.cameraUp[i] = cam.up[i], d.cameraRight[i] = cam.right[i], d.cameraForward[i] = cam.fwd[i];
    full_barrier(cb);
    ffxReturnCode_t r = api_dispatch(&F.fg, &d.header);
    full_barrier(cb);
    if (r != FFX_API_RETURN_OK) fail("la preparación de la generación", r);
}

// Antes de entregar la ventana: un fotograma que no llegó a FSR (sin la pasada de movimiento) se configura sin
// generación, para que la swapchain no genere con lo de otro fotograma
extern "C" void vk_fg_frame_end_record(VkCommandBuffer cb, const void *unused) {
    (void)cb, (void)unused;
    if (!vk_fg_active() || F.failed || !F.cur || !F.fg) return;
    if (F.configured_at == F.presents) return;  // (configurado en su FSR)
    F.no_inputs++;
    configure(false, nullptr);
}

extern "C" void vk_fg_wait_presents(void) {
    if (!F.sc) return;
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK w = {};
    w.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    api_dispatch(&F.sc, &w.header);
}

extern "C" void vk_fg_release(void) {
    if (F.sc || F.fg) teardown();
    F.on.store(0, std::memory_order_relaxed);
    if (!api()) return;
    for (Shot &s : P.img) {
        if (s.mem) vkFreeMemory(api()->dev, s.mem, nullptr);
        if (s.buf) vkDestroyBuffer(api()->dev, s.buf, nullptr);
        s = Shot();
    }
    F.on.store(0, std::memory_order_relaxed);
}

extern "C" const char *vk_fg_status(void) {
    if (!F.status[0]) vk_fg_dll();
    return F.status[0] ? F.status : "sin usar todavía";
}

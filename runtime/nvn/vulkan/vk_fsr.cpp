// (SMRPG) AMD FSR como antialiasing temporal («nativo»: a la misma resolución que se dibuja) con el SDK de AMD FSR 2.x.
// Sus DLL firmadas van junto al ejecutable: amd_fidelityfx_loader_vk.dll, que carga amd_fidelityfx_upscaler_vk.dll y
// elige la versión que corre en la tarjeta (FSR 4, de red neuronal, en RDNA 4; FSR 3.1 en las demás). Las cabeceras
// (MIT) están en third_party/amd-fsr.
//
// Entradas (exec.c las identifica en cada fotograma): el color HDR de la escena antes del posproceso, su profundidad y
// los vectores de movimiento que dibuja la pasada MotionVectorRenderPass de URP, que el juego no pide y el runtime
// fuerza (tools/game_overrides.c, GetRenderPassInputs). La escena se dibuja con el desplazamiento subpíxel que da
// vk_fsr_jitter, distinto en cada fotograma (exec.c set_viewport). El resultado se escribe de vuelta en el color de la
// escena: lo que sigue (el posproceso de URP, la interfaz) no cambia.
//
// Todo lo que graba comandos corre en el hilo grabador (record.c rec_CmdCallback), en su sitio del orden; las imágenes
// del juego están siempre en GENERAL, que el backend del SDK toma como su estado de acceso desordenado.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include <vulkan/vulkan.h>

#include "../../../third_party/amd-fsr/api/include/ffx_api.h"
#include "../../../third_party/amd-fsr/upscalers/include/ffx_upscale.h"
#include "../../../third_party/amd-fsr/api/include/vk/ffx_api_vk.h"
#include "vk_fsr.h"

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

static struct {
    bool tried, ok;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxConfigure configure;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    VkPhysicalDevice phys;
    VkDevice dev;
    PFN_vkGetDeviceProcAddr gdpa;
    char status[160];
    FsrApi api;  // (vk_fsr_api: lo mismo para vk_fg.cpp)
} A;

static struct {
    ffxContext ctx;
    uint32_t w, h;
    bool inverted;
    int version;  // (la pedida al crearlo)
    VkImage out;
    VkDeviceMemory out_mem;
    bool out_ready;  // (en GENERAL)
    bool failed;     // (no se pudo crear a este tamaño: no se reintenta cada fotograma)
    uint64_t frames;
} C;

extern "C" void vk_queue_lock(void);  // (record.c)
extern "C" void vk_queue_unlock(void);
static VkQueue g_queue;
extern "C" void vk_fsr_set_queue(VkQueue q) { g_queue = q; }

static bool exe_dir(char *path, size_t n) {
    DWORD k = GetModuleFileNameA(NULL, path, (DWORD)n);
    if (!k || k >= n) return false;
    while (k && path[k - 1] != '\\' && path[k - 1] != '/') k--;
    path[k] = 0;
    return k != 0;
}

extern "C" bool vk_fsr_available(void) {
    static int have = -1;
    if (have < 0) {
        char dir[MAX_PATH], p[MAX_PATH + 64];
        have = 0;
        if (exe_dir(dir, sizeof dir)) {
            snprintf(p, sizeof p, "%samd_fidelityfx_loader_vk.dll", dir);
            DWORD a = GetFileAttributesA(p);
            snprintf(p, sizeof p, "%samd_fidelityfx_upscaler_vk.dll", dir);
            DWORD b = GetFileAttributesA(p);
            have = a != INVALID_FILE_ATTRIBUTES && b != INVALID_FILE_ATTRIBUTES;
        }
        if (!have) snprintf(A.status, sizeof A.status, "no disponible (faltan las DLL de AMD FSR junto al programa)");
    }
    return have;
}

static void msg(uint32_t type, const wchar_t *m) {
    char b[512];
    size_t i = 0;
    for (; m && m[i] && i < sizeof b - 1; i++) b[i] = m[i] < 128 ? (char)m[i] : '?';
    b[i] = 0;
    flog("[fsr] %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "aviso", b);
}

extern "C" bool vk_fsr_init(VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
    if (A.tried) return A.ok;
    A.tried = true;
    A.phys = phys, A.dev = dev, A.gdpa = gdpa;
    if (!vk_fsr_available()) return false;
    char dir[MAX_PATH], p[MAX_PATH + 64];
    if (!exe_dir(dir, sizeof dir)) return false;
    snprintf(p, sizeof p, "%samd_fidelityfx_loader_vk.dll", dir);
    HMODULE m = LoadLibraryA(p);
    if (!m) {
        snprintf(A.status, sizeof A.status, "no disponible (no se pudo cargar amd_fidelityfx_loader_vk.dll: %lu)", GetLastError());
        flog("[fsr] %s", A.status);
        return false;
    }
    A.create = (PfnFfxCreateContext)(void *)GetProcAddress(m, "ffxCreateContext");
    A.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(m, "ffxDestroyContext");
    A.configure = (PfnFfxConfigure)(void *)GetProcAddress(m, "ffxConfigure");
    A.query = (PfnFfxQuery)(void *)GetProcAddress(m, "ffxQuery");
    A.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(m, "ffxDispatch");
    A.ok = A.create && A.destroy && A.configure && A.query && A.dispatch;
    if (!A.ok) {
        snprintf(A.status, sizeof A.status, "no disponible (el cargador de AMD FSR no tiene la API esperada)");
        flog("[fsr] %s", A.status);
        return false;
    }
    A.api = {(void *)A.create, (void *)A.destroy, (void *)A.configure, (void *)A.query, (void *)A.dispatch, phys, dev, gdpa};
    // las versiones que ofrece el SDK en esta tarjeta (al registro)
    VkDeviceContext dc = {dev, phys, gdpa};
    uint64_t count = 16, ids[16] = {};
    const char *names[16] = {};
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = &dc;
    q.outputCount = &count;
    q.versionIds = ids;
    q.versionNames = names;
    char list[400] = "";
    if (A.query(nullptr, &q.header) == FFX_API_RETURN_OK) {
        if (count > 16) count = 16;
        for (uint64_t i = 0; i < count; i++) {
            if (i) strncat(list, ", ", sizeof list - strlen(list) - 1);
            strncat(list, names[i] ? names[i] : "?", sizeof list - strlen(list) - 1);
        }
    }
    flog("[fsr] AMD FSR cargado; versiones del escalador en esta tarjeta: %s", count ? list : "(ninguna)");
    snprintf(A.status, sizeof A.status, "listo (%s)", count && names[0] ? names[0] : "versión desconocida");
    return true;
}

// Halton (base 2 y 3) desde 1, como la secuencia de FSR; fases: 8 x (salida / entrada)^2
static float halton(uint64_t i, uint32_t base) {
    float f = 1, r = 0;
    while (i > 0) {
        f /= (float)base;
        r += f * (float)(i % base);
        i /= base;
    }
    return r;
}
extern "C" void vk_fsr_jitter(uint64_t index, uint32_t render_w, uint32_t display_w, float *jx, float *jy) {
    float ratio = render_w ? (float)display_w / (float)render_w : 1;
    uint64_t phases = (uint64_t)ceilf(8.0f * ratio * ratio);
    if (phases < 1) phases = 1;
    uint64_t i = index % phases + 1;
    *jx = halton(i, 2) - 0.5f;
    *jy = halton(i, 3) - 0.5f;
}

// (SMRPG) su memoria de vídeo (el contexto, según AMD, y su imagen de salida), para el medidor del menú
// (runtime/ui/perf_meter.c); se mide al crearlo
static uint64_t g_fsr_vram, g_fsr_out_bytes;
extern "C" uint64_t vk_fsr_vram(void) { return g_fsr_vram; }

static void destroy_ctx(void) {
    g_fsr_vram = 0;
    if (C.ctx) A.destroy(&C.ctx, nullptr), C.ctx = nullptr;
    if (C.out) vkDestroyImage(A.dev, C.out, nullptr), C.out = VK_NULL_HANDLE;
    if (C.out_mem) vkFreeMemory(A.dev, C.out_mem, nullptr), C.out_mem = VK_NULL_HANDLE;
    C.out_ready = false;
}

extern "C" void vk_fsr_release(void) {
    if (!A.ok) return;
    destroy_ctx();
    C.w = C.h = 0;
    C.failed = false;
}

static bool make_output(uint32_t w, uint32_t h) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(A.dev, &ci, nullptr, &C.out) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(A.dev, C.out, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(A.phys, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(A.dev, &ai, nullptr, &C.out_mem) != VK_SUCCESS) return false;
    g_fsr_out_bytes = mr.size;
    return vkBindImageMemory(A.dev, C.out, C.out_mem, 0) == VK_SUCCESS;
}

extern "C" const FsrApi *vk_fsr_api(void) { return A.ok ? &A.api : nullptr; }

// La versión del SDK para ese tipo de contexto cuyo nombre empieza por want (escalador: 4 -> "4.1.2", 3 -> "3.1.6",
// 2 -> "2.3.4"; generación de fotogramas: 4 -> "4.0.2", 3 -> "3.1.6"); false: la del SDK
extern "C" bool vk_fsr_pick_version(uint64_t create_desc_type, int want, void *override_out) {
    if (!want || !A.ok) return false;
    ffxOverrideVersion *o = (ffxOverrideVersion *)override_out;
    VkDeviceContext dc = {A.dev, A.phys, A.gdpa};
    uint64_t count = 16, ids[16] = {};
    const char *names[16] = {};
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = create_desc_type;
    q.device = &dc;
    q.outputCount = &count;
    q.versionIds = ids;
    q.versionNames = names;
    if (A.query(nullptr, &q.header) != FFX_API_RETURN_OK) return false;
    for (uint64_t i = 0; i < count && i < 16; i++)
        if (names[i] && names[i][0] == '0' + want && names[i][1] == '.') {
            *o = {};
            o->header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
            o->versionId = ids[i];
            return true;
        }
    flog("[fsr] la versión %d no está entre las que ofrece el SDK: la suya", want);
    return false;
}
static bool pick_version(int want, ffxOverrideVersion *o) {
    return vk_fsr_pick_version(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, want, o);
}

static bool ensure_ctx(uint32_t w, uint32_t h, bool inverted, int version) {
    if (C.ctx && C.w == w && C.h == h && C.inverted == inverted && C.version == version) return true;
    if (C.failed && C.w == w && C.h == h && C.inverted == inverted && C.version == version) return false;
    if (C.ctx || C.out) {
        // (sus recursos pueden estar en entregas anteriores, todas en la cola del juego; un cambio de tamaño es raro)
        if (g_queue) {
            vk_queue_lock();
            vkQueueWaitIdle(g_queue);
            vk_queue_unlock();
        } else {
            vkDeviceWaitIdle(A.dev);
        }
        destroy_ctx();
    }
    C.w = w, C.h = h, C.inverted = inverted, C.version = version, C.failed = true;
    ffxCreateBackendVKDesc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext.vkDevice = A.dev;
    be.deviceContext.vkPhysicalDevice = A.phys;
    be.deviceContext.vkDeviceProcAddr = A.gdpa;
    ffxCreateContextDescUpscaleVersion ver = {};  // (obligatorio desde el SDK 2.1: la API con que se compiló)
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.header.pNext = &be.header;
    ver.version = FFX_UPSCALER_VERSION;
    ffxCreateContextDescUpscale d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    d.header.pNext = &ver.header;
    ffxOverrideVersion ov = {};
    if (pick_version(version, &ov)) ov.header.pNext = d.header.pNext, d.header.pNext = &ov.header;
    d.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION |
              (inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0);
    d.maxRenderSize = {w, h};
    d.maxUpscaleSize = {w, h};
    d.fpMessage = msg;
    ffxReturnCode_t r = A.create(&C.ctx, &d.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        C.ctx = nullptr;
        snprintf(A.status, sizeof A.status, "error al crear el contexto a %ux%u (%u)", w, h, (unsigned)r);
        flog("[fsr] %s", A.status);
        return false;
    }
    if (!make_output(w, h)) {
        destroy_ctx();
        snprintf(A.status, sizeof A.status, "sin memoria para su imagen de %ux%u", w, h);
        flog("[fsr] %s", A.status);
        return false;
    }
    ffxQueryGetProviderVersion v = {};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const char *name = A.query(&C.ctx, &v.header) == FFX_API_RETURN_OK && v.versionName ? v.versionName : "?";
    snprintf(A.status, sizeof A.status, "AMD FSR %s a %ux%u", name, w, h);
    {
        FfxApiEffectMemoryUsage mu = {};
        ffxQueryDescUpscaleGetGPUMemoryUsage q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
        q.gpuMemoryUsageUpscaler = &mu;
        uint64_t ctx = A.query(&C.ctx, &q.header) == FFX_API_RETURN_OK ? mu.totalUsageInBytes : 0;
        g_fsr_vram = ctx + g_fsr_out_bytes;
        flog("[fsr] memoria de vídeo: %.0f MB (contexto %.0f MB, imagen de salida %.0f MB)", g_fsr_vram / 1048576.0,
             ctx / 1048576.0, g_fsr_out_bytes / 1048576.0);
    }
    flog("[fsr] en uso: %s (profundidad %s)", A.status, inverted ? "invertida" : "normal");
    C.failed = false;
    return true;
}

static FfxApiResource res(const FsrImage *im, uint32_t extra_usage) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = im->format;
    ci.extent = {im->width, im->height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = im->usage;
    return ffxApiGetResourceVK((void *)im->image, ffxApiGetImageResourceDescriptionVK(im->image, ci, extra_usage),
                               FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
}

extern "C" void vk_fsr_record(VkCommandBuffer cb, const void *data) {
    const FsrDispatch *f = (const FsrDispatch *)data;
    if (!A.ok || !f->color.image || !f->depth.image || !f->mv.image) return;
    uint32_t w = f->color.width, h = f->color.height;  // (las imágenes: lo dibujado y la salida, dentro)
    uint32_t rw = f->render_w ? f->render_w : w, rh = f->render_h ? f->render_h : h;
    uint32_t ow = f->out_w ? f->out_w : rw, oh = f->out_h ? f->out_h : rh;
    if (rw > w || rh > h || ow > w || oh > h) return;
    bool fresh = !C.ctx || C.w != w || C.h != h || C.inverted != f->depth_inverted || C.version != f->version;
    if (!ensure_ctx(w, h, f->depth_inverted, f->version)) return;
    barrier(cb);
    if (!C.out_ready) {  // (la imagen de salida, de UNDEFINED a GENERAL una vez)
        VkImageMemoryBarrier ib = {};
        ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = C.out;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &ib);
        C.out_ready = true;
    }
    FsrImage out = {C.out, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cb;
    d.color = res(&f->color, 0);
    d.depth = res(&f->depth, 0);
    d.motionVectors = res(&f->mv, 0);
    d.output = res(&out, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset = {f->jitter_x, f->jitter_y};
    d.motionVectorScale = {f->mv_scale_x, f->mv_scale_y};
    d.renderSize = {rw, rh};
    d.upscaleSize = {ow, oh};
    d.enableSharpening = f->sharpness > 0;
    d.sharpness = f->sharpness > 1 ? 1.0f : f->sharpness;
    d.frameTimeDelta = f->frame_ms > 0 ? f->frame_ms : 16.67f;
    d.preExposure = 1.0f;
    d.reset = f->reset || fresh;
    // la cámara del fotograma (exec.c la saca de la vista-proyección de URP); sin ella, valores típicos de Unity.
    // FSR los usa para sus umbrales de profundidad.
    d.cameraNear = f->cam_near > 0 ? f->cam_near : 0.3f;
    d.cameraFar = f->cam_far > d.cameraNear ? f->cam_far : 1000.0f;
    d.cameraFovAngleVertical = f->cam_fov > 0.01f && f->cam_fov < 3.0f ? f->cam_fov : 0.6f;
    d.viewSpaceToMetersFactor = 1.0f;
    ffxReturnCode_t r = A.dispatch(&C.ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static int nlog;
        if (nlog++ < 5) flog("[fsr] ffxDispatch falló (%u)", (unsigned)r);
        return;
    }
    barrier(cb);
    VkImageBlit bl = {};  // el resultado, de vuelta en el color de la escena
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {(int32_t)ow, (int32_t)oh, 1};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.dstOffsets[1] = {(int32_t)ow, (int32_t)oh, 1};
    vkCmdBlitImage(cb, C.out, VK_IMAGE_LAYOUT_GENERAL, f->color.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    barrier(cb);
    static uint32_t lrw, low;
    if (C.frames++ == 0 || rw != lrw || ow != low) flog("[fsr] %ux%u -> %ux%u", rw, rh, ow, oh);
    lrw = rw, low = ow;
}

extern "C" const char *vk_fsr_status(void) {
    if (!A.status[0]) vk_fsr_available();
    return A.status[0] ? A.status : "sin cargar todavía";
}

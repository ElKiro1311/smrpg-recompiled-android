// (SMRPG) AMD FSR como antialiasing temporal: vulkan/vk_fsr.cpp (el SDK) y exec.c (dónde y con qué se llama).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VkImage image;
    VkFormat format;
    uint32_t width, height;
    VkImageUsageFlags usage;
} FsrImage;

// Lo que graba un paquete del hilo grabador (record.c rec_CmdCallback): copiado tal cual en el paquete.
typedef struct {
    FsrImage color;  // el color HDR de la escena (se escribe el resultado de vuelta en él)
    FsrImage depth;  // su profundidad
    FsrImage mv;     // los vectores de movimiento de URP (RG16F, en UV)
    float jitter_x, jitter_y;      // el desplazamiento subpíxel de este fotograma, en píxeles de la imagen
    float mv_scale_x, mv_scale_y;  // de los vectores de URP a píxeles hacia el fotograma anterior
    float frame_ms;
    float sharpness;  // 0: sin el afinado propio de FSR; 0..1
    bool reset;       // la historia no vale (corte de cámara, cambio de tamaño)
    bool depth_inverted;
    uint32_t render_w, render_h;  // lo dibujado (arriba a la izquierda de las imágenes)
    uint32_t out_w, out_h;        // el tamaño al que FSR lo amplía (igual: antialiasing nativo)
    int version;  // la versión pedida: 0 la que elija el SDK (la mejor para la tarjeta), 4, 3 (3.1) o 2 (2.3)
    float cam_near, cam_far, cam_fov;  // la cámara del fotograma (exec.c); 0: valores típicos de Unity
} FsrDispatch;

// (vk_fg.cpp) el SDK que cargó vk_fsr_init: sus funciones (los Pfn* de ffx_api.h) y el dispositivo; NULL sin cargar
typedef struct {
    void *create, *destroy, *configure, *query, *dispatch;
    VkPhysicalDevice phys;
    VkDevice dev;
    PFN_vkGetDeviceProcAddr gdpa;
} FsrApi;
const FsrApi *vk_fsr_api(void);
// La versión del SDK para ese tipo de contexto cuyo nombre empieza por «want.» (4 -> «4.0.2»): un ffxOverrideVersion
// en override_out; false: la que elija el SDK
bool vk_fsr_pick_version(uint64_t create_desc_type, int want, void *override_out);

bool vk_fsr_available(void);  // las DLL del SDK junto al ejecutable
// (hilo traductor, una vez con el dispositivo creado) carga las DLL; false si no están o no sirven
bool vk_fsr_init(VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa);
// el desplazamiento subpíxel número `index` de la secuencia de FSR (Halton 2, 3), en píxeles (-0.5..0.5)
void vk_fsr_jitter(uint64_t index, uint32_t render_w, uint32_t display_w, float *jx, float *jy);
void vk_fsr_record(VkCommandBuffer cb, const void *dispatch);  // (hilo grabador) un FsrDispatch
void vk_fsr_release(void);  // (con la GPU parada) el contexto y su imagen
// (device.c) la cola del juego: un contexto nuevo espera solo a ella (no vkDeviceWaitIdle: con la generación de
// fotogramas, la cola de presentación de AMD la usan sus propios hilos)
void vk_fsr_set_queue(VkQueue q);
const char *vk_fsr_status(void);  // para el menú: versión en uso o por qué no

#ifdef __cplusplus
}
#endif

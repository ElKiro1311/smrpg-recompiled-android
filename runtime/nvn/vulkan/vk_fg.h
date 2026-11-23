// (SMRPG) Generación de fotogramas de AMD FSR (vulkan/vk_fg.cpp): la swapchain de AMD en lugar de la de device.c y,
// cada fotograma con FSR, su preparación con la profundidad, los vectores de movimiento y la cámara (exec.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "vk_fsr.h"

#ifdef __cplusplus
extern "C" {
#endif

// La cámara del fotograma en el espacio del mundo de Unity (exec.c la saca de la vista-proyección de URP)
typedef struct {
    float pos[3], up[3], right[3], fwd[3];  // posición y ejes (unitarios)
    float fov_y;                            // campo de visión vertical, en radianes
    float znear, zfar;                      // planos (zfar muy grande: infinito)
    int ok;
} FgCamera;

// Lo que graba el paquete del hilo grabador en cada fotograma con FSR (exec.c fsr_dispatch, record.c rec_CmdCallback)
typedef struct {
    FsrImage depth, mv;           // la profundidad y los vectores de movimiento de la escena (los de FSR)
    uint32_t render_w, render_h;  // lo dibujado (arriba a la izquierda de las imágenes)
    float jitter_x, jitter_y;     // el desplazamiento subpíxel del fotograma, en píxeles
    float mv_scale_x, mv_scale_y; // de los vectores de URP a píxeles hacia el fotograma anterior
    bool reset;                   // la historia no vale (corte, cambio de tamaño)
    bool depth_inverted;
    bool paused;                  // el menú del runtime está abierto: no se generan fotogramas
    int version;                  // la pedida: 0 la del SDK (la mejor de la tarjeta), 4 o 3
    int pic[4];                   // dónde está la imagen del juego en la ventana (x, y, ancho, alto)
    FgCamera cam;
    FsrImage hudless;             // la ventana sin el menú del runtime (device.c vk_fg_hudless); image 0: sin ella
} FgPrepare;

// (device.c) la ventana sin el menú del runtime (contador de FPS, cronómetro), que este fotograma copia antes de que el
// menú dibuje encima: con ella AMD no interpola lo que el menú pone
bool vk_fg_hudless_wanted(void);
FsrImage vk_fg_hudless(void);

bool vk_fg_dll(void);              // amd_fidelityfx_framegeneration_vk.dll (y las de FSR) junto al ejecutable
uint32_t vk_fg_queue_count(void);  // colas de la familia gráfica que conviene pedir: 3 con la DLL, 1 sin ella
// (device.c, con el dispositivo creado) la cola del juego y las dos más para la presentación y la adquisición de AMD
void vk_fg_queues(VkQueue game, uint32_t family, VkQueue present, VkQueue acquire);
bool vk_fg_usable(void);           // el SDK cargado, las colas y sin un fallo antes (el ajuste lo mira device.c)
bool vk_fg_active(void);           // la swapchain de la ventana es la de AMD
bool vk_fg_generating(void);       // ... y el último fotograma preparado generaba
bool vk_fg_rebuild_wanted(void);   // el contexto de generación no sirve (otra versión o escena mayor): device.c recrea
                                   // la swapchain (con la GPU parada se va el contexto, y el fotograma siguiente hace otro)
// Las llamadas de swapchain de device.c mientras se quiere la generación. La primera creación hace el contexto de la
// swapchain de AMD; las siguientes (otro tamaño) la recrean en su sitio. Un fallo: VK_ERROR_* y device.c sigue con la
// suya. destroy solo anota el objeto (la creación siguiente lo recrea, o vk_fg_release lo destruye tras los contextos).
VkResult vk_fg_create_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc);
void vk_fg_destroy_swapchain(VkSwapchainKHR sc);
VkResult vk_fg_get_images(VkSwapchainKHR sc, uint32_t *n, VkImage *img);
VkResult vk_fg_acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx);
VkResult vk_fg_present(VkQueue queue, const VkPresentInfoKHR *pi);  // (hilo grabador)
void vk_fg_prepare_record(VkCommandBuffer cb, const void *prepare);    // (hilo grabador) un FgPrepare
void vk_fg_frame_end_record(VkCommandBuffer cb, const void *unused);   // (hilo grabador) antes de entregar la ventana
void vk_fg_wait_presents(void);    // las presentaciones de AMD en curso, hechas (antes de vkDeviceWaitIdle)
void vk_fg_release(void);          // todo abajo (con la GPU parada y el grabador quieto)
const char *vk_fg_status(void);    // para el menú
// (pruebas) LARECOMP_FG_PSHOT=N: desde el fotograma N, seis imágenes seguidas tal como se presentan
// (build/fg_pres_<i>_<real|generado>.png); LARECOMP_FG_MV="x,y" multiplica la escala de los vectores; LARECOMP_FG_DEBUG=1
// las marcas de AMD en los generados, =2 su comprobador de entradas (avisos al registro)

#ifdef __cplusplus
}
#endif

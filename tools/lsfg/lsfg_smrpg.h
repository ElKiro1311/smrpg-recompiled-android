// (SMRPG) Lossless Scaling Frame Generation para el port: la API en C de la biblioteca aparte (lsfg.dll en Windows,
// liblsfg.so en Android; tools/lsfg/build.sh) que lleva lsfg-vk (la rama de Android de GameNative, MIT), el traductor
// de DXBC a SPIR-V de DXVK (zlib), pe-parse (MIT) y volk (MIT). El port la carga al vuelo (runtime/nvn/vulkan/vk_lsfg.c)
// y solo habla con ella por aquí. Los sombreadores de LSFG salen del Lossless.dll de cada jugador (su copia de Lossless
// Scaling): nunca van con el port.
//
// Modo de un solo dispositivo: LSFG trabaja en el dispositivo y en la cola del port (initializeExternal), con imágenes
// del port en disposición GENERAL, y envía su trabajo a esa cola: hay que llamar a lsfg_smrpg_present desde el hilo que
// envía a la cola, en orden (después del envío que escribió la entrada, antes de los que leen las salidas). También
// envían a esa cola init, create, destroy y finalize (una imagen de reserva, un envío vacío con valla para esperar):
// con el cerrojo de la cola si otros hilos la usan.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSFG_SMRPG_ABI 2  // (lsfg_smrpg_abi: cambia si cambia esta API; 2: cache_path)

typedef struct {
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    const char *dll_path;  // el Lossless.dll del jugador
    int performance;       // 1: LSFG 3.1 «rendimiento» (más barato), 0: la normal
    float flow_scale;      // la «escala del flujo» de Lossless Scaling, 0,25 .. 1
    int generations;       // imágenes generadas por cada real (el multiplicador - 1)
    int hdr;               // imágenes HDR
    int orig_mipmaps;      // 1: el sombreador «mipmaps» del Lossless.dll; 0: el que trae lsfg-vk-android (por defecto)
    const char *cache_path;  // la caché de sus tuberías en disco (NULL: sin caché); se crea y se guarda sola
} LsfgSmrpgInit;

typedef int (*PFN_lsfg_smrpg_abi)(void);
// 0 bien; si no, err (hasta errlen bytes) dice por qué
typedef int (*PFN_lsfg_smrpg_init)(const LsfgSmrpgInit *in, char *err, int errlen);
// el contexto (>= 0) para dos entradas (in[0], in[1]: la imagen real nueva va en la de índice par / impar de cada
// fotograma, por turnos) y n salidas; < 0 si falla
typedef int (*PFN_lsfg_smrpg_create)(const VkImage in[2], const VkImage *out, int n, uint32_t w, uint32_t h, VkFormat fmt,
                                     char *err, int errlen);
// genera las n imágenes de este fotograma (envía su trabajo a la cola); 0 bien. Otra cosa (-2: su espera de 2 s agotada)
// es un contexto perdido: su cuenta de fotogramas no avanzó (la paridad de las entradas ya no casa) y puede haber dejado
// una valla sin enviar que la siguiente esperaría; hay que borrarlo y crear otro
typedef int (*PFN_lsfg_smrpg_present)(int ctx, char *err, int errlen);
typedef void (*PFN_lsfg_smrpg_destroy)(int ctx);
typedef void (*PFN_lsfg_smrpg_finalize)(void);

#ifdef __cplusplus
}
#endif

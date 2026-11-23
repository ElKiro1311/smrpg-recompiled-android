// (SMRPG) El medidor del menú (Vídeo > Rendimiento ahora, y el impacto de cada ajuste): lo que gasta el juego en cada
// componente, medido mientras corre (runtime/perf_meter.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PM_CPU, PM_GPU, PM_RAM, PM_SSD, PM_VRAM, PM_N };
enum { PM_OK = 0, PM_MID = 1, PM_HIGH = 2 };  // el semáforo: verde, amarillo, rojo

typedef struct {
    bool ok;                 // hay al menos dos muestras (los ritmos necesitan un intervalo)
    double fps;              // imágenes del juego por segundo
    double cpu_pct;          // el proceso, en % del procesador entero (todos sus hilos lógicos)
    int ncpu;
    bool gpu_known;          // (sin temporizador de GPU no se sabe)
    double gpu_pct, gpu_ms;  // la GPU ocupada: % del tiempo (Windows: con la generación de AMD) y ms por imagen del juego
    bool gpu_pdh;            // (el % viene de los contadores de Windows; si no, del temporizador del render)
    double ram_gb, ram_total_gb;      // memoria física del proceso (working set) y la del equipo
    double io_mbs;                    // MB/s leídos + escritos por el proceso (disco: texturas, guardados, cachés)
    double vram_gb, vram_budget_gb;   // la memoria de vídeo usada por el proceso y la que el controlador le deja
    uint64_t vram_targets, vram_hd, vram_tex, vram_fsr, vram_fg;  // bytes: destinos de render, texturas HD, el resto
                                                                  // de texturas, AMD FSR, generación de fotogramas
    double px;               // píxeles que se dibujan por imagen, en veces los de 1920x1080
    int lv[PM_N];            // el semáforo de cada componente
} PerfNow;

// La medición de ahora (nueva cada medio segundo; antes, la anterior). Hilo del render (donde se dibuja el menú).
const PerfNow *perf_meter_now(void);
// El nivel (PM_OK, PM_MID, PM_HIGH) de un valor: verde por debajo de a, amarillo por debajo de b, rojo desde b
static inline int pm_level(double v, double a, double b) { return v < a ? PM_OK : v < b ? PM_MID : PM_HIGH; }

#ifdef __cplusplus
}
#endif

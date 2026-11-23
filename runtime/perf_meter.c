// (SMRPG) El medidor del menú: lo que gasta el juego en CPU, GPU, RAM, disco y memoria de vídeo, medido mientras corre.
// Cada medio segundo (solo cuando alguien lo pide: el menú abierto, o LARECOMP_LOG_PERF=1 cada 2 s en el registro):
//  - CPU: el tiempo de CPU del proceso (todos sus hilos) entre dos muestras, en % del procesador entero;
//  - GPU: el temporizador de GPU del render (marcas de tiempo en cada envío, vulkan/exec.c pace_gpu_ns): el tiempo que
//    la GPU pasó con el juego, en % del tiempo y en ms por imagen (sin la generación de fotogramas de AMD, que va por
//    sus colas);
//  - RAM: la memoria física del proceso (working set);
//  - disco: los bytes leídos y escritos por el proceso, por segundo;
//  - memoria de vídeo: la que usa el proceso según el controlador (VK_EXT_memory_budget) y, por clases, la de sus
//    texturas (vulkan/textures.c vk_vram_by_kind) y la de AMD FSR y su generación de fotogramas (lo que dice AMD).
// El semáforo de cada uno (verde / amarillo / rojo): CPU < 30 % / < 60 %, GPU < 60 % / < 90 %, RAM < 25 % / < 50 % de la
// del equipo, disco < 10 / < 100 MB/s, memoria de vídeo < 60 % / < 85 % de lo que el controlador deja.
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "perf_meter.h"
#include "platform.h"

int64_t pace_gpu_ns(void);                                  // nvn/vulkan/exec.c (-1: sin temporizador de GPU)
uint64_t renderer_present_count(void);                      // nvn/vulkan/device.c
bool vk_vram_budget(uint64_t *usage, uint64_t *budget);     // nvn/vulkan/device.c
void vk_vram_by_kind(uint64_t out[3]);                      // nvn/vulkan/textures.c
uint64_t vk_fsr_vram(void);                                 // nvn/vulkan/vk_fsr.cpp
uint64_t vk_fg_vram(void);                                  // nvn/vulkan/vk_fg.cpp
void renderer_get_scale(float *sx, float *sy);              // nvn/vulkan/textures.c
extern int g_guest_w, g_guest_h;                            // nvn/output_scale.c

static PerfNow now_;
static struct {
    bool have;
    uint64_t t_ns, cpu_ns, io, frames;
    int64_t gpu_ns;
} prev;

static void sample(void) {
    PlatProcStats ps;
    uint64_t t = plat_now_ns(), frames = renderer_present_count();
    int64_t gpu = pace_gpu_ns();
    bool ok = plat_proc_stats(&ps);
    PerfNow n = now_;
    if (ok && prev.have && t > prev.t_ns) {
        double dt = (double)(t - prev.t_ns) / 1e9;
        n.ok = true;
        n.fps = (double)(frames - prev.frames) / dt;
        n.ncpu = ps.ncpu > 0 ? ps.ncpu : 1;
        n.cpu_pct = (double)(ps.cpu_ns - prev.cpu_ns) / 1e9 / dt / n.ncpu * 100.0;
        n.gpu_known = gpu >= 0 && prev.gpu_ns >= 0;
        if (n.gpu_known) {
            double g = (double)(gpu - prev.gpu_ns) / 1e9;
            n.gpu_pct = g / dt * 100.0;
            n.gpu_ms = frames > prev.frames ? g * 1000.0 / (double)(frames - prev.frames) : 0;
        }
        n.io_mbs = (double)(ps.io_read + ps.io_write - prev.io) / 1e6 / dt;
    }
    if (ok) {
        n.ram_gb = ps.ram_used / 1073741824.0;
        n.ram_total_gb = ps.ram_total / 1073741824.0;
    }
    uint64_t use, bud, kinds[3];
    if (vk_vram_budget(&use, &bud)) n.vram_gb = use / 1073741824.0, n.vram_budget_gb = bud / 1073741824.0;
    vk_vram_by_kind(kinds);
    n.vram_targets = kinds[0], n.vram_hd = kinds[1], n.vram_tex = kinds[2];
    n.vram_fsr = vk_fsr_vram(), n.vram_fg = vk_fg_vram();
    float sx = 1, sy = 1;
    renderer_get_scale(&sx, &sy);
    n.px = (double)g_guest_w * sx * (double)g_guest_h * sy / (1920.0 * 1080.0);
    {   // (la GPU entera según Windows, también la generación de fotogramas de AMD; si no, el temporizador del render)
        static uint64_t pdh_at;
        static double pdh_pct = -1;
        double v;
        if (!pdh_at || t - pdh_at >= 1000000000ull) {  // (los contadores de Windows: una vez por segundo)
            pdh_at = t;
            if (plat_gpu_usage(&v)) pdh_pct = v;
        }
        n.gpu_pdh = pdh_pct >= 0;
        if (n.gpu_pdh) n.gpu_pct = pdh_pct, n.gpu_known = true;
    }
    n.lv[PM_CPU] = pm_level(n.cpu_pct, 30, 60);
    n.lv[PM_GPU] = n.gpu_known ? pm_level(n.gpu_pct, 60, 90) : PM_OK;
    n.lv[PM_RAM] = n.ram_total_gb > 0 ? pm_level(n.ram_gb / n.ram_total_gb, 0.25, 0.5) : PM_OK;
    n.lv[PM_SSD] = pm_level(n.io_mbs, 10, 100);
    n.lv[PM_VRAM] = n.vram_budget_gb > 0 ? pm_level(n.vram_gb / n.vram_budget_gb, 0.6, 0.85) : PM_OK;
    now_ = n;
    prev.have = ok, prev.t_ns = t, prev.cpu_ns = ps.cpu_ns, prev.io = ps.io_read + ps.io_write, prev.frames = frames;
    prev.gpu_ns = gpu;
}

const PerfNow *perf_meter_now(void) {
    uint64_t t = plat_now_ns();
    if (!prev.have || t - prev.t_ns >= 500000000ull) sample();
    return &now_;
}

// LARECOMP_LOG_PERF=1 (pruebas: el impacto de cada ajuste): una línea [perf] cada 2 s. Desde la presentación
// (vulkan/device.c), en el hilo del render.
void perf_meter_log_tick(void) {
    static int on = -1;
    static uint64_t last;
    if (on < 0) on = getenv("LARECOMP_LOG_PERF") && atoi(getenv("LARECOMP_LOG_PERF")) != 0;
    if (!on) return;
    uint64_t t = plat_now_ns();
    if (last && t - last < 2000000000ull) return;
    last = t;
    const PerfNow *n = perf_meter_now();
    if (!n->ok) return;
    LOG("[perf] fotograma %llu %.1f fps cpu %.1f%% gpu %.1f%% %.2f ms/imagen ram %.2f GB disco %.1f MB/s vram %.2f/%.2f GB "
        "(destinos %.0f MB, HD %.0f MB, texturas %.0f MB, fsr %.0f MB, fg %.0f MB) px %.2f\n",
        (unsigned long long)renderer_present_count(), n->fps, n->cpu_pct, n->gpu_pct, n->gpu_ms, n->ram_gb, n->io_mbs,
        n->vram_gb, n->vram_budget_gb, n->vram_targets / 1048576.0, n->vram_hd / 1048576.0, n->vram_tex / 1048576.0,
        n->vram_fsr / 1048576.0, n->vram_fg / 1048576.0, n->px);
}

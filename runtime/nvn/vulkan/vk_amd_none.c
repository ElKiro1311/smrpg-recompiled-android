// (SMRPG Android) AMD FSR and its frame generation (vk_fsr.cpp, vk_fg.cpp) are the AMD SDK's DLLs for Windows: there
// are none here. Outwardly the same as without the DLLs on Windows: "not available", one queue, the normal swapchain.
#ifndef _WIN32  // (on Windows there are the real ones, vk_fsr.cpp and vk_fg.cpp)
#include "../../ui/tr.h"
#include "vk_fg.h"
#include "vk_fsr.h"

const FsrApi *vk_fsr_api(void) { return NULL; }
bool vk_fsr_pick_version(uint64_t create_desc_type, int want, void *override_out) {
    (void)create_desc_type, (void)want, (void)override_out;
    return false;
}
bool vk_fsr_available(void) { return false; }
bool vk_fsr_init(VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
    (void)phys, (void)dev, (void)gdpa;
    return false;
}
void vk_fsr_jitter(uint64_t index, uint32_t render_w, uint32_t display_w, float *jx, float *jy) {
    (void)index, (void)render_w, (void)display_w;
    *jx = *jy = 0;
}
void vk_fsr_record(VkCommandBuffer cb, const void *dispatch) { (void)cb, (void)dispatch; }
void vk_fsr_release(void) {}
void vk_fsr_set_queue(VkQueue q) { (void)q; }
const char *vk_fsr_status(void) { return tr("not available on Android (AMD FSR is for Windows)"); }
uint64_t vk_fsr_vram(void) { return 0; }  // (runtime/perf_meter.c)
uint64_t vk_fg_vram(void) { return 0; }

// (vk_fg_hudless_wanted and vk_fg_hudless are in device.c)
bool vk_fg_dll(void) { return false; }
uint32_t vk_fg_queue_count(void) { return 1; }
void vk_fg_queues(VkQueue game, uint32_t family, VkQueue present, VkQueue acquire) {
    (void)game, (void)family, (void)present, (void)acquire;
}
bool vk_fg_usable(void) { return false; }
bool vk_fg_active(void) { return false; }
bool vk_fg_generating(void) { return false; }
bool vk_fg_rebuild_wanted(void) { return false; }
VkResult vk_fg_create_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc) {
    (void)ci, (void)sc;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
void vk_fg_destroy_swapchain(VkSwapchainKHR sc) { (void)sc; }
VkResult vk_fg_get_images(VkSwapchainKHR sc, uint32_t *n, VkImage *img) {
    (void)sc, (void)n, (void)img;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
VkResult vk_fg_acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx) {
    (void)sc, (void)timeout, (void)sem, (void)idx;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
VkResult vk_fg_present(VkQueue queue, const VkPresentInfoKHR *pi) {
    (void)queue, (void)pi;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
void vk_fg_prepare_record(VkCommandBuffer cb, const void *prepare) { (void)cb, (void)prepare; }
void vk_fg_frame_end_record(VkCommandBuffer cb, const void *unused) { (void)cb, (void)unused; }
void vk_fg_wait_presents(void) {}
void vk_fg_release(void) {}
const char *vk_fg_status(void) { return tr("not available on Android (AMD frame generation is for Windows)"); }
#endif

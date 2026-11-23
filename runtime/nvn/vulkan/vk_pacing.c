// (SMRPG Android) Present pacing with VK_GOOGLE_display_timing: each game frame on screen for a fixed number of
// refreshes. The game times its frames by hle/vi.c's clock (60 per second, or the "FPS limit"), which didn't follow the
// display's refreshes: in MAILBOX each frame came out on the first refresh after the GPU finished it, and at 60 fps on
// a 120 Hz display some of those finishing close to a refresh came out before it and others after: some 8 ms on screen,
// others 25 (AYN Thor, 10 minutes: 1,080 and 1,081 of 32,367 frames, about 2 hitches per second).
// Now:
// - each present asks for its time (VkPresentTimesInfoGOOGLE: "not before") on the refresh grid given by the past
//   presents (vkGetPastPresentationTimingGOOGLE: when each one actually came out), at a fixed step: the game's vsync
//   period in refreshes (2 at 60 fps and 120 Hz; if it isn't a whole number of refreshes - 90 fps at 120 Hz - no time,
//   as before). It arrives ahead of its time (the lead) and the display holds it until then; one that arrives up to
//   half a refresh later than the lead calls for still lands in its slot. If 3 in 60 frames come out late (later than
//   is normal on that display - on the Thor, one refresh before the requested one: its compositor presents early - and
//   not because the compositor held them; a lone one doesn't count, since rephasing the step for it would be another
//   hitch), the next one rephases the step with more lead, up to 2 frames (also when the step shrinks: the lead from 30
//   fps doesn't carry over to a battle at 60); those right after a stall (a load, coming back from the background)
//   don't count.
// - the game's vsync falls on a display refresh (vk_pacing_vsync, from vi.c): the CPU's clock and the display's aren't
//   exactly in step (0.01% is already a frame of drift every minute and a bit), and without this the step was rephased
//   (a hitch) every time they drifted a refresh apart. Only if the display is within 0.5% of that rate: that way the
//   game runs at most 0.5% faster or slower.
// - (Android) the display is asked for the refresh rate of the game's pace (vote_rate: 60 Hz at 60 fps), because at 120
//   Hz the Thor's compositor cuts it so close that one frame every ~5 s came out a refresh late.
// Only the normal path: with Lossless Scaling generation (FIFO, vk_lsfg.c) each image already gets one refresh.
// By default only on Android (where it was tested). LARECOMP_VK_DISPLAY_TIMING=0 turns it off (=1 tries it on another
// system with the extension); LARECOMP_VK_PACING_VSYNC=0 leaves vsync on its own clock; LARECOMP_VK_PACING_LOG=<n>: for
// the first <n> paced frames, raw, the requested refresh and when they came out ([pacing] d and p lines).
#include "vk.h"
#include "../../settings.h"
#include <stdlib.h>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif

uint64_t plat_now_ns(void);
extern volatile uint64_t g_vsync_period_ns;  // (hle/vi.c) the game's vsync period now (0 before the first one)
extern uint64_t (*volatile g_vsync_follow)(uint64_t next, uint64_t period);  // (hle/vi.c)

#define LD(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define ST(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)

static struct {
    bool on;
    PFN_vkGetRefreshCycleDurationGOOGLE refresh_fn;
    PFN_vkGetPastPresentationTimingGOOGLE past_fn;
    // (the presenting thread; device.c zeroes them with it stopped. refresh, anchor and paced_at are also read by the
    // vsync thread: LD / ST)
    VkSwapchainKHR sc;
    uint64_t refresh;     // ns per display refresh (as measured by the compositor)
    uint64_t refresh_at;  // when it was queried
    uint64_t anchor;      // the actual time of a refresh (the last past present's); 0: not known yet
    uint64_t last;        // the refresh requested for the last paced frame; 0: the next one rephases the step
    uint64_t steps;       // refreshes per frame
    uint64_t lead;        // ns: how long before its refresh a frame has to arrive (lead)
    uint64_t paced_at;    // when the last paced frame was presented (vsync only follows it while there are any)
    uint64_t prev_now;    // when the last time was requested (0: the swapchain is new)
    // the last one presented; the one from which late ones count (earlier ones no longer say anything); the one up to
    // which they don't raise the lead (those right after a stall)
    uint32_t id, rephase_id, grace_id;
    uint32_t late_ids[2];  // the last two isolated late ones (0: none)
    int early;  // > 0: the display puts frames out one refresh before the requested one (-64..64: the latest ones
    // decide)
    // (Android) the refresh rate requested from the display (vote_rate): the swapchain's native window, what was
    // requested (-1: nothing yet on this window) and until when the refresh is queried often (after requesting another
    // rate)
    bool match;
    void *anw;
    float voted;
    float sys_hz;  // the display's refresh rate at the start, before requesting anything (the system's; 0: not known yet)
    uint64_t fast_until;
    int log_n;
    uint64_t n_paced, n_late, n_held, n_rephase, log_at;  // (for the log, every minute)
} P;

static int env_int(const char *name, int def) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : def;
}

// (device.c, when choosing the extensions) whether it is wanted: by default only on Android
bool vk_pacing_wanted(void) {
#ifdef __ANDROID__
    return env_int("LARECOMP_VK_DISPLAY_TIMING", 1) != 0;
#else
    return env_int("LARECOMP_VK_DISPLAY_TIMING", 0) != 0;
#endif
}

// the game's next vsync (hle/vi.c, on its thread: next = the previous one + period) on the nearest refresh: the step
// ends up a whole number of refreshes and in phase with the display
static uint64_t vk_pacing_vsync(uint64_t next, uint64_t period) {
    uint64_t r = LD(P.refresh), anchor = LD(P.anchor), paced = LD(P.paced_at);
    if (!r || !anchor || !paced || (int64_t)(next - paced) > 500000000) return next;  // (no paced frames: its own clock)
    uint64_t steps = (period + r / 2) / r;
    int64_t off = (int64_t)(steps * r) - (int64_t)period;
    if (!steps || (uint64_t)(off < 0 ? -off : off) > period / 200) return next;
    int64_t d = (int64_t)(next - anchor);
    if (d < 0 || d > 2000000000) return next;
    return anchor + ((uint64_t)d + r / 2) / r * r;
}

// (device.c, after creating the device) ext: VK_GOOGLE_display_timing enabled (vk_pacing_wanted)
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
    LOG("[vk] presentation pacing: %s%s\n", P.on ? "each image at its own time (VK_GOOGLE_display_timing)"
                                         : ext ? "no timing (the VK_GOOGLE_display_timing functions are missing)"
                                               : "each image as soon as it is done",
        P.on ? (follow ? "; the game's vsync, on the display's refreshes" : "; the game's vsync, on its own "
                                                                                   "clock") : "");
}

static void ask_refresh(VkSwapchainKHR sc, uint64_t now) {
    VkRefreshCycleDurationGOOGLE d = {0};
    P.refresh_at = now;
    if (P.refresh_fn(g_vk.dev, sc, &d) != VK_SUCCESS || !d.refreshDuration) return;
    uint64_t old = P.refresh;
    if (old && (d.refreshDuration > old ? d.refreshDuration - old : old - d.refreshDuration) * 100 < old) {
        ST(P.refresh, d.refreshDuration);  // (what the compositor keeps measuring: the same grid)
        return;
    }
    LOG("[vk] pacing: display refresh %.4f ms (%.2f Hz)\n", d.refreshDuration / 1e6, 1e9 / (double)d.refreshDuration);
    if (!P.sys_hz) P.sys_hz = (float)(1e9 / (double)d.refreshDuration);
    ST(P.refresh, d.refreshDuration);
    ST(P.anchor, 0);  // (another rate - the one requested, or the system's 60 / 120 Hz setting: another grid)
    // and the starting lead and what is normal on that display, again. The lead: the GPU (~12 ms on the Thor) and the
    // compositor starting to compose before the refresh (on the Thor, 10.2 ms at 120 Hz: 1.2 refreshes): 22.4 ms at 120
    // Hz (with 22, 46 hitches in 5 minutes; with 16, 362, although in that test the compositor was also busier)
    P.lead = 12000000 + d.refreshDuration + d.refreshDuration / 4;
    P.last = 0, P.early = 0, P.late_ids[0] = P.late_ids[1] = 0, P.prev_now = 0;
}

// (SMRPG Android) The display refresh rate requested for the game's pace (ANativeWindow_setFrameRate): at 60 fps, 60
// Hz. At 120 Hz, on the Thor, the compositor starts composing only 1.85 ms before the refresh and presents early if the
// submission to the panel gets there more than ~0.7 ms before vsync: it gets there with 0.2-0.4 ms to spare, and when
// the compositor, the display service or the kernel take a little longer (atrace trace), the frame comes out a refresh
// late: the hitch that remained every ~5 s. At 60 Hz it starts ~15 ms before. With DEFAULT compatibility, because only
// then can the foreground layer leave the range the system sets (on the Thor, a fixed 120 Hz); Android goes back to its
// own rate on leaving the game. With Lossless Scaling generation, the rate it puts out (the pace times the multiplier:
// 120 with x2; if there is no such rate, Android sets the highest): withdrawing the request wasn't enough, the Thor
// stayed at 60 Hz and generation couldn't get its frames out. With "Screen refresh rate: the system's" in the menu
// (match_refresh), the one the display had at the start, requested too (for the same reason: withdrawing the request
// doesn't bring it back). LARECOMP_VK_MATCH_REFRESH=0: nothing, ever.
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
    if (hz > 0) LOG("[vk] pacing: asks the display for %.0f Hz (%s)%s\n", hz, gen_rate() ? "Lossless Scaling frame "
                                                                                        "generation"
                    : g_settings.match_refresh ? "the game's pace" : "the system's", r ? " — rejected" : "");
    else LOG("[vk] pacing: leaves the display's refresh rate alone%s\n", r ? " — rejected" : "");
    P.voted = hz;
    P.fast_until = now + 3000000000ull;  // (the mode change: seen sooner)
#else
    (void)hz, (void)now;
#endif
}

// (device.c create_swapchain and surface_drop, with the presenting thread stopped) another swapchain (or none): its
// refresh, and the step and the grid from scratch
void vk_pacing_swapchain(VkSwapchainKHR sc) {
    P.sc = sc, P.last = 0, P.prev_now = 0;
    ST(P.anchor, 0);
    ST(P.paced_at, 0);
    extern void *vk_android_window(void);
    P.anw = sc ? vk_android_window() : NULL, P.voted = -1;  // (another window: request it again)
    if (!sc) return;
    if (P.on) ask_refresh(sc, plat_now_ns());  // (before requesting anything: the first time, the system's rate)
    // (already when the swapchain is created: with Lossless Scaling generation its presents don't go through
    // vk_pacing_time, and they have to run at its rate from the start)
    vote_rate(wanted_rate(LD(g_vsync_period_ns)), plat_now_ns());
}

static uint64_t grid_round(uint64_t t) {  // the grid refresh closest to t
    int64_t d = (int64_t)(t - P.anchor), r = (int64_t)P.refresh;
    return P.anchor + (uint64_t)((d >= 0 ? (d + r / 2) / r : -((-d + r / 2) / r)) * r);
}
static uint64_t grid_ceil(uint64_t t) {  // the first one not before t
    int64_t d = (int64_t)(t - P.anchor), r = (int64_t)P.refresh;
    return P.anchor + (uint64_t)((d >= 0 ? (d + r - 1) / r : -((-d) / r)) * r);
}

// (record.c present_run, on the presenting thread) the time this present asks for: true with *t filled in
// (desiredPresentTime 0 while the grid isn't known: the present carries its ID so that the past ones provide it), false
// without it
bool vk_pacing_time(VkSwapchainKHR sc, VkPresentTimeGOOGLE *t) {
    if (!P.on || sc != P.sc || !sc) return false;
    uint64_t now = plat_now_ns(), period = LD(g_vsync_period_ns);
    vote_rate(wanted_rate(period), now);
    if (g_vk.sc_lsfg) return false;
    // (changing the rate doesn't recreate the swapchain: it is queried every 2 s, and every 100 ms after requesting
    // another)
    if (now - P.refresh_at > (now < P.fast_until ? 100000000ull : 2000000000ull)) ask_refresh(sc, now);
    uint64_t r = P.refresh;
    if (!r || !period) return false;
    uint64_t steps = (period + r / 2) / r;
    if (!steps) steps = 1;
    int64_t off = (int64_t)(steps * r) - (int64_t)period;
    if ((uint64_t)(off < 0 ? -off : off) > period / 25) { P.last = 0; return false; }  // (not whole refreshes)
    if (steps != P.steps) P.steps = steps, P.last = 0;
    // the lead, up to 2 frames (any more and the held ones don't fit in the swapchain); also when the step shrinks:
    // from 30 fps to a battle, which runs at 60, or from 60 to 120
    uint64_t cap = 2 * steps * r;
    if (P.lead > cap) P.lead = cap;
    // the past presents (Android provides them once there are 4 newer ones): the grid and the ones that came out late
    for (int round = 0; round < 4; round++) {
        VkPastPresentationTimingGOOGLE past[16];
        uint32_t n = 16;
        VkResult pr = P.past_fn(g_vk.dev, sc, &n, past);
        if (pr != VK_SUCCESS && pr != VK_INCOMPLETE) break;
        for (uint32_t i = 0; i < n; i++) {
            const VkPastPresentationTimingGOOGLE *p = &past[i];
            if (P.log_n > 0)  // (raw, ns: id, requested, actual, earliest possible, margin)
                LOG("[pacing] p %u %llu %llu %llu %lld\n", p->presentID, (unsigned long long)p->desiredPresentTime,
                    (unsigned long long)p->actualPresentTime, (unsigned long long)p->earliestPresentTime,
                    (long long)p->presentMargin);
            if (!p->actualPresentTime) continue;  // (it didn't come out, or without its times)
            ST(P.anchor, p->actualPresentTime);
            if (!p->desiredPresentTime) continue;
            // where it came out relative to its refresh (desired + r/2), in refreshes. Normal isn't 0 on every display:
            // the Thor's puts almost all of them out one refresh BEFORE (its compositor starts composing 10.3 ms before
            // the refresh and, if it finishes in ~2 ms, presents on the previous one: "Early Present"), so what is
            // normal is learned (early)
            int64_t d = (int64_t)(p->actualPresentTime - (p->desiredPresentTime + r / 2)), ri = (int64_t)r;
            int64_t k = d >= 0 ? (d + ri / 2) / ri : -((-d + ri / 2) / ri);
            if (p->presentID > P.grace_id) P.early += k < 0 ? (P.early < 64) : -(P.early > -64);
            int64_t base = P.early > 0 ? -1 : 0;
            if (k <= base) continue;
            // it came out later than normal. If it could have come out earlier (Android works it out: the compositor
            // took it more than one refresh before presenting it, with the GPU already done), the compositor held it
            // (on the Thor, when it goes over its 2 ms): nothing to correct here
            if (p->earliestPresentTime && p->earliestPresentTime < p->actualPresentTime) { P.n_held++; continue; }
            // otherwise, the GPU or the submission didn't make it in time. A single one says nothing (on the Thor a few
            // come out per minute with any lead, even the maximum: the CPU or the compositor) and rephasing the step
            // for it would be another hitch; 3 in 60 frames do: the lead isn't enough. Then the next one rephases the
            // step with more lead, and those presented up to it, with the earlier lead, no longer count. Those right
            // after a stall (a load, coming back from the background, the new swapchain) don't count: they are only
            // late in passing (the GPU is still ramping up), and if they counted the lead would stay high for the whole
            // session
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
                LOG("[vk] pacing: image %u came out %lld refresh%s later than usual%s; lead %.2f ms\n", p->presentID,
                    (long long)(k - base), k - base == 1 ? "" : "s",
                    grace ? " (after a hitch: does not count)" : more ? " (3 in 60 images: the step anew, with more lead)" : "",
                    P.lead / 1e6);
        }
        if (pr == VK_SUCCESS) break;
    }
    t->presentID = ++P.id;
    t->desiredPresentTime = 0;
    if (!P.prev_now || now - P.prev_now > 100000000ull) P.grace_id = P.id + 60;  // (a stall: 1 s at 60 fps)
    P.prev_now = now;
    if (!P.anchor) return true;
    // its refresh: the step's (the previous one + steps refreshes) if it can make it - up to half a refresh short of
    // the lead - and it isn't a whole frame early (the game got ahead); otherwise, the first one with the full lead
    uint64_t earliest = now + P.lead, natural = grid_ceil(earliest), target = natural;
    bool keep = false;
    if (P.last) {
        uint64_t want = grid_round(P.last + steps * r);
        keep = want + r / 2 >= earliest && want < natural + steps * r;
        if (keep) target = want;
    }
    if (!keep) P.n_rephase++, P.rephase_id = P.id;
    P.last = target;
    t->desiredPresentTime = target - r / 2;  // (it comes out on the refresh whose time is not before this one)
    P.n_paced++;
    ST(P.paced_at, now);
    if (P.log_n > 0) {  // (raw, ns: id, now, the game's last vsync, the requested refresh, the grid, usual step)
        P.log_n--;
        extern volatile uint64_t g_vsync_ns;
        LOG("[pacing] d %u %llu %llu %llu %llu %d\n", P.id, (unsigned long long)now, (unsigned long long)LD(g_vsync_ns),
            (unsigned long long)target, (unsigned long long)P.anchor, keep);
    }
    if (!P.log_at) P.log_at = now;
    if (now - P.log_at >= 60000000000ull) {
        LOG("[vk] pacing: in 1 min %llu images at their own time (%llu refreshes each), %llu late, %llu held by the "
            "compositor, %llu times the step anew; lead %.2f ms; the display shows them %s; refresh %.4f ms\n",
            (unsigned long long)P.n_paced, (unsigned long long)steps, (unsigned long long)P.n_late,
            (unsigned long long)P.n_held, (unsigned long long)P.n_rephase, P.lead / 1e6,
            P.early > 0 ? "one refresh earlier than asked" : "as asked", r / 1e6);
        P.n_paced = P.n_late = P.n_held = P.n_rephase = 0, P.log_at = now;
    }
    return true;
}

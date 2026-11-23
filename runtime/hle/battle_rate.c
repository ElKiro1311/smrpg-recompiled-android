// (SMRPG) Battles at 60 frames per second, with their logic at the console's pace, whatever the "FPS limit".
//
// Battle logic (BattleManager.update_common, main 0x7001cd9360: turns, hits, the timed-hit windows, sounds, camera
// moves) advances in steps of 1/60 s: BattleManager.update (main 0x7001cd9250) calls it once per frame (and once more
// if _frame_rate, +0x1c8, is 30). When a battle starts the game calls set_frame_rate (main 0x7001cd0500) with 60
// (QualitySettings.vSyncCount = 1: one step per frame; with BattleDebug's _is_30fps, a debug option that is off in the
// game, it would be 30, vSyncCount = 2 and two steps per frame). The character animators, on the other hand, run on
// time (BattleCharacterAnimator.internal_set_animator_speed, main 0x7001e32420, only slows them down if a frame takes
// longer than normal), and so do the sound and the UI.
// Here the vsync that sets Unity's pace ticks at the "FPS limit" (hle/vi.c): at 240, the battle ran at 240 frames with
// one step in each = 240 steps per second, four times faster than its animations and its sound (the timed hits, the
// sound against the picture and the speed, all out of step above 60).
// Now set_frame_rate (replaced in tools/game_overrides.c) reports here and, while the battle has its rate set (until
// the game puts _frame_rate back to 0 on leaving), the vsync ticks at 60 Hz: 60 frames and 60 steps per second, as on
// the console. With frame generation, 120 on screen. (Running the logic at 60 and drawing at 240 by skipping steps on
// some frames would lose presses: each step reads the buttons of its frame.)
// Tests: LARECOMP_BATTLE_FPS=30 (the debug mode: 30 frames with two steps, at 60 Hz); LARECOMP_BATTLE_FIX=0 (as before:
// the rate the game asks for and the vsync at the FPS limit, for comparison).
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../hle.h"

static volatile uint64_t bm_statics;  // BattleManager's static fields (_instance at +0x10)
static volatile int battle_on;        // (what the vsync thread reads: that thread doesn't touch game memory)
RT_ON_RESET(battle_rate_reset) { bm_statics = 0, battle_on = 0; }  // (a restart rebuilds game memory)

static int fix_on(void) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_BATTLE_FIX") || atoi(getenv("LARECOMP_BATTLE_FIX")) != 0;
    return on;
}

// (tools/game_overrides.c, set_frame_rate; Unity's main thread) the rate of the battle that is starting
int rt_battle_frame_rate(uint64_t statics, int requested) {
    if (!fix_on()) return requested;
    bm_statics = statics;  // (the battle is seen on the next frame, rt_battle_poll: _frame_rate is set on return)
    static int want = -1;
    if (want < 0) want = getenv("LARECOMP_BATTLE_FPS") && atoi(getenv("LARECOMP_BATTLE_FPS")) == 30 ? 30 : 60;
    extern uint64_t renderer_present_count(void);
    LOG("[battle] the game asks for %d FPS: %d (%s), with vsync at 60 Hz while it lasts (frame %llu)\n", requested, want,
        want == 60 ? "one 1/60 s logic step per frame" : "two steps per frame, the debug mode",
        (unsigned long long)renderer_present_count());
    return want;
}

// (nvn/vulkan/device.c backend_present: a game thread, every frame) is the battle still going, with its rate set?
// BattleManager._instance with _frame_rate != 0 (_instance is a static reference: as long as it points to the object,
// the object is still alive). Read by a game thread, which is stopped before the game's memory is rebuilt.
void rt_battle_poll(void) {
    uint64_t s = bm_statics;
    if (!s) return;
    uint64_t inst = LD64(s + 0x10);
    battle_on = inst && LD32(inst + 0x1c8) != 0;
}

// (hle/vi.c, the vsync thread, on every tick) true while a battle lasts
bool rt_battle_vsync_60(void) { return battle_on != 0; }

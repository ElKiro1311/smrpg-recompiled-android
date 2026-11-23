// (SMRPG) Los combates a 60 imágenes por segundo, con su lógica al ritmo de la consola, sea cual sea el «Límite de FPS».
//
// La lógica del combate (BattleManager.update_common, main 0x7001cd9360: turnos, golpes, las ventanas del golpe a
// tiempo, los sonidos, los movimientos de cámara) avanza por pasos de 1/60 s: BattleManager.update (main 0x7001cd9250)
// la llama una vez por imagen (y otra más si _frame_rate, +0x1c8, vale 30). Al empezar un combate el juego llama a
// set_frame_rate (main 0x7001cd0500) con 60 (QualitySettings.vSyncCount = 1: un paso por imagen; con el _is_30fps de
// BattleDebug, de depuración y apagado en el juego, sería 30, vSyncCount = 2 y dos pasos por imagen). Los animadores de
// los personajes, en cambio, van por tiempo (BattleCharacterAnimator.internal_set_animator_speed, main 0x7001e32420,
// solo los frena si una imagen tarda más de lo normal), igual que el sonido y la interfaz.
// Aquí el vsync que marca el ritmo de Unity late al «Límite de FPS» (hle/vi.c): a 240, el combate iba a 240 imágenes
// con un paso en cada una = 240 pasos por segundo, cuatro veces más deprisa que sus animaciones y su sonido (los
// golpes a tiempo, el sonido frente a la imagen y la velocidad, desfasados por encima de 60).
// Ahora set_frame_rate (sustituida en tools/game_overrides.c) avisa aquí y, mientras el combate tenga su tasa puesta
// (hasta que el juego devuelve _frame_rate a 0 al salir), el vsync late a 60 Hz: 60 imágenes y 60 pasos por segundo,
// como en la consola. Con la generación de fotogramas, 120 en pantalla. (Correr la lógica a 60 y dibujar a 240
// saltándose pasos en unas imágenes perdería pulsaciones: cada paso lee los botones de su imagen.)
// Pruebas: LARECOMP_BATTLE_FPS=30 (el modo de depuración: 30 imágenes con dos pasos, a 60 Hz); LARECOMP_BATTLE_FIX=0
// (como antes: la tasa que pide el juego y el vsync al límite de FPS, para comparar).
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../hle.h"

static volatile uint64_t bm_statics;  // los campos estáticos de BattleManager (_instance en +0x10)
static volatile int battle_on;        // (lo que lee el hilo del vsync: él no toca la memoria del juego)
RT_ON_RESET(battle_rate_reset) { bm_statics = 0, battle_on = 0; }  // (la memoria del juego se rehace al reiniciarlo)

static int fix_on(void) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_BATTLE_FIX") || atoi(getenv("LARECOMP_BATTLE_FIX")) != 0;
    return on;
}

// (tools/game_overrides.c, set_frame_rate; el hilo principal de Unity) la tasa del combate que empieza
int rt_battle_frame_rate(uint64_t statics, int requested) {
    if (!fix_on()) return requested;
    bm_statics = statics;  // (el combate se ve en la próxima imagen, rt_battle_poll: _frame_rate se pone al volver)
    static int want = -1;
    if (want < 0) want = getenv("LARECOMP_BATTLE_FPS") && atoi(getenv("LARECOMP_BATTLE_FPS")) == 30 ? 30 : 60;
    extern uint64_t renderer_present_count(void);
    LOG("[combate] el juego pide %d FPS: %d (%s), con el vsync a 60 Hz mientras dure (fotograma %llu)\n", requested, want,
        want == 60 ? "un paso de lógica de 1/60 s por imagen" : "dos pasos por imagen, el modo de depuración",
        (unsigned long long)renderer_present_count());
    return want;
}

// (nvn/vulkan/device.c backend_present: un hilo del juego, en cada imagen) ¿sigue el combate con su tasa puesta?
// BattleManager._instance con _frame_rate != 0 (_instance es una referencia estática: mientras apunta al objeto, el
// objeto sigue vivo). Lo lee un hilo del juego, que se detiene antes de que se rehaga su memoria.
void rt_battle_poll(void) {
    uint64_t s = bm_statics;
    if (!s) return;
    uint64_t inst = LD64(s + 0x10);
    battle_on = inst && LD32(inst + 0x1c8) != 0;
}

// (hle/vi.c, el hilo del vsync, en cada latido) true mientras dura un combate
bool rt_battle_vsync_60(void) { return battle_on != 0; }

// User settings (larecomp.ini next to the executable's working directory), edited from the in-game menu.
#pragma once
#include "cosmetic_colour.h"
#include <stdbool.h>

// unlocked: la pantalla del juego toma el aspecto de la de quien juega (21:9, 32:9, 16:10: más escena a los lados o
// arriba y abajo, nunca estirada); original: 16:9 con bandas
enum { ASPECT_UNLOCKED = 0, ASPECT_ORIGINAL = 1 };
// (SMRPG) cómo se muestra la ventana: con marco, sin bordes a la resolución del monitor (sin ser pantalla completa) o
// pantalla completa exclusiva a la resolución elegida (runtime/ui/menu.mm apply_window_mode)
enum { WIN_WINDOW = 0, WIN_BORDERLESS = 1, WIN_FULLSCREEN = 2 };
enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12,
       // (SMRPG) resoluciones con nombre (alto en líneas; 1080p, 2160p y 4320p son 1x, 2x y 4x) y más porcentajes
       RES_H720 = 13, RES_H900 = 14, RES_H1440 = 15, RES_H1800 = 16, RES_H2880 = 17, RES_Q67 = 18, RES_Q85 = 19 };
// (SMRPG) el alto de una resolución con nombre o de 1x..8x (0: las que dependen de la ventana)
static inline int res_mode_height(int m) {
    switch (m) {
    case RES_H720: return 720; case RES_H900: return 900; case RES_H1440: return 1440;
    case RES_H1800: return 1800; case RES_H2880: return 2880;
    default: return m >= 1 && m <= 8 ? 1080 * m : 0;
    }
}
// (11 was 67% of the display: larecomp.ini "67%" now loads as RES_Q75)
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* Windows: AMD FSR 1 */,
       UPSCALE_GSR1 = 3 /* (SMRPG Android) Snapdragon GSR 1, en cualquier GPU con Vulkan */ };
#ifdef __APPLE__
#define UPSCALE_SHARP UPSCALE_METALFX  // each OS's non-bilinear upscaler (the default)
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1, AA_FSR = 2,  // (SMRPG: AA_FSR, AMD FSR temporal; vulkan/vk_fsr.cpp)
       AA_GSR2 = 3 };  // (SMRPG Android) Snapdragon GSR 2, temporal, con sombreadores propios (vulkan/exec.c gsr2_dispatch)
enum { PROMPTS_AUTO = 0, PROMPTS_NINTENDO = 1, PROMPTS_XBOX = 2, PROMPTS_PLAYSTATION = 3 };
// Which face button confirms in our own menus (runtime/ui/pad_nav.mm); the game's input is never affected
enum { MENU_CONFIRM_GAME = 0, MENU_CONFIRM_CONTROLLER = 1 };
enum { TIMER_POS_LEFT = 0, TIMER_POS_RIGHT = 1 };  // the run timer's corner (bottom left / right)
enum { CHAOS_LETTERS_OFF = 0, CHAOS_LETTERS_EXPLORED = 1, CHAOS_LETTERS_MAP = 2 /* explored, all with the Map */ };
// System language served to the game (runtime/services/am.c GetDesiredLanguage, services/hid_set.c GetLanguageCode):
// the game picks its text language from it, as it would from the Switch's own system setting. LANG_AUTO follows the
// host locale ($LC_ALL / $LC_MESSAGES / $LANG); the rest are the codes of the languages the game has text for.
enum { LANG_AUTO = 0, N_LANGUAGES = 12 };
extern const char *const g_language_codes[N_LANGUAGES];  // what the game is told: "ja", "en-US", "es", ...
extern const char *const g_language_names[N_LANGUAGES];  // for the menu ("Auto", "Japanese", ..., "Spanish")

typedef struct {
    int aspect;          // ASPECT_*: unlocked = fill the window (wider/taller views), original = 16:9 with bars
    bool fullscreen;     // (lo mismo que window_mode != WIN_WINDOW; se guarda por compatibilidad)
    int window_mode;     // (SMRPG) WIN_*
    int out_w, out_h;    // (SMRPG) resolución de pantalla: tamaño de la ventana o modo de pantalla completa; 0 automática
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    bool res_auto;       // never chosen: the default for this GPU (settings_resolve_defaults); saved as "auto"
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (MetalFX: macOS, FSR 1: Windows)
    bool upscaler_auto;  // never chosen: the default for this GPU (settings_resolve_defaults); saved as "auto"
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    int supersample;     // (SMRPG) 1 (no), 2, 4, 8, 16: se dibuja con raíz(N) veces más líneas y se reduce a la ventana
    int sharpness;       // (SMRPG) 0 (no) .. 10: AMD RCAS a la resolución final (con FSR 1, 0 = su valor por defecto)
    int aniso;           // (SMRPG) filtrado anisótropo forzado: 0 = el del juego, 2, 4, 8, 16
    int fps_limit;       // (SMRPG) imágenes por segundo: el ritmo del vsync que espera Unity (60 = la consola)
    bool match_refresh;  // (SMRPG Android) la frecuencia de la pantalla, la del ritmo del juego (60 Hz con 60 fps: más suave);
                         // si no, la del sistema (en la Thor 120 Hz: menos retardo). vulkan/vk_pacing.c
    int tex_detail;      // (SMRPG) detalle de texturas: 0 normal, 1 alto (sesgo de LOD -0.5), 2 máximo (-1)
    int shader_wait;     // (SMRPG) 0 los dibujos con shaders nuevos se saltan, 1 esperan, 2 esperan hasta 10 ms por fotograma
                         // (exec.c draw_waits_for_build)
    int fsr_mode;        // (SMRPG) 0 nativo (antialiasing), 1 calidad (x1,5), 2 equilibrado (x1,7), 3 rendimiento (x2), 4 ultra (x3)
    int fsr_version;     // (SMRPG) 0 automática (la mejor que corra la tarjeta), 4, 3 (3.1) o 2 (2.3): vulkan/vk_fsr.cpp
    bool framegen;       // (SMRPG) generación de fotogramas de AMD FSR (con AMD FSR de antialiasing): vulkan/vk_fg.cpp
    int fg_version;      // (SMRPG) su versión: 0 automática (FG 4 en las RX 9000, 3.1 en las demás), 4 o 3 (3.1)
    int lsfg;            // (SMRPG) generación de fotogramas de Lossless Scaling (vulkan/vk_lsfg.c): 0 no, 2, 3 o 4 (multiplicador)
    int lsfg_flow;       // (SMRPG) su «escala del flujo», en %: 25 .. 100
    bool lsfg_perf;      // (SMRPG) su modo rendimiento (LSFG 3.1 «performance»)
    bool tex_pack;       // (SMRPG) texturas HD: las de <datos>/texturas sustituyen a las del juego (vulkan/texpack.c)
    bool tex_dump;       // (SMRPG) volcar cada textura nueva del juego a <datos>/texturas-volcadas en PNG
    bool hdr;            // (SMRPG) salida HDR10 si la pantalla la ofrece (vulkan/device.c, exec.c vk_hdr_encode)
    int hdr_paper;       // (SMRPG) nits del blanco SDR (80..400)
    int hdr_peak;        // (SMRPG) nits del pico al que se realzan las luces (400..2000)
    int hdr_boost;       // (SMRPG) realce de las luces, 0..10
    bool true_black;     // (SMRPG) negro puro: el negro del etalonaje del juego (su LUT) pasa a 0 (vulkan/exec.c lut_black_fix)
    int movie_fit;       // (SMRPG) vídeos en pantalla ancha: 0 bandas negras, 1 llenar la pantalla (hle/movie_fit.c)
    float volume;        // master volume, 1 = 100%
    bool reverb;         // room reverb effect
    bool movie_audio;
    bool show_fps;
    bool dpad_move;      // the d-pad (and arrow keys) move Link like the stick
    bool rumble;         // controller vibration from the game (runtime/hle/input.c)
    int rumble_strength; // its intensity, percent (25 / 50 / 75 / 100)
    bool controller_light; // the controller's light bar shows Link's health (runtime/led.h, hle/input.c)
    bool bg_input;       // controllers keep working while the window isn't in front (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS)
    bool move_360;       // analog movement in any direction (the game snaps the stick to 8): runtime/hle/qol.c
    bool blur_removal;   // no tilt-shift blur at the top / bottom of the screen
    bool hide_hero_icon; // no Hero Mode icon on the HUD
    bool mirror_mode;    // the world mirrored left to right (Gameplay tab): renderers + runtime/hle/input.c
    int language;        // LANG_AUTO or an index into g_language_codes: the game's text language
    int prompts;         // PROMPTS_*: button glyphs (auto = follow the controller SDL reports): runtime/nvn/texpatch.c
    int menu_confirm;    // MENU_CONFIRM_*: game layout (right confirms, bottom goes back) or the controller's convention
    bool pad_swap_ab;    // (SMRPG) A<->B en el mando: se aplica a sus asignaciones (hle/bindings.c bind_swap)
    bool pad_swap_xy;    // (SMRPG) X<->Y en el mando, igual
    bool inv_keys;       // randomizer: show every dungeon's keys on the pause menu's Items page
    bool tracker;        // randomizer: check tracker on the pause menu's Map page
    int tracker_filters; // its filter bar (runtime/ui/tracker_overlay.mm bits), -1 = the defaults; no menu row
    int chaos_letters;   // CHAOS_LETTERS_*: room-shuffle doorway letters on the dungeon map
    bool aspect_asked;   // the start screen's one-time "authentic 16:9 or unlocked?" question was answered
    bool menu_tip_seen;  // the start screen's one-time "how to open the menu" note was shown (first PLAY ever)
    bool onedrive_kept;  // the start screen's OneDrive warning (Windows) was answered with Keep here
    // cheats (runtime/ui/cheats.mm), applied every frame while a save is loaded
    bool cheat_hearts;   // health kept full
    bool cheat_ammo;     // bombs / arrows / magic powder kept full (for the bags Link owns)
    bool cheat_rupees;   // rupees kept at 9999
    bool cheat_hookshot; // hookshot latches onto any surface (Gameplay tab; runtime/hle/hookshot.c; seeds made with it: always)
    bool no_buff_drops;  // no Guardian Acorns / Pieces of Power (Gameplay tab; runtime/hle/qol.c; seeds made with it: always)
    // difficulty (runtime/ui/difficulty.mm)
    float diff_damage;   // damage taken multiplier (1 = normal)
    float walk_speed;    // Link's walking speed multiplier (1 = normal; Gameplay tab, runtime/hle/qol.c)
    bool walk_speed_dungeons;  // walk speed also indoors: houses, caves, dungeons (Gameplay tab, under Walk speed)
    bool diff_ohko;      // any hit kills
    bool diff_hero;      // Hero Mode flag forced on in the loaded save (2x damage, no recovery hearts)
    // cosmetics (Cosmetics tab, runtime/ui/menu.mm), live, never part of a seed: colour rows of the shared palette
    // (runtime/cosmetic_colour.h; Default = the game's own, for the tunic also the seed's)
    CosColour tunic, hearts;  // runtime/nvn/texpatch.c (tunic map, heart tints)
    CosColour hair, shield;   // runtime/nvn/recolour_link.c
    CosColour skin;           // runtime/nvn/texpatch.c skin_apply
    CosColour leaves, grass;  // the overworld's foliage, runtime/nvn/recolour_seasons.c
    CosColour sand;           // the beaches, the desert, the sandy south (recolour_seasons.c)
    int enemy_colours;        // 0 Off, 1 Random per seed, 2 Random every load (runtime/nvn/recolour_enemies.c)
    int screen_filter;        // 0 Off, 1 Game Boy, 2 Game Boy Color, 3 Grayscale (runtime/nvn/screen_filter.h)
    // speedrun (Gameplay tab; runtime/hle/run_timer.c, runtime/livesplit.c)
    bool timer;            // the in-game run timer in the corner (game time: level loads don't count)
    int timer_pos;         // TIMER_POS_*: which bottom corner it sits in
    bool livesplit;        // drive LiveSplit (its Server component) over TCP: start, splits, game time
    char livesplit_host[64];  // host:port of the LiveSplit Server ("localhost:16834")
    char rando_seed[128];  // active randomizer seed (rando/<name>/), empty = vanilla
    char rando_options[48];  // the Randomizer tab's New seed options: a seed ID without its random part (empty = defaults)
} Settings;


enum { N_ENEMY_COLOUR_MODES = 3, N_SCREEN_FILTERS = 4 };
extern const char *const g_screen_filters[N_SCREEN_FILTERS];  // (the menu's names)
extern const char *const g_enemy_colour_modes[N_ENEMY_COLOUR_MODES];  // (the menu's names)

extern Settings g_settings;

void settings_load(void);
void settings_save(void);
extern Settings g_settings_defaults;  // (SMRPG) los valores por defecto (antes de leer larecomp.ini)
extern Settings g_settings_boot;      // (SMRPG) los de este arranque (lo que pide volver a abrir el juego)
void settings_reset_video(void);      // (SMRPG) los ajustes de Vídeo a sus valores por defecto (no el idioma)
void settings_reset_audio(void);      // (SMRPG) los de Audio
// At device init: the defaults that depend on the GPU. integrated (Vulkan: an integrated GPU; never on macOS): plain
// bilinear upscaling instead of the sharp upscaler, whose pass at the window's size costs as much as the game's frame
// there (UHD 770: FSR 1 + SMAA 8 ms of GPU per frame at 1080p, 20 ms at 4K), and Dynamic resolution instead of Match
// display. A player's own choice is kept. Anti-aliasing is off by default everywhere. LARECOMP_INTEGRATED=0/1 (test):
// treat the GPU as dedicated / integrated.
void settings_resolve_defaults(bool integrated);
extern int g_default_upscaler;  // UPSCALE_*: what the menu shows as the default
extern int g_default_res_mode;  // RES_*: likewise
// An integrated GPU (as settings_resolve_defaults was told): the fixed 1x renders 1280x720 there, what the game itself
// picked on such a GPU (runtime/nvn/output_scale.c)
extern bool g_gpu_integrated;
// The language code to serve the game ("es", "en-US", ...): the setting, or the host locale for LANG_AUTO
const char *settings_language(void);
// That code's name for the menu ("Spanish")
const char *settings_language_name(void);

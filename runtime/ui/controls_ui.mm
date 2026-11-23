// The settings' Controls tab (in game: menu.mm; start screen: launcher.mm, both through menu_tabs_draw), in option rows
// (runtime/ui/option_rows.h), and its Configure inputs page for the bindings (runtime/input_bind.h): DEVICE and the
// categories on the left, the chosen category's bindings on the right. A on a category moves the cursor to its
// bindings; A on a binding waits for the new button / key (Esc or 5 s without a press cancels; a button already in
// use swaps over); B goes back a step (bindings -> categories -> the tab). Drawn in whichever ImGui context is current.
#include "../app_name.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"  // (SetFocusID, NavCursorVisible)
#include "option_rows.h"

extern "C" bool g_game_las;  // runtime/game_addresses.c (false en SMRPG)

extern "C" {
#include "../input_bind.h"
#include "../settings.h"
int input_pad_family(void);              // runtime/hle/input.c
SDL_JoystickID input_active_pad(void);   // runtime/hle/input.c: the controller the game reads (0: none yet)
void input_apply_background(void);
}

// ------------------------------------------------------------------ Configure inputs: what's on it
// (SMRPG: en español, y cada botón con lo que hace en Super Mario RPG, como en la guía de la pestaña: guide_rows)
static const char *T(const char *en, const char *es) { return g_game_las ? en : es; }
enum { CAT_BUTTONS, CAT_TRIGGERS, CAT_STICKS, CAT_DPAD, N_CAT };
static const char *cat_name(int c) {
    static const char *const en[N_CAT] = {"Buttons", "Triggers", "Sticks", "D-pad"};
    static const char *const es[N_CAT] = {"Botones", "Gatillos", "Sticks", "Cruceta"};
    return (g_game_las ? en : es)[c];
}
static const char *const k_cat_hook[N_CAT] = {"buttons", "triggers", "sticks", "dpad"};  // (LARECOMP_CONTROLS_SUB)

struct Bind { int slot; const char *label; const char *section; };  // section: a header before this row
static int cat_rows(int cat, bool pad, Bind *out) {
    int n = 0;
    auto add = [&](int slot, const char *label, const char *section = NULL) { out[n++] = {slot, label, section}; };
    switch (cat) {
    case CAT_BUTTONS:
        add(BIND_A, T("A", "A · hablar / aceptar")), add(BIND_B, T("B", "B · saltar / cancelar")),
            add(BIND_X, T("X", "X · objetos")), add(BIND_Y, T("Y", "Y · especiales")), add(BIND_PLUS, T("Plus", "Botón + · menú")),
            add(BIND_MINUS, T("Minus", "Botón - · ataque de la barra"));
        break;
    case CAT_TRIGGERS:
        add(BIND_L, T("L", "L · caminar")), add(BIND_R, T("R", "R · caminar")), add(BIND_ZL, T("ZL", "ZL · caminar")),
            add(BIND_ZR, T("ZR", "ZR · caminar"));
        break;
    case CAT_STICKS: {  // (a controller's sticks stay its sticks: only their presses are buttons)
        const char *press = T("Press", "Clic"), *up = T("Up", "Arriba"), *down = T("Down", "Abajo"), *left = T("Left", "Izquierda"),
                   *right = T("Right", "Derecha");
        add(BIND_LSTICK, press, T("Left stick", "Stick izquierdo (moverse)"));
        if (!pad) add(BIND_LS_UP, up), add(BIND_LS_DOWN, down), add(BIND_LS_LEFT, left), add(BIND_LS_RIGHT, right);
        add(BIND_RSTICK, press, T("Right stick", "Stick derecho"));
        if (!pad) add(BIND_RS_UP, up), add(BIND_RS_DOWN, down), add(BIND_RS_LEFT, left), add(BIND_RS_RIGHT, right);
        break;
    }
    case CAT_DPAD:
        add(BIND_UP, T("Up", "Arriba")), add(BIND_DOWN, T("Down", "Abajo")), add(BIND_LEFT, T("Left", "Izquierda")),
            add(BIND_RIGHT, T("Right", "Derecha"));
        break;
    }
    return n;
}

// (SMRPG) Los nombres en español: de las teclas (SDL las da en inglés), de los botones del mando y de las entradas de la
// Switch. Solo caracteres de Latin-1: la fuente del menú no tiene más (menu.mm, sus rangos por defecto).
static const char *key_name(int sc) {
    const char *n = bind_key_label(sc);
    if (g_game_las) return n;
    if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) return "(ninguna)";
    static const struct { int sc; const char *es; } k[] = {
        {SDL_SCANCODE_RETURN, "Intro"},         {SDL_SCANCODE_BACKSPACE, "Retroceso"},  {SDL_SCANCODE_SPACE, "Espacio"},
        {SDL_SCANCODE_ESCAPE, "Esc"},           {SDL_SCANCODE_TAB, "Tab"},              {SDL_SCANCODE_LEFT, "Flecha izq."},
        {SDL_SCANCODE_RIGHT, "Flecha der."},    {SDL_SCANCODE_UP, "Flecha arriba"},     {SDL_SCANCODE_DOWN, "Flecha abajo"},
        {SDL_SCANCODE_LSHIFT, "Mayús izq."},    {SDL_SCANCODE_RSHIFT, "Mayús der."},    {SDL_SCANCODE_LCTRL, "Ctrl izq."},
        {SDL_SCANCODE_RCTRL, "Ctrl der."},      {SDL_SCANCODE_LALT, "Alt"},             {SDL_SCANCODE_RALT, "Alt Gr"},
        {SDL_SCANCODE_CAPSLOCK, "Bloq Mayús"},  {SDL_SCANCODE_DELETE, "Supr"},          {SDL_SCANCODE_INSERT, "Insert"},
        {SDL_SCANCODE_HOME, "Inicio"},          {SDL_SCANCODE_END, "Fin"},              {SDL_SCANCODE_PAGEUP, "Re Pág"},
        {SDL_SCANCODE_PAGEDOWN, "Av Pág"},      {SDL_SCANCODE_KP_ENTER, "Intro (num.)"},
    };
    for (auto &e : k)
        if (e.sc == sc) return e.es;
    if (!strncmp(n, "Keypad ", 7)) {  // "Keypad 8" -> "8 (num.)" (varios en una misma línea: unos cuantos búferes)
        static char buf[8][32];
        static int bi;
        char *b = buf[bi++ & 7];
        snprintf(b, sizeof buf[0], "%s (num.)", n + 7);
        return b;
    }
    return n;
}
static const char *pad_name(int host, int fam) {
    const char *n = bind_pad_label(host, fam);
    if (g_game_las) return n;
    static const char *const tr[][2] = {
        {"(none)", "(ninguno)"},
        {"Minus", "Botón -"},
        {"Plus", "Botón +"},
        {"Left stick press", "Clic del stick izq."},
        {"Right stick press", "Clic del stick der."},
        {"D-pad Up", "Cruceta arriba"},
        {"D-pad Down", "Cruceta abajo"},
        {"D-pad Left", "Cruceta izq."},
        {"D-pad Right", "Cruceta der."},
        {"Capture", "Captura"},
        {"Right paddle 1", "Pala der. 1"},
        {"Left paddle 1", "Pala izq. 1"},
        {"Right paddle 2", "Pala der. 2"},
        {"Left paddle 2", "Pala izq. 2"},
        {"Touchpad", "Panel táctil"},
        {"View", "Vista"},
        {"Menu", "Menú"},
        {"LS (press)", "LS (clic)"},
        {"RS (press)", "RS (clic)"},
        {"Share", "Compartir"},
        {"Cross", "Cruz"},
        {"Circle", "Círculo"},
        {"Square", "Cuadrado"},
        {"Triangle", "Triángulo"},
        {"Create", "Crear"},
        {"Mute", "Silencio"},
        {"Right back button", "Botón trasero der."},
        {"Left back button", "Botón trasero izq."},
        {"Right Fn", "Fn der."},
        {"Left Fn", "Fn izq."},
    };
    for (auto &e : tr)
        if (!strcmp(n, e[0])) return e[1];
    return n;
}
static const char *input_name(int slot) {  // (la entrada de la Switch: "A", "Cruceta arriba"...)
    const char *n = bind_input_name(slot);
    if (g_game_las || slot < 0 || slot >= BIND_KEY_COUNT) return n;
    static const char *const es[BIND_KEY_COUNT] = {
        "A", "B", "X", "Y", "Clic del stick izquierdo", "Clic del stick derecho", "L", "R", "ZL", "ZR", "Botón +", "Botón -",
        "Cruceta izquierda", "Cruceta arriba", "Cruceta derecha", "Cruceta abajo", "Stick izquierdo arriba",
        "Stick izquierdo abajo", "Stick izquierdo izquierda", "Stick izquierdo derecha", "Stick derecho arriba",
        "Stick derecho abajo", "Stick derecho izquierda", "Stick derecho derecha",
    };
    return es[slot];
}
static int slot_cat(int slot) {
    Bind b[16];
    for (int c = 0; c < N_CAT; c++)
        for (int i = 0, n = cat_rows(c, false, b); i < n; i++)
            if (b[i].slot == slot) return c;
    return CAT_BUTTONS;
}

// Devices: each connected controller by name (they share one set of bindings; the name picks the button names), or
// "Controller" when none is connected (bindings for one connected later), then the keyboard.
struct Device { SDL_JoystickID id; char name[64]; int fam; bool pad; };
static int pad_fam(SDL_JoystickID id) {
    if (!id || getenv("LARECOMP_PAD_TYPE")) return bind_label_family();  // (none / a test pretending one)
    return input_family_of_type(SDL_GetGamepadTypeForID(id));
}
static int list_devices(Device *d, int cap) {
    int n = 0, np = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&np);
    for (int i = 0; i < np && n < cap - 1; i++) {
        const char *nm = SDL_GetGamepadNameForID(ids[i]);
        d[n].id = ids[i], d[n].pad = true, d[n].fam = pad_fam(ids[i]);
        snprintf(d[n].name, sizeof d[n].name, "%s", nm && *nm ? nm : T("Controller", "Mando"));
        if (strlen(d[n].name) > 30) strcpy(d[n].name + 27, "...");  // (fits beside DEVICE)
        n++;
    }
    SDL_free(ids);
    if (!n) d[n].id = 0, d[n].pad = true, d[n].fam = pad_fam(0), snprintf(d[n].name, sizeof d[n].name, "%s", T("Controller", "Mando")), n++;
    d[n].id = 0, d[n].pad = false, d[n].fam = 0, snprintf(d[n].name, sizeof d[n].name, "%s", T("Keyboard", "Teclado")), n++;
    return n;
}

// ------------------------------------------------------------------ state
static bool g_sub;                 // on the Configure inputs page
static int g_cat = CAT_BUTTONS;    // the category shown on the right
static bool g_dev_pad = true;      // the device chosen: a controller (g_dev_id) or the keyboard
static SDL_JoystickID g_dev_id;
static bool g_in_right;            // the cursor is on a binding (last frame)
static int g_last_frame = -10;     // (a frame without this tab: the page starts over)
static int g_back_frame = -10;     // the frame B / Esc went back a step here (the menu mustn't close on it)
enum { FOCUS_NONE, FOCUS_CONFIGURE, FOCUS_DEVICE, FOCUS_CAT, FOCUS_RIGHT };
static int g_focus;                // put the cursor on this row when it's drawn

// B / Esc this frame went back a step on the Configure inputs page (menu.mm / launcher.mm: don't close too)
extern "C" bool controls_ui_took_back(void) { return g_back_frame == ImGui::GetFrameCount(); }

static void take_focus(int want) {  // (right after the row: it's the last item)
    if (g_focus != want) return;
    g_focus = FOCUS_NONE;
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
    ImGui::SetScrollHereY(0.3f);
}

// The device the page opens on: the keyboard when it was opened with one (Enter / Space), else the controller the game
// reads (or the first connected; "Controller" when there's none)
static void pick_device(bool keyboard) {
    SDL_JoystickID act = input_active_pad();
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (!act && n > 0) act = ids[0];
    SDL_free(ids);
    g_dev_pad = !keyboard, g_dev_id = act;
}
static bool keyboard_pressed(void) {
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
           ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
}

// A finished capture keeps larecomp.ini current; a swap is said under the list (another row changed too)
static void watch_results(int fam, char *msg, size_t cap) {
    static int seen = -1;
    int rdev, rslot, rother, serial, kind = bind_last_result(&rdev, &rslot, &rother, &serial);
    if (seen < 0) seen = serial;
    if (serial == seen) return;
    seen = serial;
    msg[0] = 0;
    if (kind == 2)
        snprintf(msg, cap, T("%s moved to %s (swapped).", "%s pasó a %s (intercambiados)."), input_name(rother),
                 rdev == BIND_DEV_PAD ? pad_name(g_bind_pad[rother], fam) : key_name(g_bind_key[rother]));
    if (kind == 1 || kind == 2 || kind == 5) settings_save();
}

static bool nav_on_item(void) { return ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible; }

static void configure_inputs(float scale) {
    Device devs[9];
    int nd = list_devices(devs, 9), di = -1;
    for (int i = 0; i < nd; i++)
        if (devs[i].pad == g_dev_pad && (!g_dev_pad || devs[i].id == g_dev_id)) di = i;
    if (di < 0) di = g_dev_pad ? 0 : nd - 1;  // (that controller is gone: the first one)
    g_dev_pad = devs[di].pad, g_dev_id = devs[di].id;
    const int dev = g_dev_pad ? BIND_DEV_PAD : BIND_DEV_KEY, fam = devs[di].fam;

    int cd, cs, ms;
    bind_capture_active(&cd, &cs, &ms);
    static char msg[160];
    watch_results(fam, msg, sizeof msg);

    // B / Esc: from the bindings back to their category, from the categories back to the tab (a capture takes Esc and
    // every controller button itself: runtime/hle/bindings.c)
    if (cd < 0 && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        g_back_frame = ImGui::GetFrameCount();
        if (g_in_right) g_focus = FOCUS_CAT;
        else { g_sub = false, g_focus = FOCUS_CONFIGURE, msg[0] = 0; return; }  // (the tab is drawn instead)
    }
    // Left on a binding: back to its category too
    if (g_in_right && cd < 0 && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
                                 ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)))
        g_focus = FOCUS_CAT;

    opt_begin("##configure", scale);
    opt_section(T("Configure inputs", "Configurar controles"));
    const char *names[9];
    for (int i = 0; i < nd; i++) names[i] = devs[i].name;
    int ndi = di;
    if (opt_choice(T("Device", "Dispositivo"), &ndi, names, nd, di, NULL)) {
        bind_capture_cancel();
        g_dev_pad = devs[ndi].pad, g_dev_id = devs[ndi].id, msg[0] = 0;
    }
    take_focus(FOCUS_DEVICE);
    for (int c = 0; c < N_CAT; c++) {
        if (g_in_right && c == g_cat) opt_mark_next();
        bool open = opt_link(cat_name(c), NULL);
        if (c == g_cat) take_focus(FOCUS_CAT);
        if (nav_on_item() && g_cat != c) g_cat = c, msg[0] = 0;  // (moving down the list shows each)
        if (open) { if (g_cat != c) msg[0] = 0; g_cat = c, g_focus = FOCUS_RIGHT; bind_capture_cancel(); }
    }
    if (bind_is_default(dev)) opt_disable_next();
    if (opt_action(T("Restore default controls", "Restaurar los controles"), "",
                   g_dev_pad ? T("Puts every controller button back to its default.", "Vuelve a poner cada botón del mando como al principio.")
                             : T("Puts every key back to its default.", "Vuelve a poner cada tecla como al principio."))) {
        bind_capture_cancel();
        bind_reset(dev);
    }
    // (the cursor or the mouse on a row with a description: it shows on the right instead of the bindings)
    bool desc_right = (ImGui::IsItemHovered() || nav_on_item()) && g_focus != FOCUS_RIGHT;
    opt_split();
    if (desc_right) {
        opt_desc_text();
        g_in_right = false;
    } else {
        Bind rows[16];
        int n = cat_rows(g_cat, g_dev_pad, rows);
        opt_section(cat_name(g_cat));
        bool in_right = false;
        for (int i = 0; i < n; i++) {
            const Bind &b = rows[i];
            if (b.section) opt_section(b.section);
            bool waiting = cd == dev && cs == b.slot;
            char val[64];
            if (waiting)
                snprintf(val, sizeof val, "%s...  %d", g_dev_pad ? T("Press a button", "Pulsa un botón") : T("Press a key", "Pulsa una tecla"),
                         (ms + 999) / 1000);
            else snprintf(val, sizeof val, "%s", g_dev_pad ? pad_name(g_bind_pad[b.slot], fam) : key_name(g_bind_key[b.slot]));
            // the same button / key on two inputs (a hand-edited larecomp.ini): red
            const int16_t *t = g_dev_pad ? g_bind_pad : g_bind_key;
            bool dup = false;
            for (int j = 0, m = g_dev_pad ? BIND_PAD_COUNT : BIND_KEY_COUNT; j < m; j++)
                dup |= j != b.slot && t[j] == t[b.slot] && t[b.slot] > (g_dev_pad ? -1 : 0);
            if (dup && !waiting) opt_warn_next();
            ImGui::PushID(b.slot);
            bool hit = opt_action(b.label, val, NULL, true, waiting || !bind_is_default_slot(dev, b.slot));
            ImGui::PopID();
            if (i == 0) take_focus(FOCUS_RIGHT);
            if (ImGui::IsItemFocused()) in_right = true;
            if (waiting && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);  // (a capture started by a test hook)
            if (hit) {
                if (waiting) bind_capture_cancel();
                else { msg[0] = 0; bind_capture_begin(dev, b.slot); }
            }
        }
        g_in_right = in_right;
        if (cd == dev) ImGui::TextDisabled("%s", T("Esc cancels.", "Esc cancela."));
        else if (msg[0]) ImGui::TextDisabled("%s", msg);
    }
    opt_split_end();
}

// ------------------------------------------------------------------ (SMRPG) la guía: qué hace cada botón
// Lo que hace cada botón de la Switch en Super Mario RPG, con la tecla y el botón del mando que lo dan ahora (las
// asignaciones de quien juega: cambian con ellas). Consultado en la sección de controles de Super Mario Wiki («Super Mario
// RPG (Nintendo Switch)», leída el 2026-11-16) y de la propia interfaz del juego (en el campo: X objetos, Y especiales,
// + menú). Lo que no dice ninguna de las dos (la cruceta, el stick derecho, sus clics) no se cuenta. A / clic en una
// fila: su entrada en «Configurar controles», para cambiarla.
static void guide_list(char *out, size_t cap, const int *slots, int n, bool pad, int fam) {
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < n && k < cap; i++) {
        int s = slots[i];
        if (pad ? g_bind_pad[s] < 0 : g_bind_key[s] <= 0) continue;  // (sin asignar)
        k += snprintf(out + k, cap - k, "%s%s", k ? " " : "", pad ? pad_name(g_bind_pad[s], fam) : key_name(g_bind_key[s]));
    }
    if (!out[0]) snprintf(out, cap, "(sin asignar)");
}
static void guide_rows(void) {
    struct Row { const char *label; int slots[4]; int n, cat; const char *desc; };
    static const Row rows[] = {
        {"Moverse", {BIND_LS_UP, BIND_LS_LEFT, BIND_LS_DOWN, BIND_LS_RIGHT}, 4, CAT_STICKS,
         "Mario corre al moverse; manteniendo L, R, ZL o ZR, camina (el menú Sistema del juego puede invertirlo). En "
         "combate, elige la acción y el objetivo."},
        {"Hablar / aceptar", {BIND_A}, 1, CAT_BUTTONS, "Hablar, examinar y aceptar. En combate: atacar."},
        {"Saltar / cancelar", {BIND_B}, 1, CAT_BUTTONS,
         "Saltar y cancelar. En combate: las demás opciones (como defenderse o huir)."},
        {"Objetos", {BIND_X}, 1, CAT_BUTTONS, "El menú de objetos, también en combate."},
        {"Especiales", {BIND_Y}, 1, CAT_BUTTONS, "El menú de especiales, también en combate. Montado en Yoshi: saltar."},
        {"Menú del juego", {BIND_PLUS}, 1, CAT_BUTTONS, "El menú del juego."},
        {"Ataque de la barra", {BIND_MINUS}, 1, CAT_BUTTONS,
         "En combate, con la barra de acción llena: su ataque (con tres compañeros, el ataque triple). En el castillo de "
         "Bowser: abandonar una prueba de las puertas."},
        {"Caminar (mantener)", {BIND_L, BIND_R, BIND_ZL, BIND_ZR}, 4, CAT_TRIGGERS,
         "Mantener uno: caminar en vez de correr. En la lista de monstruos, L y R pasan de página."},
    };
    int fam = bind_label_family();
    for (const Row &r : rows) {
        char kb[96], pd[96], val[200];
        guide_list(kb, sizeof kb, r.slots, r.n, false, fam);
        if (r.cat == CAT_STICKS) snprintf(pd, sizeof pd, "Stick izq.");  // (los sticks del mando no se cambian)
        else guide_list(pd, sizeof pd, r.slots, r.n, true, fam);
        snprintf(val, sizeof val, "%s · %s", kb, pd);
        ImGui::PushID(r.label);
        bool hit = opt_action(r.label, val, r.desc);
        ImGui::PopID();
        if (hit) {
            pick_device(keyboard_pressed());
            g_sub = true, g_cat = r.cat, g_focus = FOCUS_RIGHT, g_in_right = false;
        }
    }
    opt_action("Menú de ajustes", "Esc / F1 · Home", "Este menú (vídeo, audio y controles). Con mando: Home, o Back + Start.");
    opt_action("Ventana / pantalla completa", "F11 / Alt+Intro",
               "Cambia entre la ventana y el último modo a pantalla completa (Vídeo > Modo de pantalla).");
}

// ------------------------------------------------------------------ the tab
extern "C" void controls_tab_draw(float scale) {
    int frame = ImGui::GetFrameCount();
    if (frame - g_last_frame > 1) g_sub = false, g_focus = FOCUS_NONE, g_in_right = false;  // (back on the tab)
    g_last_frame = frame;
    // (test) LARECOMP_CONTROLS_SUB=<buttons|triggers|sticks|dpad>[:right][:key]: on that page, the cursor on the category
    // (right: on its first binding; key: the keyboard's) - for a few frames, past the menu putting it on the tab bar
    static bool hook_done;
    static int hook_frames, hook_focus;
    if (!hook_done) {
        hook_done = true;
        if (const char *h = getenv("LARECOMP_CONTROLS_SUB")) {
            pick_device(strstr(h, ":key") != NULL);
            g_sub = true, hook_focus = strstr(h, "right") ? FOCUS_RIGHT : FOCUS_CAT, hook_frames = 10;
            for (int c = 0; c < N_CAT; c++) if (!strncmp(h, k_cat_hook[c], strlen(k_cat_hook[c]))) g_cat = c;
        }
    }
    if (hook_frames > 0) hook_frames--, g_focus = hook_focus;
    {   // a capture started elsewhere (LARECOMP_BIND_TEST): show it waiting
        int cd, cs;
        if (bind_capture_active(&cd, &cs, NULL) && (!g_sub || g_dev_pad != (cd == BIND_DEV_PAD) || g_cat != slot_cat(cs))) {
            if (!g_sub) pick_device(cd == BIND_DEV_KEY);
            g_sub = true, g_dev_pad = cd == BIND_DEV_PAD, g_cat = slot_cat(cs);
        }
    }
    if (g_sub) {
        configure_inputs(scale);
        if (g_sub) return;
    }

    opt_begin("##controls", scale);
    if (!g_game_las) {  // (SMRPG: en español y sin lo de Link's Awakening - mover a Link con la cruceta, 360 grados, la
                        // luz del mando con su salud, los iconos de botones que cambian sus texturas)
        opt_section("Teclas y botones");
        guide_rows();
        opt_section("Entradas");
        if (opt_link("Configurar controles", "Elige los botones y teclas del juego (con los nombres de la Switch: A es "
                                             "aceptar / hablar, B saltar / cancelar). El menú siempre se abre con F1, Esc, "
                                             "Home o Back + Start, así que ninguna asignación te deja sin él.")) {
            pick_device(keyboard_pressed());
            g_sub = true, g_focus = FOCUS_DEVICE, g_in_right = false;
        }
        take_focus(FOCUS_CONFIGURE);
        if (opt_toggle("Mandos en segundo plano", &g_settings.bg_input, false,
                       "Los mandos siguen funcionando aunque haya otra ventana delante."))
            input_apply_background();
        opt_section("Mando");
        if (opt_toggle("Invertir A-B", &g_settings.pad_swap_ab, false,
                       "Intercambia A con B en el mando. Sin invertir van por posición, como en la Switch: acepta el botón "
                       "de la derecha. Con un mando de letras de Xbox, invertidos, acepta su A y cancela su B, como dicen "
                       "sus letras. Cambia también sus botones en «Configurar controles».")) {
            bind_capture_cancel();
            bind_swap(BIND_A, BIND_B);
        }
        if (opt_toggle("Invertir X-Y", &g_settings.pad_swap_xy, false,
                       "Intercambia X con Y en el mando. Sin invertir van por posición, como en la Switch: X es el botón de "
                       "arriba. Con un mando de letras de Xbox, invertidos, su X abre los objetos y su Y los especiales, "
                       "como dicen sus letras. Cambia también sus botones en «Configurar controles».")) {
            bind_capture_cancel();
            bind_swap(BIND_X, BIND_Y);
        }
        opt_toggle("Vibración", &g_settings.rumble, true, "La vibración del mando cuando el juego la pide.");
        if (g_settings.rumble) {
            static const char *const pct[] = {"25 %", "50 %", "75 %", "100 %"};
            int i = g_settings.rumble_strength / 25 - 1;
            if (i < 0 || i > 3) i = 3;
            opt_indent(1);
            if (opt_choice("Intensidad", &i, pct, 4, 3, "La fuerza de la vibración.", false))
                g_settings.rumble_strength = (i + 1) * 25;
            opt_indent(-1);
        }
        {
            static const char *const names[] = {"Como en el juego", "Como en el mando"};
            opt_choice("Aceptar en los menús", &g_settings.menu_confirm, names, 2, MENU_CONFIRM_CONTROLLER,
                       "Qué botón acepta en los menús de " APP_NAME " (no en los del juego).");
        }
        opt_end();
        return;
    }
    opt_section("Inputs");
    if (opt_link("Configure inputs", "Choose the buttons and keys for the game. The menu always opens with F1, Esc, "
                                     "Home or Back + Start, so no binding can lock you out.")) {
        pick_device(keyboard_pressed());
        g_sub = true, g_focus = FOCUS_DEVICE, g_in_right = false;
    }
    take_focus(FOCUS_CONFIGURE);
    if (opt_toggle("Allow background inputs", &g_settings.bg_input, false,
                   "Controllers keep working while another window is in front. macOS may ask for Input Monitoring "
                   "permission."))
        input_apply_background();

    opt_section("Movement");
    opt_toggle("D-pad moves Link", &g_settings.dpad_move, false, "The d-pad moves Link like the control stick.");
    opt_toggle("360-degree movement", &g_settings.move_360, false,
               "Link walks in the exact direction of the stick instead of snapping to 8 directions.");

    opt_section("Controller");
    opt_toggle("Rumble", &g_settings.rumble, true, "Controller vibration.");
    if (g_settings.rumble) {
        static const char *const pct[] = {"25%", "50%", "75%", "100%"};
        int i = g_settings.rumble_strength / 25 - 1;
        if (i < 0 || i > 3) i = 3;
        opt_indent(1);
        if (opt_choice("Intensity", &i, pct, 4, 3, "How strong the vibration is.", false))
            g_settings.rumble_strength = (i + 1) * 25;
        opt_indent(-1);
    }
    opt_toggle("Controller light", &g_settings.controller_light, true, "The light bar shows Link's health.");
    {
        static const char *const fam[] = {"Nintendo", "Xbox", "PlayStation"};
        char autol[64];
        snprintf(autol, sizeof autol, "Auto (%s)", fam[input_pad_family() % 3]);
        const char *names[] = {autol, "Nintendo", "Xbox", "PlayStation"};
        opt_choice("Button icons", &g_settings.prompts, names, 4, PROMPTS_AUTO,
                   "The button icons the game shows. Auto: the connected controller's.");
    }
    {
        static const char *const names[] = {"Game layout", "Controller convention"};
        opt_choice("Confirm button", &g_settings.menu_confirm, names, 2, MENU_CONFIRM_CONTROLLER,
                   "Which button confirms in " APP_NAME "'s menus.");
    }

    opt_end();
}

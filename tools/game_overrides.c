// (SMRPG) Funciones del juego que el runtime sustituye dentro de lasgame.dll sin regenerar el código: este objeto se
// enlaza el primero, con --allow-multiple-definition (Makefile), así que su definición gana a la generada para todas
// las llamadas, también las de dentro del mismo archivo generado y la tabla de saltos indirectos.
#include "guest_addr.h"  // (SMRPG Android: GADDR, las direcciones del juego movidas)
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"

// runtime/hle/unity_screen.c (lo exporta smrpg-recomp.exe)
extern void rt_unity_screen_size(int32_t *w, int32_t *h);

// main 0x70006b7204 (UnityPlayer, Switch): la resolución de la pantalla de Unity. En el juego: 1280x720, o 1920x1080
// según el modo de funcionamiento (TV) y el de rendimiento, por un ajuste del reproductor (+0x1c del objeto de
// 0x7000631100). El runtime siempre está en modo TV: 1920x1080, ensanchado al aspecto de la pantalla de quien juega si
// el ajuste «Pantalla ancha» lo pide (rt_unity_screen_size). (x0, x1) = (int *ancho, int *alto).
void f_70006b7204(Cpu *c) {
    int32_t *w = (int32_t *)(uintptr_t)c->x[0], *h = (int32_t *)(uintptr_t)c->x[1];
    *w = 1920;
    *h = 1080;
    rt_unity_screen_size(w, h);
}

// main 0x700062f1d0 (UnityPlayer, el gestor de pantalla de Switch): SetResolution(ancho, alto). Lo mismo que el original
// salvo dos cosas, para que la pantalla del juego pueda seguir en marcha a la de quien juega (21:9, 16:10...):
//  - el original guarda el tamaño recortado al de la Switch (+0xc0 = min(ancho, 1920), +0xc4 = min(alto, 1080): lo que
//    leen Screen.width / height): pedido 2560x1080 en marcha, la escena seguía a 1920 de ancho y el runtime la estiraba;
//    aquí se guarda entero, como al arrancar (esa vía no recorta);
//  - el original marca +0xc9 («se aplicó una resolución propia») cualquier tamaño que no sea 1280x720 ni 1920x1080, y
//    con ella sus manejadores de cambio de modo ya no hacen nada; aquí no se marca.
// (y sin su aviso «Requested resolution ... is not a 16:9 aspect ratio»). Lo usa el manejador del aviso de cambio del
// modo de rendimiento (0x700062ed24, vtable +0x30), que runtime/hle/unity_screen.c provoca cuando la ventana cambia de
// aspecto.
void f_7000de69c0(Cpu *c);
void f_700057bef0(Cpu *c);
void f_7000237cf0(Cpu *c);
void f_7000256b4c(Cpu *c);
static uint32_t display_mode_now(Cpu *c) {  // (la entrada de modo de pantalla de ahora: +0x30 + índice +0x4c * 0xa8, +0x64)
    f_7000de69c0(c);
    uint64_t d = c->x[0];
    return LD32(LD64(d + 0x30) + (uint64_t)((int64_t)(int32_t)LD32(d + 0x4c) * 0xa8) + 0x64);
}
void f_700062f1d0(Cpu *c) {
    uint64_t self = c->x[0], ret = c->x[30];
    uint32_t w = (uint32_t)c->x[1], h = (uint32_t)c->x[2];
    uint32_t mode_before = LD32(self + 0xcc);
    c->x[30] = GADDR(0x700062f1fcull);
    uint32_t mode_new = display_mode_now(c);
    bool mode_changed = mode_before != mode_new, size_changed = true;
    c->x[0] = self, c->pc = LD64(LD64(self) + 0xb0), c->x[30] = GADDR(0x700062f228ull);  // (el ancho de ahora)
    rt_dispatch(c);
    if ((uint32_t)c->x[0] == w) {
        c->x[0] = self, c->pc = LD64(LD64(self) + 0xb8), c->x[30] = GADDR(0x700062f240ull);  // (el alto de ahora)
        rt_dispatch(c);
        size_changed = (uint32_t)c->x[0] != h;
        if (!mode_changed && !size_changed) {
            c->x[0] = 0, c->x[30] = ret;
            return;
        }
    }
    c->x[30] = GADDR(0x700062f2bcull);
    f_700057bef0(c);
    uint64_t g = c->x[0];
    c->x[0] = g, c->pc = LD64(LD64(g) + 0x6c0), c->x[30] = GADDR(0x700062f2c8ull);
    rt_dispatch(c);
    ST32(self + 0xc0, w);  // (sin el recorte a 1920x1080)
    ST32(self + 0xc4, h);
    c->x[30] = GADDR(0x700062f2ecull);
    ST32(self + 0xcc, display_mode_now(c));
    ST8(self + 0xc9, 0);  // (nunca «propia»)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ST8(self + 0xc8, size_changed);
    ST8(self + 0xd0, mode_changed);
    c->x[30] = GADDR(0x700062f344ull);
    f_7000237cf0(c);
    c->x[30] = GADDR(0x700062f348ull);
    f_7000256b4c(c);
    c->x[0] = 1, c->x[30] = ret;
    static int logged;
    if (logged++ < 8) fprintf(stderr, "[unity] SetResolution %ux%u (sin recorte)\n", w, h);
}

// ---------------------------------------------------------------- la interfaz en pantalla ancha
// main 0x7001fdb520 = UnityEngine.UI.CanvasScaler.HandleScaleWithScreenSize (uGUI de Unity 2021.3; nombres y
// direcciones del volcado de IL2CPP). Hace lo mismo que el original (el código abierto de uGUI) con una diferencia: el
// tamaño de la pantalla pasa antes por rt_unity_ui_scale_screen (runtime/hle/unity_screen.c), que en una pantalla más
// ancha (o más alta) que 16:9 da el 16:9 que cabe dentro. La rama de las pantallas secundarias (targetDisplay > 0) no
// está: la Switch tiene una.
//   campos (CanvasScaler): 0x1c m_ReferencePixelsPerUnit, 0x24 m_ReferenceResolution (x, y), 0x2c m_ScreenMatchMode
//   (0 ancho o alto, 1 expandir, 2 encoger), 0x30 m_MatchWidthOrHeight, 0x48 m_Canvas, 0x50 m_PrevScaleFactor,
//   0x54 m_PrevReferencePixelsPerUnit.
extern void rt_unity_ui_scale_screen(float *w, float *h);
void f_70022fb850(Cpu *c);  // Canvas.get_renderingDisplaySize: Vector2 en (s0, s1)
void f_70022fb0d0(Cpu *c);  // Canvas.set_scaleFactor(s0)
void f_70022fb170(Cpu *c);  // Canvas.set_referencePixelsPerUnit(s0)

static inline float sreg(Cpu *c, int n) {
    float f;
    memcpy(&f, &c->v[n], 4);
    return f;
}
static inline void set_sreg(Cpu *c, int n, float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    c->v[n] = b;
}
static inline float ldf(uint64_t a) {
    float f;
    memcpy(&f, (const void *)(uintptr_t)a, 4);
    return f;
}
static inline void stf(uint64_t a, float f) { memcpy((void *)(uintptr_t)a, &f, 4); }

// (diagnóstico, LARECOMP_LOG_CANVAS=1) el nombre del objeto de cada CanvasScaler la primera vez que se escala:
// UnityEngine.Object.get_name (main 0x700179fc20) devuelve un System.String (longitud en +0x10, UTF-16 en +0x14)
void f_700179fc20(Cpu *c);
static void log_canvas(Cpu *c, uint64_t self, float sw, float sh, float scale) {
    static int on = -1;
    static uint64_t seen[256];
    static int nseen;
    if (on < 0) on = getenv("LARECOMP_LOG_CANVAS") != NULL;
    if (!on || nseen >= 256) return;
    for (int i = 0; i < nseen; i++) if (seen[i] == self) return;
    seen[nseen++] = self;
    uint64_t lr = c->x[30];
    c->x[0] = self, c->x[1] = 0, c->x[30] = GADDR(0x7001fdb520ull);
    f_700179fc20(c);
    c->x[30] = lr;
    char name[96] = "?";
    uint64_t s = c->x[0];
    if (s) {
        int32_t n = (int32_t)LD32(s + 0x10), k = 0;
        for (int32_t i = 0; i < n && k < 95; i++) {
            uint16_t ch = *(const uint16_t *)(uintptr_t)(s + 0x14 + 2 * (uint64_t)i);
            name[k++] = ch < 128 ? (char)ch : '?';
        }
        name[k] = 0;
    }
    char line[300];  // (una sola escritura, como rt_log del runtime: la UCRT es la misma, stderr también)
    int len = snprintf(line, sizeof line, "[canvas] %s: ref %.0fx%.0f modo %d ajuste %.2f pantalla %.0fx%.0f escala %.3f\n",
                       name, ldf(self + 0x24), ldf(self + 0x28), (int)LD32(self + 0x2c), ldf(self + 0x30), sw, sh, scale);
    if (len > 0) fwrite(line, 1, (size_t)len < sizeof line ? (size_t)len : sizeof line - 1, stderr);
}

void f_7001fdb520(Cpu *c) {
    uint64_t self = c->x[0], lr = c->x[30];
    uint64_t canvas = LD64(self + 0x48);
    if (!canvas) return;  // (el original saltaría con NullReferenceException en el método del Canvas)
    c->x[0] = canvas, c->x[1] = 0, c->x[30] = GADDR(0x7001fdb568ull);
    f_70022fb850(c);
    float sw = sreg(c, 0), sh = sreg(c, 1);
    float ow = sw, oh = sh;
    rt_unity_ui_scale_screen(&sw, &sh);
    float rx = ldf(self + 0x24), ry = ldf(self + 0x28), scale = 0;
    switch ((int32_t)LD32(self + 0x2c)) {
    case 0: {  // Mathf.Log(x, 2) en doble precisión, como Math.Log; Mathf.Lerp con t entre 0 y 1; 2 elevado al resultado
        float lw = (float)(log((double)(sw / rx)) / log(2.0)), lh = (float)(log((double)(sh / ry)) / log(2.0));
        float t = ldf(self + 0x30);
        t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
        scale = exp2f(lw + (lh - lw) * t);
        break;
    }
    case 1: scale = fminf(sw / rx, sh / ry); break;
    case 2: scale = fmaxf(sw / rx, sh / ry); break;
    }
    log_canvas(c, self, ow, oh, scale);
    // SetScaleFactor y SetReferencePixelsPerUnit: solo si cambian
    if (scale != ldf(self + 0x50)) {
        c->x[0] = LD64(self + 0x48), c->x[1] = 0, c->x[30] = GADDR(0x7001fdb754ull);
        set_sreg(c, 0, scale);
        f_70022fb0d0(c);
        stf(self + 0x50, scale);
    }
    float ppu = ldf(self + 0x1c);
    if (ppu != ldf(self + 0x54)) {
        c->x[0] = LD64(self + 0x48), c->x[1] = 0, c->x[30] = GADDR(0x7001fdb778ull);
        set_sreg(c, 0, ppu);
        f_70022fb170(c);
        stf(self + 0x54, ppu);
    }
    c->x[30] = lr;
}

// ---------------------------------------------------------------- vectores de movimiento de URP
// main 0x7001838ee0 = UniversalRenderer.GetRenderPassInputs(ref RenderingData) (URP 12.1): qué piden las pasadas de la
// cola (profundidad, normales, color, movimiento). Con «movimiento», URP pone en cola su MotionVectorRenderPass, que
// dibuja _MotionVectorTexture (RG16F: el movimiento de la cámara, por la profundidad, y el de cada objeto, con sus
// matrices del fotograma anterior). El juego no lo pide nunca; el runtime lo fuerza (rt_force_motion_vectors) para los
// escaladores temporales, que lo necesitan, junto con la textura de profundidad de la que sale el de la cámara.
// Igual que el original (código abierto de URP) en todo lo demás. Campos: UniversalRenderer +0xf8
// m_ActiveRenderPassQueue (List: +0x10 elementos, +0x18 número), +0x410 m_RenderingMode (1 diferido);
// ScriptableRenderPass +0x10 renderPassEvent, +0xc0 m_Input (1 profundidad, 2 normales, 4 color, 8 movimiento).
// Devuelve RenderPassInputSummary (16 bytes) en x0 (seis booleanos, bytes 0-5) y x1 (dos RenderPassEvent).
extern int rt_force_motion_vectors(void);  // runtime/hle/unity_screen.c
void f_70000c4150(Cpu *c);  // il2cpp: inicializar los metadatos que usa un método (una vez)
void f_7001838ee0(Cpu *c) {
    uint64_t self = c->x[0];
    if (!(LD8(GADDR(0x7003cb4bc3ull)) & 1)) {  // (los mismos tres que el original, la primera vez)
        uint64_t lr = c->x[30];
        c->x[0] = LD64(GADDR(0x7003a547d0ull)), c->x[30] = GADDR(0x7001838f1cull);
        f_70000c4150(c);
        c->x[0] = LD64(GADDR(0x7003a56520ull)), c->x[30] = GADDR(0x7001838f28ull);
        f_70000c4150(c);
        c->x[0] = LD64(GADDR(0x7003a56528ull)), c->x[30] = GADDR(0x7001838f34ull);
        f_70000c4150(c);
        ST8(GADDR(0x7003cb4bc3ull), 1);
        c->x[30] = lr;
    }
    int32_t before_main = (int32_t)LD32(self + 0x410) == 1 ? 210 : 250;  // BeforeRenderingGbuffer / BeforeRenderingOpaques
    bool depth = false, prepass = false, normals = false, color = false, color_created = false, motion = false;
    int32_t normal_at = 250, earliest = 550;  // BeforeRenderingOpaques, BeforeRenderingPostProcessing
    uint64_t list = LD64(self + 0xf8), items = list ? LD64(list + 0x10) : 0;
    int32_t n = list ? (int32_t)LD32(list + 0x18) : 0;
    uint64_t dbuf = LD64(LD64(GADDR(0x7003a547d0ull)));  // (la clase DBufferRenderPass)
    for (int32_t i = 0; i < n && items; i++) {
        uint64_t pass = LD64(items + 0x20 + 8 * (uint64_t)i);
        if (!pass) continue;
        uint32_t in = LD32(pass + 0xc0);
        int32_t ev = (int32_t)LD32(pass + 0x10);
        bool nd = in & 1, nn = (in & 2) != 0, nc = (in & 4) != 0, nm = (in & 8) != 0;
        if (dbuf) {  // pass is DBufferRenderPass: la jerarquía de su clase (profundidad en +0x12c, tabla en +0xc8)
            uint64_t k = LD64(pass);
            uint8_t kd = LD8(k + 0x12c), dd = LD8(dbuf + 0x12c);
            if (dd && kd >= dd && LD64(LD64(k + 0xc8) + 8 * (uint64_t)dd - 8) == dbuf) color_created = true;
        }
        depth |= nd;
        prepass |= nn || (nd && ev <= before_main);
        normals |= nn;
        color |= nc;
        motion |= nm;
        if (nd && ev < earliest) earliest = ev;
        if ((nn || nd) && ev < normal_at) normal_at = ev;
    }
    if (rt_force_motion_vectors()) motion = depth = true;
    c->x[0] = (uint64_t)depth | (uint64_t)prepass << 8 | (uint64_t)normals << 16 | (uint64_t)color << 24 |
              (uint64_t)color_created << 32 | (uint64_t)motion << 40;
    c->x[1] = (uint64_t)(uint32_t)normal_at | (uint64_t)(uint32_t)earliest << 32;
}

// ---------------------------------------------------------------- las capturas de pantalla como fondo, en pantalla ancha
// Los menús del campo (Menu.StartScreenCapture, corrutina main 0x7001f4dd40) y los minijuegos (su ScreenCapture) copian
// la pantalla entera (Screen.width x Screen.height, ReadPixels: 2560x1080 con la pantalla ensanchada) y la ponen de fondo
// en una RawImage, directa o desenfocada por GaussianBlur (a la mitad de tamaño, el mismo aspecto). Esa RawImage mide
// 1920x1080 en el lienzo, el 16:9 de la consola: en pantalla ancha (el lienzo ocupa todo el ancho,
// rt_unity_ui_scale_screen) quedaba en el centro con la captura comprimida dentro y el fondo del lienzo a los lados
// («el juego se achica al abrir un menú»; medido: escala horizontal 0,75 con correlación 0,988, gris en los 320
// píxeles de cada lado de 2560x1080).
// main 0x7001e93320 = UnityEngine.UI.RawImage.set_texture: igual que el original (uGUI: si cambia, la guarda y marca
// vértices y material), y después, si la textura nueva tiene el aspecto de la pantalla del juego y la RawImage es un
// recuadro 16:9 a toda la altura del lienzo (>= 1000 unidades de las 1080), su ancho pasa al de la textura
// (RectTransform.SetSizeWithCurrentAnchors, centrado: crece por los dos lados). Con una pantalla más alta (16:10), su
// alto. Si luego recibe otra textura, vuelve a su medida.
// (DrawCaptureImage, otra captura con su propia cámara y un cuadro 16:9, no se ha visto en uso: solo la activa
// GS.TestFade, de pruebas, y algún FadeWithEffect de escena. Sin tocar.)
void f_700179f800(Cpu *c);  // UnityEngine.Object.op_Equality
void f_70000c4330(Cpu *c);  // il2cpp: el constructor estático de una clase (una vez)
void f_70000c4090(Cpu *c);  // il2cpp: la barrera de escritura del recolector (campo, valor)
void f_7001c459f0(Cpu *c);  // Graphic.get_rectTransform
void f_70017a5670(Cpu *c);  // RectTransform.get_rect -> (x, y, ancho, alto) en s0-s3
void f_70017a6f20(Cpu *c);  // RectTransform.SetSizeWithCurrentAnchors(Axis eje, float tamaño)
extern double rt_unity_screen_aspect(void);
static int tex_dim(Cpu *c, uint64_t tex, int slot) {  // Texture.width (vtable +0x188) / height (+0x1a8)
    uint64_t vt = LD64(tex);
    c->x[0] = tex, c->x[1] = LD64(vt + slot + 8), c->pc = LD64(vt + slot), c->x[30] = GADDR(0x7001e933f0ull);
    rt_dispatch(c);
    return (int32_t)c->x[0];
}
static void rect_size(Cpu *c, uint64_t rt, int axis, float v) {
    c->x[0] = rt, c->x[1] = (uint32_t)axis, c->x[2] = 0, c->x[30] = GADDR(0x7001e933f0ull);
    set_sreg(c, 0, v);
    f_70017a6f20(c);
}
static void capture_image_fit(Cpu *c, uint64_t img, uint64_t tex) {
    static struct { uint64_t img; float w0, h0, w1, h1; } fitted[32];
    static int nfit;
    double ga = rt_unity_screen_aspect(), a169 = 16.0 / 9.0;
    c->x[0] = img, c->x[1] = 0, c->x[30] = GADDR(0x7001e933f0ull);
    f_7001c459f0(c);
    uint64_t rt = c->x[0];
    if (!rt || !LD64(rt + 0x10)) return;
    c->x[0] = rt, c->x[1] = 0, c->x[30] = GADDR(0x7001e933f0ull);
    f_70017a5670(c);
    float rw = sreg(c, 2), rh = sreg(c, 3);
    int k = -1;
    for (int i = 0; i < nfit; i++) if (fitted[i].img == img) k = i;
    double ta = 0;
    if (tex && LD64(tex + 0x10)) {
        int tw = tex_dim(c, tex, 0x188), th = tex_dim(c, tex, 0x1a8);
        if (tw > 0 && th > 0) ta = (double)tw / th;
    }
    bool wide_screen = fabs(ga - a169) > 0.01 * a169;
    bool screen_tex = ta > 0 && fabs(ta - ga) < 0.02 * ga;
    if (wide_screen && screen_tex) {
        bool box169 = rh > 0 && fabs(rw / rh - a169) < 0.01 * a169;
        if (k < 0 && !(box169 && (ga > a169 ? rh >= 1000 : rw >= 1800))) return;
        if (k >= 0 && (fabsf(rw - fitted[k].w1) > 1 || fabsf(rh - fitted[k].h1) > 1)) return;  // (la movió otro)
        float w = ga > a169 ? (float)(rh * ta) : rw, h = ga > a169 ? rh : (float)(rw / ta);
        if (k < 0 && nfit < 32) k = nfit++, fitted[k].img = img, fitted[k].w0 = rw, fitted[k].h0 = rh;
        if (k >= 0) fitted[k].w1 = w, fitted[k].h1 = h;
        if (w != rw) rect_size(c, rt, 0, w);
        if (h != rh) rect_size(c, rt, 1, h);
        static int nlog;
        if (nlog++ < 6)
            fprintf(stderr, "[unity] captura de pantalla de fondo: RawImage %.0fx%.0f -> %.0fx%.0f (textura de aspecto %.3f)\n",
                    rw, rh, w, h, ta);
    } else if (k >= 0) {  // (otra textura: la medida de antes, si nadie la cambió)
        if (fabsf(rw - fitted[k].w1) <= 1 && fabsf(rh - fitted[k].h1) <= 1) {
            if (fitted[k].w0 != rw) rect_size(c, rt, 0, fitted[k].w0);
            if (fitted[k].h0 != rh) rect_size(c, rt, 1, fitted[k].h0);
        }
        fitted[k] = fitted[--nfit];
    }
}
void f_7001e93320(Cpu *c) {  // RawImage.set_texture(Texture value)
    uint64_t self = c->x[0], value = c->x[1], ret = c->x[30];
    if (!(LD8(GADDR(0x7003cb7d8full)) & 1)) {
        c->x[0] = LD64(GADDR(0x7003a477b8ull)), c->x[30] = GADDR(0x7001e9335cull);
        f_70000c4150(c);
        ST8(GADDR(0x7003cb7d8full), 1);
    }
    uint64_t old = LD64(self + 0xd0);
    uint64_t cls = LD64(LD64(GADDR(0x7003a477b8ull)));  // (UnityEngine.Object: su constructor estático)
    if (!LD32(cls + 0xe0)) {
        c->x[0] = cls, c->x[30] = GADDR(0x7001e9337cull);
        f_70000c4330(c);
    }
    c->x[0] = old, c->x[1] = value, c->x[2] = 0, c->x[30] = GADDR(0x7001e9338cull);
    f_700179f800(c);
    if (c->x[0] & 1) {  // (la misma)
        c->x[30] = ret;
        return;
    }
    ST64(self + 0xd0, value);
    c->x[0] = self + 0xd0, c->x[1] = value, c->x[30] = GADDR(0x7001e933b4ull);
    f_70000c4090(c);
    uint64_t vt = LD64(self);  // SetVerticesDirty, SetMaterialDirty (virtuales)
    c->x[0] = self, c->x[1] = LD64(vt + 0x300), c->pc = LD64(vt + 0x2f8), c->x[30] = GADDR(0x7001e933c8ull);
    rt_dispatch(c);
    vt = LD64(self);
    c->x[0] = self, c->x[1] = LD64(vt + 0x310), c->pc = LD64(vt + 0x308), c->x[30] = GADDR(0x7001e933f0ull);
    rt_dispatch(c);
    capture_image_fit(c, self, value);
    c->x[30] = ret;
}

// ---------------------------------------------------------------- los combates a 60 FPS (runtime/hle/battle_rate.c)
// main 0x7001cd0500 = BattleManager.set_frame_rate(int frame_rate), al empezar cada combate. Igual que el original: si ya
// tiene tasa, nada; guarda QualitySettings.vSyncCount (+0x1cc) y Application.targetFrameRate (+0x1d0) para
// reset_frame_rate, pone _frame_rate (+0x1c8) y vSyncCount = 2 para 30 o 1 para 60. La diferencia: la tasa pasa por el
// runtime (rt_battle_frame_rate: 60, la que pide el juego, un paso de lógica por imagen), que además deja el vsync a 60 Hz
// mientras dure. Los
// campos estáticos de la clase (para que el runtime vea cuándo termina: _instance en +0x10) salen del propio objeto:
// su clase (+0x0) y en ella static_fields (+0xb8), como los lee BattleManager.update.
extern int rt_battle_frame_rate(uint64_t statics, int requested);
void f_7001678140(Cpu *c);  // QualitySettings.get_vSyncCount
void f_7001666000(Cpu *c);  // Application.get_targetFrameRate
void f_7001678180(Cpu *c);  // QualitySettings.set_vSyncCount(int)
void f_7001cd0500(Cpu *c) {
    uint64_t self = c->x[0], ret = c->x[30];
    int requested = (int32_t)c->x[1];
    if (LD32(self + 0x1c8)) return;
    c->x[0] = 0, c->x[30] = GADDR(0x7001cd0530ull);
    f_7001678140(c);
    ST32(self + 0x1cc, (uint32_t)c->x[0]);
    c->x[0] = 0, c->x[30] = GADDR(0x7001cd053cull);
    f_7001666000(c);
    ST32(self + 0x1d0, (uint32_t)c->x[0]);
    uint64_t klass = LD64(self), statics = klass ? LD64(klass + 0xb8) : 0;
    int rate = rt_battle_frame_rate(statics, requested);
    ST32(self + 0x1c8, (uint32_t)rate);
    if (rate == 30 || rate == 60) {
        c->x[0] = rate == 30 ? 2 : 1, c->x[1] = 0, c->x[30] = GADDR(0x7001cd0568ull);
        f_7001678180(c);
    }
    c->x[30] = ret;
}

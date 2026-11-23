// Instalador de SMRPG Recomp (Windows: Win32 y zstd; sin nada del juego dentro).
//
// La ventana pide tu copia del juego, ya extraída (la carpeta con exefs\ y romfs\), y la carpeta de instalación. Al
// instalar:
//   1. comprueba la copia antes de escribir nada (Super Mario RPG, su ID de título, en la versión 1.0.0: el build ID de
//      exefs\main, el único que sabe el port; y los archivos de la RomFS, por sus rutas y tamaños) y la pone en
//      <carpeta>\extracted: con enlaces duros si está en la misma unidad, o copiada;
//   2. copia el programa que lleva dentro (smrpg-recomp.exe, sus DLL, gamelib-src y construir\: nada del juego) a la
//      carpeta; de una 0.47 quita lo que ya no se usa (lasgen\ y zig\);
//   3. construye el código del juego en este PC, DENTRO de este proceso, sin procesos hijos: carga
//      <carpeta>\construir\construir.dll (construir.cpp), que traduce exefs/main y exefs/sdk a C con el generador en un
//      Python embebido y lo compila y enlaza con clang y lld como bibliotecas en extracted/lasgame.dll, para la CPU de
//      este PC (x86-64-v3 con AVX2 si la tiene; si no, -v2 o x86-64);
//   4. crea los accesos directos (escritorio y menú Inicio) a smrpg-recomp.exe --play en esa carpeta.
// Si se corta, se reanuda: un archivo copiado se escribe como .part y se renombra al acabar (uno con su nombre y su
// tamaño no se vuelve a poner), y la construcción guarda lo ya compilado. Instalar encima de una instalación anterior
// la actualiza: los guardados (save/) y los ajustes (larecomp.ini) se quedan.
// Registro: <carpeta>\instalacion.log (y el del programa al construir, <carpeta>\construccion.log).
//
// Empaquetar (`make instalador`): instalador.exe --empaquetar <carpeta del programa> <salida.exe>: este .exe, y detrás
// cada archivo de la carpeta comprimido con zstd y un índice al final.
// Pruebas: --juego J --destino D [--sin-accesos] --auto: instala sin preguntar y se cierra (código 0 = bien);
//   --oculto: sin ventana; --captura <archivo.bmp>: la ventana dibujada en un BMP (fuera de la pantalla) y se cierra;
//   --solo-copiar: solo el paso 1.
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // (Windows 10: GetDpiForWindow, AdjustWindowRectExForDpi)
#define WINVER 0x0A00
#endif
#include <windows.h>
#include <ctype.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <zstd.h>

#ifndef INSTALADOR_VERSION
#define INSTALADOR_VERSION "0.0.0"
#endif
#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)
#define APP_NAME L"SMRPG Recomp"
#define OLD_APP_NAME L"Super Mario RPG Recomp"  // (el nombre hasta la 0.50: el de sus accesos directos)
#define GAME_TID 0x0100BC0018138000ull
// exefs/main de la 1.0.0: la única versión que sabe el port (runtime/game_addresses/versions.h)
static const char GAME_BUILD_ID[] = "e968832cade2ad7cbd85b6c6dfe1d9c1";
#define NEED_BYTES (10ull << 30)  // espacio libre que se pide: el juego 6,4 GB + el programa 0,45 + lo compilado ~1,4
#define MAX_DEST_CHARS 110        // la ruta más larga de la RomFS son 137 caracteres y el programa no usa rutas largas
#define PACK_MAGIC "SMRPGPK1"

// ======================================================================== registro y estado
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;  // g_log, g_status
static wchar_t g_status[512];
static double g_frac;          // 0..1 del total
static wchar_t g_error[2048];  // el motivo de un fallo (para quien instala)
static volatile LONG g_cancel;
static HWND g_wnd;
static HMODULE g_construir;  // construir.dll, una vez cargada (se queda: ver salir)
static bool g_cfg_blocked;   // Control Flow Guard impuesto y no se pudo quitar (cfg_active): no se construye
#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)

static char *u8(const wchar_t *w) {  // (anillo de 8 búferes: para el registro)
    static char buf[8][2048];
    static int i;
    char *b = buf[i++ & 7];
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof buf[0], NULL, NULL)) b[0] = 0;
    return b;
}

static bool g_ui_check;  // (describe_game: la comprobación que hace la ventana al elegir la copia no va al registro)
static void logf_(const char *fmt, ...) {
    if (g_ui_check) return;
    char line[4096];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    if (n > 0 && line[n - 1] != '\n') line[n++] = '\n';
    EnterCriticalSection(&g_cs);
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(g_log, line, (DWORD)n, &w, NULL);
    }
    LeaveCriticalSection(&g_cs);
}

static void open_log(const wchar_t *dest) {
    wchar_t p[1200];
    swprintf(p, 1200, L"%ls\\instalacion.log", dest);
    EnterCriticalSection(&g_cs);
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    g_log = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LeaveCriticalSection(&g_cs);
    logf_("==== instalador %s", INSTALADOR_VERSION);
}

static void set_status_v(bool log, double frac, const wchar_t *fmt, va_list ap) {
    wchar_t s[512];
    vswprintf(s, 512, fmt, ap);
    EnterCriticalSection(&g_cs);
    bool changed = wcscmp(s, g_status) != 0;
    wcscpy(g_status, s);
    if (frac >= 0) g_frac = frac > 1 ? 1 : frac;
    LeaveCriticalSection(&g_cs);
    if (changed && log) logf_("%s", u8(s));
    if (g_wnd) PostMessageW(g_wnd, WM_APP_PROGRESS, 0, 0);
}
static void set_status(double frac, const wchar_t *fmt, ...) {  // (y al registro, si cambia)
    va_list ap;
    va_start(ap, fmt);
    set_status_v(true, frac, fmt, ap);
    va_end(ap);
}
static void set_status_nolog(double frac, const wchar_t *fmt, ...) {  // (solo la ventana: lo que cambia cada segundo)
    va_list ap;
    va_start(ap, fmt);
    set_status_v(false, frac, fmt, ap);
    va_end(ap);
}

static bool fail(const wchar_t *fmt, ...) {  // el motivo para quien instala; siempre devuelve false
    va_list ap;
    va_start(ap, fmt);
    vswprintf(g_error, 2048, fmt, ap);
    va_end(ap);
    logf_("ERROR: %s", u8(g_error));
    return false;
}

// ======================================================================== archivos
static bool read_at(HANDLE f, uint64_t off, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        OVERLAPPED o = {0};
        o.Offset = (DWORD)off, o.OffsetHigh = (DWORD)(off >> 32);
        DWORD want = n > (1u << 30) ? (1u << 30) : (DWORD)n, got = 0;
        if (!ReadFile(f, p, want, &got, &o)) return false;
        if (got != want) { SetLastError(ERROR_HANDLE_EOF); return false; }  // (un archivo más corto: una copia incompleta)
        p += got, off += got, n -= got;
    }
    return true;
}

static bool write_all(HANDLE f, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n) {
        DWORD want = n > (1u << 30) ? (1u << 30) : (DWORD)n, put = 0;
        if (!WriteFile(f, p, want, &put, NULL) || put != want) return false;
        p += put, n -= put;
    }
    return true;
}

// (SMRPG) Las rutas de los argumentos pueden llegar con barras normales (D:/carpeta/): se normalizan a la forma
// de Windows (\, sin barra final salvo la raíz de la unidad) antes de usarlas; el \\?\ de long_path no las admite.
// (dst: 1200 caracteres, como todas las rutas de aquí. Sin las comillas y los espacios de alrededor: «Copiar como ruta
// de acceso» del Explorador las pone)
static void norm_path(wchar_t *dst, const wchar_t *src) {
    while (*src == L' ' || *src == L'"') src++;
    size_t n = 0;
    for (; src[n] && n < 1199; n++) dst[n] = src[n] == L'/' ? L'\\' : src[n];
    dst[n] = 0;
    while (n && (dst[n - 1] == L' ' || dst[n - 1] == L'"')) dst[--n] = 0;
    while (n > 3 && dst[n - 1] == L'\\') dst[--n] = 0;  // (D:\ se queda tal cual)
    if (n == 2 && dst[1] == L':') dst[2] = L'\\', dst[3] = 0;  // («D:» sería la carpeta actual de esa unidad: su raíz)
}

// \\?\ delante: rutas de más de 260 caracteres al escribir (la carpeta ya se limita a MAX_DEST_CHARS)
static void long_path(wchar_t *out, size_t cap, const wchar_t *p) {
    if (!wcsncmp(p, L"\\\\?\\", 4)) swprintf(out, cap, L"%ls", p);
    else if (!wcsncmp(p, L"\\\\", 2)) swprintf(out, cap, L"\\\\?\\UNC\\%ls", p + 2);
    else swprintf(out, cap, L"\\\\?\\%ls", p);
}

static bool mkdirs(const wchar_t *path) {  // (absoluta)
    wchar_t p[2048];
    long_path(p, 2048, path);
    for (wchar_t *c = p + 7; *c; c++) {  // (tras \\?\C:\)
        if (*c != L'\\') continue;
        *c = 0;
        CreateDirectoryW(p, NULL);
        *c = L'\\';
    }
    return CreateDirectoryW(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool file_size(const wchar_t *path, uint64_t *size) {
    wchar_t p[2048];
    long_path(p, 2048, path);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    *size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return true;
}

static bool is_dir(const wchar_t *path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Un archivo nuevo: se escribe como <ruta>.part y se renombra al acabar (finish_file)
static HANDLE begin_file(const wchar_t *path, wchar_t *part, size_t cap) {
    wchar_t lp[2048];
    long_path(lp, 2048, path);
    swprintf(part, cap, L"%ls.part", lp);
    wchar_t dir[2048];
    wcscpy(dir, path);
    wchar_t *s = wcsrchr(dir, L'\\');
    if (s) *s = 0, mkdirs(dir);
    return CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
}
// false: GetLastError() es el de MoveFileExW (5, 32 o 1224 sobre un archivo en uso: el juego abierto)
static bool finish_file(HANDLE h, const wchar_t *part, const wchar_t *path, bool ok) {
    CloseHandle(h);
    wchar_t lp[2048];
    long_path(lp, 2048, path);
    if (ok && MoveFileExW(part, lp, MOVEFILE_REPLACE_EXISTING)) return true;
    DWORD e = ok ? GetLastError() : 0;
    DeleteFileW(part);
    SetLastError(e);
    return false;
}
static bool in_use_error(DWORD e) { return e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION || e == ERROR_USER_MAPPED_FILE; }

// ¿Está abierto el juego de esa carpeta? (la imagen de un programa en marcha no se deja abrir para escribir)
static bool game_running(const wchar_t *dest) {
    wchar_t exe[1300];
    swprintf(exe, 1300, L"%ls\\smrpg-recomp.exe", dest);
    HANDLE h = CreateFileW(exe, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return false; }
    return in_use_error(GetLastError());
}

// ======================================================================== la copia del juego (ya extraída)
// Quien instala elige la carpeta de su copia del juego ya extraída: la que trae exefs\ y romfs\ (si elige una de las
// dos, vale la de encima; si elige una instalación anterior, su extracted\). Antes de escribir nada se comprueba que es
// Super Mario RPG 1.0.0: el build ID de exefs\main (la única versión que sabe el port), el título de exefs\main.npdm y
// cada archivo de la RomFS por su ruta y su tamaño, con las huellas de runtime/game_addresses/romfs_files.h (las mismas
// con las que el programa mira su copia: solo huellas, ni nombres ni datos del juego). Después va a
// <destino>\extracted: con enlaces duros si la copia está en la misma unidad (sin tiempo ni sitio de más); si no,
// copiada.
static const struct { uint64_t hash; uint8_t versions; } ROMFS_TABLE[] = {
#include "../../runtime/game_addresses/romfs_files.h"
};
enum { ROMFS_ENTRIES = sizeof ROMFS_TABLE / sizeof *ROMFS_TABLE };

// FNV-1a 64 de "<ruta>\0<tamaño en decimal>", como game_romfs_hash (runtime/game_addresses.c) y tools/romfs_files.py
static uint64_t romfs_hash(const char *rel, uint64_t size) {
    char digits[24];
    snprintf(digits, sizeof digits, "%llu", (unsigned long long)size);
    uint64_t h = 0xcbf29ce484222325ull;
    for (const unsigned char *p = (const unsigned char *)rel;; p++) {
        h = (h ^ *p) * 0x100000001b3ull;
        if (!*p) break;
    }
    for (const char *p = digits; *p; p++) h = (h ^ (unsigned char)*p) * 0x100000001b3ull;
    return h;
}

// La entrada de la tabla con esa huella en la 1.0.0 (bit 0), o -1
static int romfs_entry(uint64_t h) {
    size_t lo = 0, hi = ROMFS_ENTRIES;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ROMFS_TABLE[mid].hash < h) lo = mid + 1;
        else hi = mid;
    }
    return lo < ROMFS_ENTRIES && ROMFS_TABLE[lo].hash == h && (ROMFS_TABLE[lo].versions & 1) ? (int)lo : -1;
}

static void join_utf8(wchar_t *out, size_t cap, const wchar_t *base, const char *rel) {
    wchar_t w[1200];
    if (!MultiByteToWideChar(CP_UTF8, 0, rel, -1, w, 1200)) w[0] = 0;
    for (wchar_t *c = w; *c; c++) if (*c == L'/') *c = L'\\';
    swprintf(out, cap, L"%ls\\%ls", base, w);
}

typedef struct { wchar_t *rel; uint64_t size; } GFile;  // rel: relativa a la copia (exefs\main, romfs\Data\...)
typedef struct { GFile *f; int n, cap; } GList;

static bool glist_add(GList *l, const wchar_t *rel, uint64_t size) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 2048;
        GFile *g = (GFile *)realloc(l->f, sizeof *g * (size_t)nc);
        if (!g) return false;
        l->f = g, l->cap = nc;
    }
    if (!(l->f[l->n].rel = _wcsdup(rel))) return false;
    l->f[l->n++].size = size;
    return true;
}

static void glist_free(GList *l) {
    for (int i = 0; i < l->n; i++) free(l->f[i].rel);
    free(l->f);
    memset(l, 0, sizeof *l);
}

// Los archivos de <root>\<sub> (y de sus carpetas, con recurse), sin los ocultos (con «.» delante, como el programa) ni
// lo que haya detrás de un enlace de carpeta (unión o simbólico; los demás puntos de reanálisis —los archivos de la
// nube de OneDrive, por ejemplo— sí se recorren)
static bool list_dir(const wchar_t *root, const wchar_t *sub, bool recurse, GList *l, int depth) {
    if (depth > 32 || g_cancel) return false;
    wchar_t pat[1300], lp[2100];
    if (swprintf(pat, 1300, L"%ls\\%ls\\*", root, sub) < 0) return fail(L"Una ruta de tu copia del juego es demasiado larga.");
    long_path(lp, 2100, pat);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(lp, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;  // (vacía)
    bool ok = true;
    do {
        if (fd.cFileName[0] == L'.') continue;  // (también «.» y «..»)
        wchar_t rel[1100];
        if (swprintf(rel, 1100, L"%ls\\%ls", sub, fd.cFileName) < 0) {
            ok = fail(L"Una ruta de tu copia del juego es demasiado larga.");
            break;
        }
        bool link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(fd.dwReserved0);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recurse && !link) ok = list_dir(root, rel, true, l, depth + 1);
        } else {
            ok = glist_add(l, rel, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        }
    } while (ok && FindNextFileW(h, &fd));
    DWORD e = GetLastError();
    FindClose(h);
    if (ok && e != ERROR_NO_MORE_FILES) ok = fail(L"No se pudo leer %ls\\%ls (error %lu).", root, sub, e);  // (no es el final)
    return ok;
}

static bool read_head(const wchar_t *path, uint64_t off, void *buf, size_t n) {
    wchar_t lp[2100];
    long_path(lp, 2100, path);
    HANDLE f = CreateFileW(lp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = read_at(f, off, buf, n);
    CloseHandle(f);
    return ok;
}

// La carpeta de la copia a partir de lo elegido (sin barra final: «D:» para la raíz de una unidad): ella misma si trae
// exefs; su extracted si es una instalación; la de encima si es la propia exefs o romfs
static bool game_root(const wchar_t *picked, wchar_t *out, size_t cap) {
    wchar_t full[1200], t[1300];
    DWORD n0 = picked[0] ? GetFullPathNameW(picked, 1200, full, NULL) : 0;
    if (!n0 || n0 >= 1150) return false;  // (y sitio para «\extracted»)
    size_t n = wcslen(full);
    while (n > 2 && full[n - 1] == L'\\') full[--n] = 0;
    swprintf(t, 1300, L"%ls\\exefs", full);
    if (!is_dir(t)) {
        swprintf(t, 1300, L"%ls\\extracted\\exefs", full);
        wchar_t *s = wcsrchr(full, L'\\');
        if (is_dir(t)) wcscat(full, L"\\extracted");
        else if (s && (!_wcsicmp(s + 1, L"exefs") || !_wcsicmp(s + 1, L"romfs"))) *s = 0;
    }
    swprintf(out, cap, L"%ls", full);
    return true;
}

// exefs\main (su build ID: la 1.0.0) y exefs\main.npdm (el título de Super Mario RPG); también al elegir la carpeta
static bool check_exefs(const wchar_t *root) {
    wchar_t p[1300];
    uint8_t nso[0x60];
    swprintf(p, 1300, L"%ls\\exefs\\main", root);
    if (!read_head(p, 0, nso, sizeof nso))
        return fail(L"En esa carpeta no está exefs\\main: elige la carpeta de tu copia del juego ya extraída (la que trae "
                    L"exefs y romfs).");
    if (memcmp(nso, "NSO0", 4)) return fail(L"El programa del juego (exefs\\main) no tiene la forma esperada.");
    char id[33];
    for (int i = 0; i < 16; i++) snprintf(id + 2 * i, 3, "%02x", nso[0x40 + i]);
    logf_("exefs\\main: build ID %s", id);
    if (strcmp(id, GAME_BUILD_ID))
        return fail(L"Tu copia es otra versión del juego (build %.16hs): el port solo funciona con la 1.0.0 (build %.16hs), "
                    L"el juego base sin actualizaciones.", id, GAME_BUILD_ID);
    uint8_t meta[0x80], aci[0x18];
    swprintf(p, 1300, L"%ls\\exefs\\main.npdm", root);
    uint32_t aci_off = 0;
    bool ok = read_head(p, 0, meta, sizeof meta) && !memcmp(meta, "META", 4);
    if (ok) memcpy(&aci_off, meta + 0x70, 4);
    ok = ok && aci_off && read_head(p, aci_off, aci, sizeof aci) && !memcmp(aci, "ACI0", 4);
    if (!ok) return fail(L"Falta exefs\\main.npdm o no tiene la forma esperada: tu copia está incompleta.");
    uint64_t tid;
    memcpy(&tid, aci + 0x10, 8);
    if (tid != GAME_TID)
        return fail(L"Esa copia no es Super Mario RPG: su título es %016llX (el de Super Mario RPG, %016llX).",
                    (unsigned long long)tid, (unsigned long long)GAME_TID);
    uint64_t sz;
    swprintf(p, 1300, L"%ls\\exefs\\sdk", root);
    if (!file_size(p, &sz)) return fail(L"A tu copia le falta exefs\\sdk: está incompleta.");
    return true;
}

typedef struct {
    wchar_t root[1200];  // la copia: la carpeta con exefs\ y romfs\ (sin barra final)
    GList files;         // lo que va a extracted\: todo exefs\ y los archivos de la RomFS que son del juego
    uint64_t bytes;      // lo que ocupan
} Game;

// Paso 1a: la copia elegida, comprobada entera (sin escribir nada)
static bool check_game(const wchar_t *picked, Game *g) {
    if (!game_root(picked, g->root, 1200)) return fail(L"Elige la carpeta de tu copia del juego ya extraída.");
    logf_("copia del juego: %s", u8(g->root));
    set_status(-1, L"Comprobando tu copia del juego…");
    if (!check_exefs(g->root)) return false;
    wchar_t p[1300];
    swprintf(p, 1300, L"%ls\\romfs", g->root);
    if (!is_dir(p)) return fail(L"Junto a exefs falta la carpeta romfs: elige la carpeta de tu copia ya extraída, con las dos.");
    GList ex = {0}, rf = {0};
    uint8_t *seen = (uint8_t *)calloc(ROMFS_ENTRIES, 1);
    bool ok = seen && list_dir(g->root, L"exefs", false, &ex, 0) && list_dir(g->root, L"romfs", true, &rf, 0);
    int want = 0, have = 0, other = 0;
    for (size_t i = 0; i < ROMFS_ENTRIES; i++) want += ROMFS_TABLE[i].versions & 1;
    for (int i = 0; ok && i < ex.n; i++) ok = glist_add(&g->files, ex.f[i].rel, ex.f[i].size), g->bytes += ex.f[i].size;
    for (int i = 0; ok && i < rf.n; i++) {  // (cada archivo de la tabla, una vez: una carpeta no tiene dos iguales)
        char u[1200];
        if (!WideCharToMultiByte(CP_UTF8, 0, rf.f[i].rel + 6, -1, u, sizeof u, NULL, NULL)) u[0] = 0;  // (sin «romfs\»)
        for (char *c = u; *c; c++) if (*c == '\\') *c = '/';
        int e = u[0] ? romfs_entry(romfs_hash(u, rf.f[i].size)) : -1;
        if (e >= 0 && !seen[e]) {
            seen[e] = 1, have++;
            ok = glist_add(&g->files, rf.f[i].rel, rf.f[i].size), g->bytes += rf.f[i].size;
        } else if (++other <= 10) {
            logf_("RomFS: %s (%llu bytes) no es un archivo de la 1.0.0: no se usa", u, (unsigned long long)rf.f[i].size);
        }
    }
    free(seen), glist_free(&ex), glist_free(&rf);
    if (!ok) {  // (con el motivo de list_dir, si lo dio)
        if (!g_error[0]) fail(g_cancel ? L"Cancelado." : L"No se pudo leer tu copia del juego en %ls.", g->root);
        return false;
    }
    logf_("RomFS: %d de los %d archivos del juego; %d que no son suyos (no se usan); en total %.2f GB", have, want, other,
          g->bytes / 1e9);
    if (have < want)
        return fail(L"A la RomFS de tu copia le faltan %d de sus %d archivos (o tienen otro tamaño): está incompleta o no es la "
                    L"versión 1.0.0. Vuelve a ponerla entera.", want - have, want);
    return true;
}

typedef struct { uint64_t base, total; double f0, f1; DWORD last; const wchar_t *what; } CopyProg;

static void copy_status(CopyProg *c, uint64_t at) {
    set_status_nolog(c->f0 + (c->f1 - c->f0) * (c->total ? (double)at / (double)c->total : 1),
                     L"Poniendo tu copia del juego en la instalación: %ls (%.1f de %.1f GB)", c->what, at / 1e9, c->total / 1e9);
}

static DWORD CALLBACK copy_progress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER s_total, LARGE_INTEGER s_done,
                                    DWORD stream, DWORD reason, HANDLE src, HANDLE dst, LPVOID ctx) {
    (void)total, (void)s_total, (void)s_done, (void)stream, (void)reason, (void)src, (void)dst;
    CopyProg *c = (CopyProg *)ctx;
    if (g_cancel) return PROGRESS_CANCEL;
    if (GetTickCount() - c->last > 150) c->last = GetTickCount(), copy_status(c, c->base + (uint64_t)done.QuadPart);
    return PROGRESS_CONTINUE;
}

// ¿Son el mismo archivo (el mismo volumen y el mismo índice: un enlace duro, o la propia copia)? (rutas con \\?\)
static bool same_file(const wchar_t *a, const wchar_t *b) {
    BY_HANDLE_FILE_INFORMATION ia, ib;
    HANDLE ha = CreateFileW(a, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hb = CreateFileW(b, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    bool same = ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE && GetFileInformationByHandle(ha, &ia) &&
                GetFileInformationByHandle(hb, &ib) && ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
                ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    return same;
}

// Los enlaces duros no se pueden en ese volumen o entre esos dos: se copia todo lo que queda
static bool link_unsupported(DWORD e) {
    return e == ERROR_NOT_SAME_DEVICE || e == ERROR_NOT_SUPPORTED || e == ERROR_INVALID_FUNCTION || e == ERROR_TOO_MANY_LINKS;
}

// Paso 1b: la copia comprobada -> <dest>\extracted (lo que ya esté con su tamaño se queda: una instalación anterior,
// o la propia copia si se eligió esa)
static bool put_game(const Game *g, const wchar_t *dest, double f0, double f1) {
    wchar_t ext[1300];
    swprintf(ext, 1300, L"%ls\\extracted", dest);
    if (!mkdirs(ext)) return fail(L"No se pudo crear %ls (error %lu).", ext, GetLastError());
    bool link = true, space_checked = false;  // (enlaces mientras funcionen: en otra unidad o en FAT32 / exFAT, no)
    int linked = 0, copied = 0, kept = 0, link_fails = 0;
    CopyProg c = {0, g->bytes, f0, f1, GetTickCount(), L""};
    copy_status(&c, 0);
    for (int i = 0; i < g->files.n; i++) {
        const GFile *f = &g->files.f[i];
        wchar_t src[1300], dst[1300], dir[1300], ls[2100], ld[2100], part[2100];
        swprintf(src, 1300, L"%ls\\%ls", g->root, f->rel);
        swprintf(dst, 1300, L"%ls\\%ls", ext, f->rel);
        c.what = wcsrchr(f->rel, L'\\') ? wcsrchr(f->rel, L'\\') + 1 : f->rel;
        uint64_t have;
        if (file_size(dst, &have) && have == f->size) {
            kept++, c.base += f->size;
            continue;
        }
        if (g_cancel) return fail(L"Cancelado.");
        long_path(ls, 2100, src), long_path(ld, 2100, dst);
        if (same_file(ls, ld)) {  // (la propia copia: sus tamaños se miden de dos formas y pueden no casar un instante)
            kept++, c.base += f->size;
            continue;
        }
        wcscpy(dir, dst);
        *wcsrchr(dir, L'\\') = 0;
        if (!mkdirs(dir)) return fail(L"No se pudo crear %ls (error %lu).", dir, GetLastError());
        swprintf(part, 2100, L"%ls.part", ld);
        DeleteFileW(part);  // (lo que dejara a medias un corte)
        SetFileAttributesW(ld, FILE_ATTRIBUTE_NORMAL);  // (uno viejo de otro tamaño, aunque sea de solo lectura)
        DeleteFileW(ld);
        bool done = false;
        if (link) {
            done = CreateHardLinkW(ld, ls, NULL) != 0;
            if (done) {
                linked++, link_fails = 0;
            } else {
                DWORD e = GetLastError();
                if (link_unsupported(e) || ++link_fails >= 3) link = false;  // (otros fallos, a la tercera seguida)
                logf_("enlace duro: no (error %lu con %s): se copia%s", e, u8(f->rel), link ? "" : ", y lo que queda también");
            }
        }
        if (!done && !space_checked) {  // (al empezar a copiar: lo que queda cabe, con el programa y su construcción)
            space_checked = true;
            ULARGE_INTEGER fb;
            uint64_t need = g->bytes - c.base + (2ull << 30);
            if (GetDiskFreeSpaceExW(ext, &fb, NULL, NULL) && fb.QuadPart < need)
                return fail(L"No cabe: hay que copiar tu copia del juego (no se pudo enlazar) y en esa unidad quedan %.1f GB "
                            L"libres de los %.1f GB que hacen falta. Haz sitio o elige otra carpeta.", fb.QuadPart / 1e9, need / 1e9);
        }
        if (!done) {
            BOOL stop = FALSE;
            if (!CopyFileExW(ls, part, copy_progress, &c, &stop, 0)) {
                DWORD e = GetLastError();
                DeleteFileW(part);
                if (g_cancel || e == ERROR_REQUEST_ABORTED) return fail(L"Cancelado.");
                if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL)
                    return fail(L"No cabe: falta espacio en la unidad de la instalación (al copiar %ls).", src);
                return fail(L"No se pudo copiar %ls (error %lu).", src, e);
            }
            if (!MoveFileExW(part, ld, MOVEFILE_REPLACE_EXISTING)) {
                DWORD e = GetLastError();
                DeleteFileW(part);
                return fail(in_use_error(e) ? L"No se pudo escribir %ls: ¿está el juego abierto? Ciérralo y vuelve a instalar."
                                            : L"No se pudo escribir %ls (error %lu).", dst, e);
            }
            copied++;
        }
        c.base += f->size;
        if (GetTickCount() - c.last > 150) c.last = GetTickCount(), copy_status(&c, c.base);
    }
    logf_("juego en %s: %d archivos enlazados, %d copiados y %d que ya estaban", u8(ext), linked, copied, kept);
    set_status(f1, L"Tu copia del juego está en la instalación (%.1f GB).", g->bytes / 1e9);
    return true;
}

// ======================================================================== el programa que lleva dentro (zstd)
// Detrás de este .exe: entradas [u16 largo][ruta UTF-8 con '/'][u64 tamaño][u64 comprimido][datos zstd] y al final
// [u64 inicio][u64 entradas][u64 bytes][PACK_MAGIC].
static bool payload_info(HANDLE self, uint64_t *start, uint64_t *count, uint64_t *bytes) {
    LARGE_INTEGER sz;
    uint8_t t[32];
    if (!GetFileSizeEx(self, &sz) || sz.QuadPart < 32 || !read_at(self, (uint64_t)sz.QuadPart - 32, t, 32) ||
        memcmp(t + 24, PACK_MAGIC, 8))
        return false;
    memcpy(start, t, 8), memcpy(count, t + 8, 8), memcpy(bytes, t + 16, 8);
    return *start < (uint64_t)sz.QuadPart;
}

static bool payload_present(void) {
    wchar_t me[1200];
    if (!GetModuleFileNameW(NULL, me, 1200)) return false;
    HANDLE f = CreateFileW(me, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    uint64_t a, b, c;
    bool ok = f != INVALID_HANDLE_VALUE && payload_info(f, &a, &b, &c);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return ok;
}

// ¿Es una instalación 0.47? Su LEEME.txt lo dice en la primera línea (se mira ANTES de copiar el programa nuevo, que lo
// sustituye). Solo entonces se quita lo suyo que ya no se usa (remove_old_parts).
static bool is_047(const wchar_t *dest) {
    wchar_t p[1300];
    swprintf(p, 1300, L"%ls\\LEEME.txt", dest);
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    char b[64] = {0};
    DWORD got = 0;
    bool ok = ReadFile(f, b, sizeof b - 1, &got, NULL) && !strncmp(b, "Super Mario RPG Recomp 0.47.", 28);
    CloseHandle(f);
    return ok;
}

// Lo que llevaba la 0.47 y ya no se usa: el generador congelado (lasgen\) y zig (zig\), que sustituye construir\ desde
// la 0.48, y dos licencias suyas. Por nombre, solo esos archivos (los que puso el instalador 0.47; lo que no sea suyo
// se queda), y después sus carpetas si se quedan vacías. Nada dentro de un enlace (unión o simbólico).
static const char *const OLD_047[] = {
    "lasgen/_internal/VCRUNTIME140.dll", "lasgen/_internal/VCRUNTIME140_1.dll", "lasgen/_internal/_bz2.pyd",
    "lasgen/_internal/_ctypes.pyd", "lasgen/_internal/_lzma.pyd", "lasgen/_internal/_socket.pyd",
    "lasgen/_internal/_wmi.pyd", "lasgen/_internal/base_library.zip", "lasgen/_internal/capstone/lib/capstone.dll",
    "lasgen/_internal/libffi-8.dll", "lasgen/_internal/lz4-4.4.5.dist-info/INSTALLER", "lasgen/_internal/lz4-4.4.5.dist-info/METADATA",
    "lasgen/_internal/lz4-4.4.5.dist-info/RECORD", "lasgen/_internal/lz4-4.4.5.dist-info/REQUESTED", "lasgen/_internal/lz4-4.4.5.dist-info/WHEEL",
    "lasgen/_internal/lz4-4.4.5.dist-info/licenses/LICENSE", "lasgen/_internal/lz4-4.4.5.dist-info/top_level.txt", "lasgen/_internal/lz4/_version.cp312-win_amd64.pyd",
    "lasgen/_internal/lz4/block/_block.cp312-win_amd64.pyd", "lasgen/_internal/python312.dll", "lasgen/_internal/rando/patch_variants.json",
    "lasgen/_internal/runtime/game_addresses/patches_v1_0_0.h", "lasgen/_internal/runtime/game_addresses/patches_v1_0_1.h", "lasgen/_internal/runtime/game_addresses/versions.h",
    "lasgen/_internal/select.pyd", "lasgen/_internal/unicodedata.pyd", "lasgen/_internal/unicorn/lib/unicorn.dll",
    "lasgen/lasgen.exe", "zig/LICENSE", "zig/lib/include/__float_float.h",
    "zig/lib/include/__float_header_macro.h", "zig/lib/include/__float_infinity_nan.h", "zig/lib/include/__stddef_header_macro.h",
    "zig/lib/include/__stddef_max_align_t.h", "zig/lib/include/__stddef_null.h", "zig/lib/include/__stddef_offsetof.h",
    "zig/lib/include/__stddef_ptrdiff_t.h", "zig/lib/include/__stddef_size_t.h", "zig/lib/include/__stddef_wchar_t.h",
    "zig/lib/include/__stddef_wint_t.h", "zig/lib/include/float.h", "zig/lib/include/limits.h",
    "zig/lib/include/mm_malloc.h", "zig/lib/include/stdbool.h", "zig/lib/include/stddef.h",
    "zig/lib/include/stdint.h", "zig/lib/include/vadefs.h", "zig/lib/libc/include/any-windows-any/_mingw.h",
    "zig/lib/libc/include/any-windows-any/_mingw_mac.h", "zig/lib/libc/include/any-windows-any/_mingw_off_t.h", "zig/lib/libc/include/any-windows-any/_mingw_secapi.h",
    "zig/lib/libc/include/any-windows-any/corecrt.h", "zig/lib/libc/include/any-windows-any/corecrt_memory.h", "zig/lib/libc/include/any-windows-any/corecrt_stdio_config.h",
    "zig/lib/libc/include/any-windows-any/corecrt_wstdlib.h", "zig/lib/libc/include/any-windows-any/crtdefs.h", "zig/lib/libc/include/any-windows-any/fenv.h",
    "zig/lib/libc/include/any-windows-any/float.h", "zig/lib/libc/include/any-windows-any/limits.h", "zig/lib/libc/include/any-windows-any/malloc.h",
    "zig/lib/libc/include/any-windows-any/math.h", "zig/lib/libc/include/any-windows-any/sec_api/stdio_s.h", "zig/lib/libc/include/any-windows-any/sec_api/stdlib_s.h",
    "zig/lib/libc/include/any-windows-any/sec_api/string_s.h", "zig/lib/libc/include/any-windows-any/stddef.h", "zig/lib/libc/include/any-windows-any/stdint.h",
    "zig/lib/libc/include/any-windows-any/stdio.h", "zig/lib/libc/include/any-windows-any/stdlib.h", "zig/lib/libc/include/any-windows-any/string.h",
    "zig/lib/libc/include/any-windows-any/swprintf.inl", "zig/lib/libc/include/any-windows-any/vadefs.h", "zig/lib/std/std.zig",
    "zig/rt/advapi32.lib", "zig/rt/api-ms-win-crt-conio-l1-1-0.lib", "zig/rt/api-ms-win-crt-convert-l1-1-0.lib",
    "zig/rt/api-ms-win-crt-environment-l1-1-0.lib", "zig/rt/api-ms-win-crt-filesystem-l1-1-0.lib", "zig/rt/api-ms-win-crt-heap-l1-1-0.lib",
    "zig/rt/api-ms-win-crt-locale-l1-1-0.lib", "zig/rt/api-ms-win-crt-math-l1-1-0.lib", "zig/rt/api-ms-win-crt-multibyte-l1-1-0.lib",
    "zig/rt/api-ms-win-crt-private-l1-1-0.lib", "zig/rt/api-ms-win-crt-process-l1-1-0.lib", "zig/rt/api-ms-win-crt-runtime-l1-1-0.lib",
    "zig/rt/api-ms-win-crt-stdio-l1-1-0.lib", "zig/rt/api-ms-win-crt-string-l1-1-0.lib", "zig/rt/api-ms-win-crt-time-l1-1-0.lib",
    "zig/rt/api-ms-win-crt-utility-l1-1-0.lib", "zig/rt/compiler_rt.lib", "zig/rt/dllcrt2.obj",
    "zig/rt/kernel32.lib", "zig/rt/libmingw32.lib", "zig/rt/link.rsp",
    "zig/rt/ntdll.lib", "zig/rt/shell32.lib", "zig/rt/user32.lib",
    "zig/rt/zigc.lib", "zig/zig.exe",
    "licencias/PyInstaller-COPYING.txt", "licencias/fuentes/lasgen-fuente.zip",
};

static bool under_link(const wchar_t *dest, const wchar_t *path) {  // ¿alguna carpeta entre dest y path es un enlace?
    wchar_t p[1300];
    wcscpy(p, path);
    size_t base = wcslen(dest);
    for (wchar_t *s = wcsrchr(p, L'\\'); s && (size_t)(s - p) > base; s = wcsrchr(p, L'\\')) {
        *s = 0;
        DWORD a = GetFileAttributesW(p);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
    }
    return false;
}

static void remove_old_parts(const wchar_t *dest) {
    int gone = 0, kept = 0;
    size_t n = sizeof OLD_047 / sizeof *OLD_047;
    for (size_t i = 0; i < n; i++) {
        wchar_t p[1300];
        join_utf8(p, 1300, dest, OLD_047[i]);
        DWORD a = GetFileAttributesW(p);
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        if ((a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || under_link(dest, p)) { kept++; continue; }
        if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p, a & ~FILE_ATTRIBUTE_READONLY);
        DeleteFileW(p) ? gone++ : kept++;
    }
    // sus carpetas (solo dentro de lasgen\ y zig\), de la más honda a la de arriba: RemoveDirectory solo quita las vacías
    for (int pass = 0; pass < 8; pass++)
        for (size_t i = 0; i < n; i++) {
            if (strncmp(OLD_047[i], "lasgen/", 7) && strncmp(OLD_047[i], "zig/", 4)) continue;
            wchar_t p[1300];
            join_utf8(p, 1300, dest, OLD_047[i]);
            for (wchar_t *s = wcsrchr(p, L'\\'); s && (size_t)(s - p) > wcslen(dest); s = wcsrchr(p, L'\\')) {
                *s = 0;
                DWORD a = GetFileAttributesW(p);
                if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_REPARSE_POINT) && !under_link(dest, p)) RemoveDirectoryW(p);
            }
        }
    logf_("de la 0.47 (ya no se usan): %d archivos quitados%s", gone, kept ? ", algunos no se pudieron quitar o no eran suyos" : "");
}

static bool install_payload(const wchar_t *dest, double f0, double f1) {
    wchar_t me[1200];
    if (!GetModuleFileNameW(NULL, me, 1200)) return fail(L"No se encuentra el propio instalador.");
    HANDLE self = CreateFileW(me, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    uint64_t at, count, bytes, done = 0;
    if (self == INVALID_HANDLE_VALUE || !payload_info(self, &at, &count, &bytes)) {
        if (self != INVALID_HANDLE_VALUE) CloseHandle(self);
        return fail(L"Este instalador está incompleto (no lleva el programa dentro).");
    }
    // (reintentar en la misma sesión: si construir.dll ya está cargada desde esta carpeta, ella y lo que cargó no se
    // dejan sustituir; son los de este mismo instalador, copiados antes de cargarla: los que están, con su tamaño, se
    // dejan, y los que falten se escriben)
    wchar_t loaded[1300] = L"", mine[1300];
    if (g_construir && GetModuleFileNameW(g_construir, loaded, 1300)) {
        wchar_t *s = wcsrchr(loaded, L'\\');
        if (s) *s = 0;
    }
    swprintf(mine, 1300, L"%ls\\construir", dest);
    bool keep_loaded = loaded[0] && !_wcsicmp(loaded, mine);
    ZSTD_DCtx *dc = ZSTD_createDCtx();
    const size_t INCH = 1 << 20;
    uint8_t *in = (uint8_t *)malloc(INCH), *outb = (uint8_t *)malloc(ZSTD_DStreamOutSize());
    bool ok = dc && in && outb;
    if (!ok) fail(L"No hay memoria para copiar el programa.");
    DWORD last = 0;
    for (uint64_t i = 0; ok && i < count; i++) {
        if (g_cancel) { ok = false; fail(L"Cancelado."); break; }
        uint8_t hdr[2];
        char rel[1024];
        uint64_t size, comp;
        ok = read_at(self, at, hdr, 2);
        uint16_t pl = ok ? (uint16_t)(hdr[0] | hdr[1] << 8) : 0;
        ok = ok && pl && pl < sizeof rel && read_at(self, at + 2, rel, pl) && read_at(self, at + 2 + pl, &size, 8) &&
             read_at(self, at + 10 + pl, &comp, 8);
        if (ok) rel[pl] = 0, at += 18 + pl;
        if (!ok || strstr(rel, "..") || rel[0] == '/' || strchr(rel, ':')) {
            ok = false;
            fail(L"Este instalador está dañado: vuelve a descargarlo.");
            break;
        }
        wchar_t path[1300];
        join_utf8(path, 1300, dest, rel);
        if (keep_loaded && (!_strnicmp(rel, "construir\\", 10) || !_strnicmp(rel, "construir/", 10))) {
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) &&
                (((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) == size) {
                at += comp, done += comp;
                continue;
            }
        }
        wchar_t part[2100];
        HANDLE h = begin_file(path, part, 2100);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            fail(e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED
                     ? L"No se pudo escribir %ls: ¿está el juego abierto? Ciérralo y vuelve a instalar."
                     : L"No se pudo escribir %ls (error %lu).", path, e);
            ok = false;
            break;
        }
        ZSTD_DCtx_reset(dc, ZSTD_reset_session_only);
        uint64_t left = comp, wrote = 0;
        while (ok && left) {
            size_t n = left > INCH ? INCH : (size_t)left;
            ok = read_at(self, at, in, n);
            at += n, left -= n;
            ZSTD_inBuffer zi = {in, n, 0};
            while (ok && zi.pos < zi.size) {
                ZSTD_outBuffer zo = {outb, ZSTD_DStreamOutSize(), 0};
                size_t r = ZSTD_decompressStream(dc, &zo, &zi);
                ok = !ZSTD_isError(r) && write_all(h, outb, zo.pos);
                wrote += zo.pos;
            }
            done += n;
            if (GetTickCount() - last > 150) {
                last = GetTickCount();
                set_status(f0 + (f1 - f0) * (bytes ? (double)done / (double)bytes : 0), L"Copiando el programa…");
            }
        }
        bool data_ok = ok && wrote == size;
        if (!finish_file(h, part, path, data_ok)) {
            DWORD e = GetLastError();
            if (!g_error[0])
                fail(!data_ok ? L"No se pudo copiar %ls: el instalador está dañado o falta espacio."
                     : in_use_error(e) ? L"No se pudo sustituir %ls: ¿está el juego abierto? Ciérralo y vuelve a instalar."
                                       : L"No se pudo sustituir %ls (error %lu).", path, e);
            ok = false;
        }
    }
    free(in), free(outb);
    ZSTD_freeDCtx(dc);
    CloseHandle(self);
    if (ok) set_status(f1, L"Programa copiado.");
    return ok;
}

// --empaquetar <carpeta> <salida.exe>
typedef struct { char rel[1024]; wchar_t abs[1300]; } PFile;
static void list_tree(const wchar_t *root, const wchar_t *sub, PFile **out, int *n, int *cap) {
    wchar_t pat[1300];
    swprintf(pat, 1300, sub[0] ? L"%ls\\%ls\\*" : L"%ls%ls\\*", root, sub);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t rel[1300];
        swprintf(rel, 1300, sub[0] ? L"%ls\\%ls" : L"%ls%ls", sub, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { list_tree(root, rel, out, n, cap); continue; }
        if (*n == *cap) *cap = *cap ? *cap * 2 : 256, *out = (PFile *)realloc(*out, sizeof **out * (size_t)*cap);
        PFile *p = &(*out)[(*n)++];
        swprintf(p->abs, 1300, L"%ls\\%ls", root, rel);
        WideCharToMultiByte(CP_UTF8, 0, rel, -1, p->rel, sizeof p->rel, NULL, NULL);
        for (char *c = p->rel; *c; c++) if (*c == '\\') *c = '/';
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static int cmp_rel(const void *a, const void *b) { return strcmp(((const PFile *)a)->rel, ((const PFile *)b)->rel); }

static int pack(const wchar_t *dir, const wchar_t *out) {
    wchar_t me[1200];
    GetModuleFileNameW(NULL, me, 1200);
    if (!CopyFileW(me, out, FALSE)) { fwprintf(stderr, L"no se pudo copiar el instalador a %ls\n", out); return 1; }
    HANDLE o = CreateFileW(out, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER end = {0}, zero = {0};
    if (o == INVALID_HANDLE_VALUE || !SetFilePointerEx(o, zero, &end, FILE_END)) return 1;
    uint64_t start = (uint64_t)end.QuadPart, count = 0, bytes = 0;
    PFile *fl = NULL;
    int n = 0, cap = 0;
    list_tree(dir, L"", &fl, &n, &cap);
    qsort(fl, (size_t)n, sizeof *fl, cmp_rel);
    ZSTD_CCtx *cc = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, 19);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ZSTD_CCtx_setParameter(cc, ZSTD_c_nbWorkers, (int)si.dwNumberOfProcessors);  // (si la zstd tiene hilos)
    size_t outcap = ZSTD_CStreamOutSize();
    uint8_t *ob = (uint8_t *)malloc(outcap);
    for (int i = 0; i < n; i++) {
        HANDLE f = CreateFileW(fl[i].abs, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        LARGE_INTEGER fs;
        if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &fs)) { fwprintf(stderr, L"no se pudo leer %ls\n", fl[i].abs); return 1; }
        uint8_t *src = (uint8_t *)malloc((size_t)fs.QuadPart + 1);
        DWORD got = 0;
        if (!src || (fs.QuadPart && (!ReadFile(f, src, (DWORD)fs.QuadPart, &got, NULL) || got != (DWORD)fs.QuadPart))) return 1;
        CloseHandle(f);
        size_t rl = strlen(fl[i].rel);
        if (!rl || rl >= 1024) { fprintf(stderr, "ruta demasiado larga: %s\n", fl[i].rel); return 1; }
        uint16_t pl = (uint16_t)rl;
        uint64_t size = (uint64_t)fs.QuadPart, comp = 0;
        LARGE_INTEGER here;
        uint8_t h2[2] = {(uint8_t)pl, (uint8_t)(pl >> 8)};
        bool w = SetFilePointerEx(o, zero, &here, FILE_CURRENT) && write_all(o, h2, 2) && write_all(o, fl[i].rel, pl) &&
                 write_all(o, &size, 8) && write_all(o, &comp, 8);
        ZSTD_CCtx_reset(cc, ZSTD_reset_session_only);
        ZSTD_CCtx_setPledgedSrcSize(cc, size);
        ZSTD_inBuffer zi = {src, (size_t)size, 0};
        while (w) {
            ZSTD_outBuffer zo = {ob, outcap, 0};
            size_t r = ZSTD_compressStream2(cc, &zo, &zi, ZSTD_e_end);
            if (ZSTD_isError(r)) { fprintf(stderr, "zstd: %s\n", ZSTD_getErrorName(r)); return 1; }
            w = write_all(o, ob, zo.pos);
            comp += zo.pos;
            if (r == 0) break;
        }
        LARGE_INTEGER after, at = {.QuadPart = here.QuadPart + 2 + pl + 8};
        w = w && SetFilePointerEx(o, zero, &after, FILE_CURRENT) && SetFilePointerEx(o, at, NULL, FILE_BEGIN) &&
            write_all(o, &comp, 8) && SetFilePointerEx(o, after, NULL, FILE_BEGIN);
        if (!w) { fprintf(stderr, "no se pudo escribir %ls (error %lu)\n", out, GetLastError()); return 1; }
        free(src);
        count++, bytes += comp;
        printf("%6.1f MB -> %6.1f MB  %s\n", size / 1e6, comp / 1e6, fl[i].rel);
    }
    uint8_t t[32];
    memcpy(t, &start, 8), memcpy(t + 8, &count, 8), memcpy(t + 16, &bytes, 8), memcpy(t + 24, PACK_MAGIC, 8);
    if (!write_all(o, t, 32) || !CloseHandle(o)) { fprintf(stderr, "no se pudo terminar %ls\n", out); return 1; }
    printf("%llu archivos, %.1f MB comprimidos\n", (unsigned long long)count, bytes / 1e6);
    return n > 0 ? 0 : 1;
}

// ======================================================================== construir, accesos directos
// El código del juego se construye DENTRO de este proceso: <destino>\construir\construir.dll (tools/instalador/
// construir.cpp) lleva el generador en un Python embebido, clang y lld como bibliotecas, y lo que incluyen y enlazan;
// trabaja con hilos, sin procesos hijos. Se queda cargada (su generador puede seguir en un hilo si se cancela).
typedef void (*AvanceFn)(void *ctx, double frac, const wchar_t *paso);
typedef int (*ConstruirFn)(const wchar_t *dest, int jobs, AvanceFn avance, void *ctx, volatile long *cancelar,
                           wchar_t *why, int why_cap);
static double g_bf0, g_bf1;
static DWORD g_bt0;
static void build_avance(void *ctx, double f, const wchar_t *paso) {
    static int logged = -1;  // (al registro, cada 5 %; la ventana, cada vez)
    (void)ctx;
    DWORD s = (GetTickCount() - g_bt0) / 1000;
    set_status_nolog(g_bf0 + (g_bf1 - g_bf0) * f, L"Construyendo el código del juego en este PC: %ls (%d %%, %u:%02u)", paso,
                     (int)(f * 100), s / 60, s % 60);
    int k = (int)(f * 20);
    if (k != logged || f >= 1) {
        logged = k;
        logf_("construyendo: %d %%, %u:%02u, %s", (int)(f * 100), s / 60, s % 60, u8(paso));
    }
}

// Compilaciones a la vez: los procesadores lógicos, sin pasar de los GB de RAM menos 2 (cada compilación de un trozo
// grande ronda 0,5-1 GB) ni de 64
static int build_jobs(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    MEMORYSTATUSEX m = {sizeof m};
    GlobalMemoryStatusEx(&m);
    // (con la memoria libre de ahora, no la total: medido, 16 hilos llegan a ~4,6 GB, pero un trozo grande pide ~1 GB)
    int cores = (int)si.dwNumberOfProcessors, ram = (int)(m.ullTotalPhys >> 30) - 2, avail = (int)(m.ullAvailPhys >> 29);
    int j = cores < ram ? cores : ram;
    if (avail < j) j = avail;  // (2 por GB libre)
    wchar_t e[16];
    if (GetEnvironmentVariableW(L"SMRPG_CONSTRUIR_HILOS", e, 16) && _wtoi(e) > 0) j = _wtoi(e);  // (pruebas)
    return j < 1 ? 1 : j > 64 ? 64 : j;
}

static bool build_game(const wchar_t *dest, double f0, double f1) {
    static ConstruirFn construir;
    wchar_t path[1300], why[2048] = L"";
    if (g_cfg_blocked)
        return fail(L"Windows tiene impuesta la protección «Control Flow Guard» a todos los programas, y con ella el generador "
                    L"del código del juego no puede funcionar (y no se pudo quitar solo para este instalador). En Seguridad de "
                    L"Windows > Control de aplicaciones y explorador > Protección contra vulnerabilidades, añade este instalador "
                    L"en «Configuración de programas» con la protección del flujo de control (CFG) desactivada, y vuelve a instalar.");
    if (g_cancel) return fail(L"Cancelado (lo ya compilado se guarda: volver a instalar sigue desde ahí).");
    if (!g_construir) {
        swprintf(path, 1300, L"%ls\\construir\\construir.dll", dest);
        g_construir = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);  // (sus DLL, de su carpeta)
        if (!g_construir) {
            DWORD e = GetLastError();
            // (una DLL con el mismo nombre que una de las suyas, ya cargada desde otra carpeta —por ejemplo por una
            // extensión del Explorador al usar Examinar—: Windows usa esa, y si no casa, la carga falla)
            static const wchar_t *const deps[] = {L"libLLVM-23.dll", L"libclang-cpp.dll", L"libstdc++-6.dll",
                                                  L"libgcc_s_seh-1.dll", L"libwinpthread-1.dll", L"libffi-8.dll",
                                                  L"libxml2-16.dll", L"libzstd.dll", L"zlib1.dll", L"libiconv-2.dll"};
            wchar_t other[1300] = L"";
            for (size_t i = 0; i < sizeof deps / sizeof *deps && !other[0]; i++) {
                HMODULE m = GetModuleHandleW(deps[i]);
                wchar_t p[1300], want[1300];
                swprintf(want, 1300, L"%ls\\construir\\%ls", dest, deps[i]);
                if (m && GetModuleFileNameW(m, p, 1300) && _wcsicmp(p, want)) wcscpy(other, p);
            }
            if (other[0])
                return fail(L"No se pudo cargar el constructor del juego (error %lu): este proceso ya tiene cargada otra %ls, "
                            L"de otro programa. Cierra el instalador, vuelve a abrirlo y escribe las rutas sin usar «Examinar…».",
                            e, other);
            return fail(L"No se pudo cargar el constructor del juego (%ls, error %lu): vuelve a descargar el instalador.", path, e);
        }
        construir = (ConstruirFn)(void *)GetProcAddress(g_construir, "construir_juego");
    }
    if (!construir) return fail(L"El constructor del juego (%ls\\construir\\construir.dll) no es de esta versión.", dest);
    int jobs = build_jobs();
    g_bf0 = f0, g_bf1 = f1, g_bt0 = GetTickCount();
    set_status(f0, L"Construyendo el código del juego en este PC: empezando…");
    logf_("construir: en este proceso, %d hilos de compilación", jobs);
    int r = construir(dest, jobs, build_avance, NULL, &g_cancel, why, 2048);
    logf_("construir: terminó con el código %d en %lu s", r, (GetTickCount() - g_bt0) / 1000);
    // (si acabó bien, vale aunque la cancelación llegara al final, en el enlace)
    if (r != 0 && g_cancel) return fail(L"Cancelado (lo ya compilado se guarda: volver a instalar sigue desde ahí).");
    if (r != 0) return fail(L"No se pudo construir el código del juego: %ls. Mira %ls\\construccion.log.", why[0] ? why : L"sin más detalle", dest);
    set_status(f1, L"Código del juego construido.");
    return true;
}

// (pruebas: SMRPG_INSTALADOR_ACCESOS=<carpeta> los deja ahí, «<nombre> (tag).lnk», en vez del escritorio y el menú Inicio)
static bool make_link(REFKNOWNFOLDERID where, const wchar_t *tag, const wchar_t *dest) {
    wchar_t lnk[1300], exe[1300], test[1100];
    if (GetEnvironmentVariableW(L"SMRPG_INSTALADOR_ACCESOS", test, 1100) && test[0]) {
        swprintf(lnk, 1300, L"%ls\\%ls (%ls).lnk", test, APP_NAME, tag);
    } else {
        PWSTR dir = NULL;
        if (FAILED(SHGetKnownFolderPath(where, 0, NULL, &dir))) return false;
        swprintf(lnk, 1300, L"%ls\\%ls.lnk", dir, APP_NAME);
        CoTaskMemFree(dir);
    }
    swprintf(exe, 1300, L"%ls\\smrpg-recomp.exe", dest);
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    bool ok = SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl));
    if (ok) {
        IShellLinkW_SetPath(sl, exe);
        IShellLinkW_SetArguments(sl, L"--play");
        IShellLinkW_SetWorkingDirectory(sl, dest);
        IShellLinkW_SetDescription(sl, L"Super Mario RPG, recompilado para PC");
        IShellLinkW_SetIconLocation(sl, exe, 0);
        ok = SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf)) && SUCCEEDED(IPersistFile_Save(pf, lnk, TRUE));
        if (pf) IPersistFile_Release(pf);
        IShellLinkW_Release(sl);
    }
    logf_("acceso directo %s: %s", u8(lnk), ok ? "creado" : "NO se pudo crear");
    return ok;
}

// El acceso directo con el nombre de antes de la 0.51 (OLD_APP_NAME), si va a esta misma instalación: ya lo sustituye
// el nuevo. Uno que vaya a otra carpeta se queda.
static void remove_old_link(REFKNOWNFOLDERID where, const wchar_t *dest) {
    wchar_t lnk[1300], exe[1300], target[1300] = L"", test[8];
    if (GetEnvironmentVariableW(L"SMRPG_INSTALADOR_ACCESOS", test, 8)) return;  // (pruebas: no se tocan los de verdad)
    PWSTR dir = NULL;
    if (FAILED(SHGetKnownFolderPath(where, 0, NULL, &dir))) return;
    swprintf(lnk, 1300, L"%ls\\%ls.lnk", dir, OLD_APP_NAME);
    CoTaskMemFree(dir);
    if (GetFileAttributesW(lnk) == INVALID_FILE_ATTRIBUTES) return;
    swprintf(exe, 1300, L"%ls\\smrpg-recomp.exe", dest);
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) {
        if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf)) && SUCCEEDED(IPersistFile_Load(pf, lnk, STGM_READ)))
            IShellLinkW_GetPath(sl, target, 1300, NULL, 0);
        if (pf) IPersistFile_Release(pf);
        IShellLinkW_Release(sl);
    }
    if (target[0] && !_wcsicmp(target, exe))
        logf_("acceso directo de antes %s: %s", u8(lnk), DeleteFileW(lnk) ? "quitado" : "no se pudo quitar");
}

// ======================================================================== la instalación
static wchar_t g_game[1200], g_dest[1200];
static bool g_links = true, g_only_copy, g_auto, g_hidden;
static bool g_ok;

// ¿Se podrá enlazar la copia en vez de copiarla? La misma unidad, y con enlaces duros (NTFS sí; FAT32 y exFAT no). Si
// al final alguno falla, put_game vuelve a mirar el espacio antes de copiar.
static bool links_possible(const wchar_t *game, const wchar_t *dest) {
    wchar_t va[MAX_PATH], vb[MAX_PATH];
    DWORD flags = 0;
    if (!game[0] || !dest[0] || !GetVolumePathNameW(game, va, MAX_PATH) || !GetVolumePathNameW(dest, vb, MAX_PATH) ||
        _wcsicmp(va, vb))
        return false;
    return GetVolumeInformationW(vb, NULL, 0, NULL, NULL, &flags, NULL, 0) && (flags & FILE_SUPPORTS_HARD_LINKS);
}

// game: la copia elegida (en la misma unidad, con enlaces duros, no ocupa sitio otra vez)
static bool check_dest(const wchar_t *dest, const wchar_t *game) {
    if (!dest[0] || dest[1] != L':' || dest[2] != L'\\' || !dest[3])  // (ni la raíz de una unidad)
        return fail(L"Elige una carpeta de instalación completa (con su unidad, como D:\\Juegos\\%ls).", APP_NAME);
    if (wcslen(dest) > MAX_DEST_CHARS)
        return fail(L"La carpeta de instalación tiene una ruta demasiado larga (%d caracteres; como mucho %d): elige una más corta.",
                    (int)wcslen(dest), MAX_DEST_CHARS);
    if (!mkdirs(dest)) return fail(L"No se pudo crear la carpeta %ls (error %lu).", dest, GetLastError());
    wchar_t t[1300];
    swprintf(t, 1300, L"%ls\\.prueba_escritura", dest);
    HANDLE h = CreateFileW(t, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return fail(L"No se puede escribir en %ls: elige una carpeta tuya (no Archivos de programa).", dest);
    CloseHandle(h);
    ULARGE_INTEGER free_b;
    if (GetDiskFreeSpaceExW(dest, &free_b, NULL, NULL)) {
        uint64_t have = 0, extra = 0;  // (el juego de una instalación anterior, o enlazado, no ocupa otra vez)
        wchar_t m[1300];
        swprintf(m, 1300, L"%ls\\extracted\\exefs\\main", dest);
        if (file_size(m, &have) || links_possible(game, dest)) extra = 6800ull << 20;
        if (free_b.QuadPart + extra < NEED_BYTES)
            return fail(L"En esa unidad quedan %.1f GB libres y hacen falta unos %.0f GB: elige otra carpeta o haz sitio.",
                        free_b.QuadPart / 1e9, NEED_BYTES / 1e9);
    }
    return true;
}

static DWORD WINAPI install_thread(void *arg) {
    (void)arg;
    g_error[0] = 0;
    g_ok = false;
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    DWORD t0 = GetTickCount();
    if (check_dest(g_dest, g_game)) {
        open_log(g_dest);
        logf_("juego: %s; destino: %s", u8(g_game), u8(g_dest));
        bool desk = false, start = false;
        Game game;
        memset(&game, 0, sizeof game);
        if (g_only_copy) {
            g_ok = check_game(g_game, &game) && put_game(&game, g_dest, 0, 1);
        } else if (game_running(g_dest)) {
            fail(L"El juego de esa carpeta está abierto: ciérralo y vuelve a pulsar Instalar.");
        } else {
            // primero la copia del juego (comprobada entera antes de escribir nada): si no vale, una instalación anterior
            // se queda como estaba, con su programa; después el programa y la construcción
            g_ok = check_game(g_game, &game) && put_game(&game, g_dest, 0.01, 0.26);
            bool was_047 = g_ok && is_047(g_dest);  // (antes de copiar el programa: su LEEME.txt lo dice)
            g_ok = g_ok && install_payload(g_dest, 0.26, 0.30);
            if (g_ok && was_047) remove_old_parts(g_dest);
            g_ok = g_ok && build_game(g_dest, 0.30, 0.99);
            if (g_ok && g_links) {
                set_status(0.995, L"Creando los accesos directos…");
                desk = make_link(&FOLDERID_Desktop, L"escritorio", g_dest);
                start = make_link(&FOLDERID_Programs, L"inicio", g_dest);
                if (desk) remove_old_link(&FOLDERID_Desktop, g_dest);
                if (start) remove_old_link(&FOLDERID_Programs, g_dest);
            }
        }
        glist_free(&game.files);
        if (g_ok) {
            DWORD s = (GetTickCount() - t0) / 1000;
            wchar_t links[400] = L"";
            if (g_links && !g_only_copy)
                swprintf(links, 400, desk ? L" Tienes el acceso directo en el escritorio."
                                     : start ? L" Tienes el acceso directo en el menú Inicio (en el escritorio no se pudo crear)."
                                             : L" No se pudieron crear los accesos directos: el juego es %ls\\smrpg-recomp.exe --play.", g_dest);
            set_status(1, L"¡Listo! Instalado en %u min %u s.%ls", s / 60, s % 60, links);
        }
    }
    if (!g_ok && !g_error[0]) fail(L"Error inesperado: mira instalacion.log en la carpeta de instalación.");
    if (!g_ok) set_status(-1, g_cancel ? L"Cancelado: lo ya hecho se guarda (volver a instalar sigue desde ahí)."
                                       : L"No se ha podido instalar: %ls", g_error);
    if (SUCCEEDED(co)) CoUninitialize();
    if (g_wnd) PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
    return 0;
}

// ======================================================================== la ventana
enum { ID_GAME = 100, ID_GAME_B, ID_DEST, ID_DEST_B, ID_LINKS, ID_GO, ID_PLAY, ID_PROG, ID_STATUS, ID_T0, ID_T1, ID_L1,
       ID_L2, ID_I1, ID_I2 };
static HFONT g_font, g_font_b, g_font_s;
static int g_dpi = 96;
static HANDLE g_thread;
static bool g_running, g_finished, g_failed;
#define S(x) MulDiv((x), g_dpi, 96)
#define CLIENT_W 640  // (el área de la ventana, en píxeles a 96 ppp)
#define CLIENT_H 374

// El tamaño de la ventana con su DPI (g_dpi): el que da CreateWindowExW sale del DPI del sistema, que no es el de un
// monitor con otra escala ni el de ahora si se cambió la escala sin volver a entrar en Windows. r: la posición que
// propone WM_DPICHANGED (si no, la de ahora).
static void fit_window(HWND w, const RECT *r) {
    RECT c = {0, 0, S(CLIENT_W), S(CLIENT_H)};
    AdjustWindowRectExForDpi(&c, (DWORD)GetWindowLongW(w, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(w, GWL_EXSTYLE), (UINT)g_dpi);
    if (r) SetWindowPos(w, NULL, r->left, r->top, c.right - c.left, c.bottom - c.top, SWP_NOZORDER | SWP_NOACTIVATE);
    else SetWindowPos(w, NULL, 0, 0, c.right - c.left, c.bottom - c.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void set_fonts(void) {
    if (g_font) DeleteObject(g_font), DeleteObject(g_font_b), DeleteObject(g_font_s);
    g_font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_b = CreateFontW(-S(22), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_s = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void layout(HWND w) {
    struct { int id, x, y, cx, cy; HFONT *f; } L[] = {
        {ID_T0, 20, 14, 600, 32, &g_font_b},
        {ID_T1, 20, 48, 600, 22, &g_font_s},
        {ID_L1, 20, 82, 600, 22, &g_font},     {ID_GAME, 20, 106, 494, 26, &g_font}, {ID_GAME_B, 522, 105, 98, 28, &g_font},
        {ID_I1, 20, 135, 600, 34, &g_font_s},
        {ID_L2, 20, 176, 600, 22, &g_font},    {ID_DEST, 20, 200, 494, 26, &g_font}, {ID_DEST_B, 522, 199, 98, 28, &g_font},
        {ID_I2, 20, 229, 600, 20, &g_font_s},
        {ID_LINKS, 20, 258, 600, 24, &g_font},
        {ID_PROG, 20, 294, 600, 18, NULL},
        {ID_STATUS, 20, 318, 490, 44, &g_font_s},
        {ID_PLAY, 412, 322, 100, 32, &g_font}, {ID_GO, 520, 322, 100, 32, &g_font},
    };
    for (size_t i = 0; i < sizeof L / sizeof *L; i++) {
        HWND c = GetDlgItem(w, L[i].id);
        SetWindowPos(c, NULL, S(L[i].x), S(L[i].y), S(L[i].cx), S(L[i].cy), SWP_NOZORDER | SWP_NOACTIVATE);
        if (L[i].f) SendMessageW(c, WM_SETFONT, (WPARAM)*L[i].f, TRUE);
    }
}

static void show_free_space(HWND w) {
    wchar_t d[1200], gm[1200], gn[1200], t[400] = L"";
    GetDlgItemTextW(w, ID_DEST, d, 1200);
    GetDlgItemTextW(w, ID_GAME, gm, 1200);
    norm_path(gn, gm);
    wchar_t root[8] = {0};
    if (d[0] && d[1] == L':') {
        swprintf(root, 8, L"%lc:\\", d[0]);
        ULARGE_INTEGER fb;
        // (con la copia en la misma unidad y enlaces duros, el juego no ocupa otra vez: como check_dest)
        uint64_t need = NEED_BYTES - (links_possible(gn, root) ? 6800ull << 20 : 0);
        if (GetDiskFreeSpaceExW(root, &fb, NULL, NULL))
            swprintf(t, 400, L"Necesita unos %.0f GB libres; en %ls hay %.0f GB. Los guardados y ajustes se quedan en esta carpeta.",
                     need / 1e9, root, fb.QuadPart / 1e9);
    }
    SetDlgItemTextW(w, ID_I2, t);
}

// Debajo del campo de la copia: si es Super Mario RPG 1.0.0 (lo rápido, exefs; la RomFS se comprueba al instalar)
static bool g_game_bad;
static void describe_game(HWND w) {
    wchar_t typed[1200], picked[1200], root[1200], t[2100], romfs[1300], saved[2048];
    GetDlgItemTextW(w, ID_GAME, typed, 1200);
    norm_path(picked, typed);
    wcscpy(saved, g_error);  // (sin tocar el motivo de una instalación anterior ni su registro)
    g_error[0] = 0;
    g_ui_check = true;
    g_game_bad = false;
    if (!picked[0]) {
        wcscpy(t, L"La carpeta que trae exefs y romfs (también vale la de una instalación anterior).");
    } else if (game_root(picked, root, 1200) && check_exefs(root)) {
        swprintf(romfs, 1300, L"%ls\\romfs", root);
        g_game_bad = !is_dir(romfs);
        wcscpy(t, g_game_bad ? L"Junto a exefs falta la carpeta romfs."
                             : L"Super Mario RPG 1.0.0. Los archivos de la RomFS se comprueban al instalar.");
    } else {
        g_game_bad = true;
        swprintf(t, 2100, L"%ls", g_error[0] ? g_error : L"Esa carpeta no se puede leer.");
    }
    g_ui_check = false;
    wcscpy(g_error, saved);
    SetDlgItemTextW(w, ID_I1, t);
    InvalidateRect(GetDlgItem(w, ID_I1), NULL, TRUE);
    show_free_space(w);  // (con la copia en la misma unidad hace falta menos)
}

// Con la copia sin elegir y una instalación en la carpeta de destino: su extracted\ (para actualizar no hace falta más)
static void suggest_game(HWND w) {
    wchar_t g[8], d[1200], m[1300];
    GetDlgItemTextW(w, ID_GAME, g, 8);
    GetDlgItemTextW(w, ID_DEST, d, 1200);
    if (g[0] || !d[0]) return;
    swprintf(m, 1300, L"%ls\\extracted\\exefs\\main", d);
    if (GetFileAttributesW(m) == INVALID_FILE_ATTRIBUTES) return;
    SetDlgItemTextW(w, ID_GAME, d);
    describe_game(w);
}

static void set_running(HWND w, bool on) {
    int ids[] = {ID_GAME, ID_GAME_B, ID_DEST, ID_DEST_B, ID_LINKS};
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++) EnableWindow(GetDlgItem(w, ids[i]), !on);
    SetDlgItemTextW(w, ID_GO, on ? L"Cancelar" : g_finished ? L"Cerrar" : L"Instalar");
    ShowWindow(GetDlgItem(w, ID_PLAY), !on && g_finished && g_ok && !g_only_copy ? SW_SHOW : SW_HIDE);
}

static void start_install(HWND w) {
    wchar_t typed[1200];
    GetDlgItemTextW(w, ID_GAME, typed, 1200);
    norm_path(g_game, typed);
    GetDlgItemTextW(w, ID_DEST, typed, 1200);
    norm_path(g_dest, typed);  // (como --destino: barras normales, comillas, barra final)
    g_links = IsDlgButtonChecked(w, ID_LINKS) == BST_CHECKED;
    if (!g_game[0] || !is_dir(g_game)) {
        MessageBoxW(w, L"Elige la carpeta de tu copia del juego, ya extraída (la que trae exefs y romfs).", APP_NAME, MB_ICONWARNING);
        return;
    }
    if (!g_dest[0]) {
        MessageBoxW(w, L"Elige la carpeta de instalación.", APP_NAME, MB_ICONWARNING);
        return;
    }
    g_cancel = 0, g_finished = false, g_failed = false, g_running = true;
    set_running(w, true);
    g_thread = CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
}

// Una carpeta, con el selector de Windows: la de la copia del juego (ID_GAME) o la de instalación (pick_dest)
static bool pick_folder(HWND w, const wchar_t *title, wchar_t *out, size_t cap) {
    IFileOpenDialog *d = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) return false;
    DWORD o = 0;
    IFileOpenDialog_GetOptions(d, &o);
    IFileOpenDialog_SetOptions(d, o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IFileOpenDialog_SetTitle(d, title);
    IShellItem *it = NULL;
    PWSTR p = NULL;
    bool ok = SUCCEEDED(IFileOpenDialog_Show(d, w)) && SUCCEEDED(IFileOpenDialog_GetResult(d, &it)) &&
              SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p));
    if (ok) swprintf(out, cap, L"%ls", p), CoTaskMemFree(p);
    if (it) IShellItem_Release(it);
    IFileOpenDialog_Release(d);
    return ok;
}

static void pick_game(HWND w) {
    wchar_t p[1200];
    if (!pick_folder(w, L"La carpeta de tu copia de Super Mario RPG, ya extraída (con exefs y romfs)", p, 1200)) return;
    SetDlgItemTextW(w, ID_GAME, p);
    describe_game(w);
}

static void pick_dest(HWND w) {
    wchar_t p[1200], title[200];
    swprintf(title, 200, L"Dónde instalar %ls", APP_NAME);
    if (pick_folder(w, title, p, 1200)) {
        wchar_t dest[1300], exe[1300];
        swprintf(exe, 1300, L"%ls\\smrpg-recomp.exe", p);
        const wchar_t *base = wcsrchr(p, L'\\');
        // una carpeta con una instalación, o que ya se llama como el programa (o como antes): tal cual; si no, una
        // carpeta dentro
        if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES ||
            (base && (!_wcsicmp(base + 1, APP_NAME) || !_wcsicmp(base + 1, OLD_APP_NAME))))
            swprintf(dest, 1300, L"%ls", p);
        else
            swprintf(dest, 1300, p[wcslen(p) - 1] == L'\\' ? L"%ls%ls" : L"%ls\\%ls", p, APP_NAME);
        SetDlgItemTextW(w, ID_DEST, dest);
        show_free_space(w);
        suggest_game(w);
    }
}

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x2
#endif
static void save_bmp(HWND w, const wchar_t *path) {
    RECT r;
    GetWindowRect(w, &r);
    int cx = r.right - r.left, cy = r.bottom - r.top;
    HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), cx, -cy, 1, 32, BI_RGB}};
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mdc, bm);
    RedrawWindow(w, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);  // (pintada ya, entera)
    {   // (pruebas) qué controles hay y cómo: junto a la captura, <captura>.txt
        wchar_t tp[1300];
        swprintf(tp, 1300, L"%ls.txt", path);
        FILE *tf = _wfopen(tp, L"w");
        if (tf) {
            RECT cr;
            GetClientRect(w, &cr);
            fprintf(tf, "ventana %ldx%ld visible %d, dpi %d\n", cr.right, cr.bottom, IsWindowVisible(w), g_dpi);
            for (HWND c = GetWindow(w, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
                RECT rr;
                GetWindowRect(c, &rr);
                MapWindowPoints(NULL, w, (POINT *)&rr, 2);
                fprintf(tf, "  %d: %ld,%ld %ldx%ld visible %d\n", GetDlgCtrlID(c), rr.left, rr.top, rr.right - rr.left,
                        rr.bottom - rr.top, IsWindowVisible(c));
            }
            fclose(tf);
        }
    }
    PrintWindow(w, mdc, 0);  // (WM_PRINT: la ventana fuera de la pantalla; la captura de DWM salía en blanco)
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof fh + sizeof bi.bmiHeader + cx * cy * 4), 0, 0, sizeof fh + sizeof bi.bmiHeader};
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        write_all(f, &fh, sizeof fh), write_all(f, &bi.bmiHeader, sizeof bi.bmiHeader), write_all(f, bits, (size_t)cx * cy * 4);
        CloseHandle(f);
    }
    SelectObject(mdc, old);
    DeleteObject(bm), DeleteDC(mdc), ReleaseDC(NULL, sdc);
}

static wchar_t g_capture[1200];

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g_dpi = (int)GetDpiForWindow(w);
        set_fonts();
        fit_window(w, NULL);  // (el tamaño con el DPI de su monitor: el del sistema puede ser otro)
        HINSTANCE hi = ((CREATESTRUCTW *)lp)->hInstance;
#define CTRL(cls, text, style, id) CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | (style), 0, 0, 10, 10, w, (HMENU)(INT_PTR)(id), hi, NULL)
#define EDIT(id) CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, w, (HMENU)(INT_PTR)(id), hi, NULL)
        CTRL(L"STATIC", APP_NAME, 0, ID_T0);
        CTRL(L"STATIC", L"El port de PC, desde tu copia del juego ya extraída: el instalador no lleva nada del juego.", 0, ID_T1);
        CTRL(L"STATIC", L"1.  Tu copia del juego, ya extraída (versión 1.0.0)", 0, ID_L1);
        EDIT(ID_GAME);
        CTRL(L"BUTTON", L"Examinar…", WS_TABSTOP, ID_GAME_B);
        CTRL(L"STATIC", L"", 0, ID_I1);
        CTRL(L"STATIC", L"2.  Carpeta de instalación", 0, ID_L2);
        EDIT(ID_DEST);
        CTRL(L"BUTTON", L"Examinar…", WS_TABSTOP, ID_DEST_B);
        CTRL(L"STATIC", L"", 0, ID_I2);
        CTRL(L"BUTTON", L"Crear accesos directos (escritorio y menú Inicio)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_LINKS);
        CTRL(PROGRESS_CLASSW, L"", 0, ID_PROG);
        CTRL(L"STATIC", L"Elige la carpeta de tu copia del juego y pulsa Instalar. Tarda de 10 a 20 minutos: el código del juego se "
                        L"construye en tu PC.", 0, ID_STATUS);
        CTRL(L"BUTTON", L"Jugar", WS_TABSTOP, ID_PLAY);
        CTRL(L"BUTTON", L"Instalar", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_GO);
        SendDlgItemMessageW(w, ID_PROG, PBM_SETRANGE32, 0, 1000);
        CheckDlgButton(w, ID_LINKS, g_links ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(w, ID_GAME, g_game);
        SetDlgItemTextW(w, ID_DEST, g_dest);
        ShowWindow(GetDlgItem(w, ID_PLAY), SW_HIDE);
        layout(w);
        show_free_space(w);
        describe_game(w);
        suggest_game(w);  // (una instalación en la carpeta propuesta: la actualización)
        DragAcceptFiles(w, TRUE);
        if (g_capture[0]) {  // (SMRPG_CAPTURA_MS: cuánto esperar antes de la captura; 1500 ms)
            wchar_t ms[16] = L"";
            GetEnvironmentVariableW(L"SMRPG_CAPTURA_MS", ms, 16);
            SetTimer(w, 2, ms[0] ? (UINT)_wtoi(ms) : 1500, NULL);
        } else if (g_auto) PostMessageW(w, WM_COMMAND, ID_GO, 0);
        return 0;
    }
    case WM_TIMER:
        if (wp == 2) { KillTimer(w, 2); save_bmp(w, g_capture); DestroyWindow(w); }
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        set_fonts();
        fit_window(w, (RECT *)lp);  // (la posición que propone Windows, el tamaño con el DPI nuevo)
        layout(w);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        int id = GetDlgCtrlID((HWND)lp);
        SetBkMode(dc, TRANSPARENT);
        if (id == ID_T0) SetTextColor(dc, RGB(196, 30, 58));
        else if (id == ID_I1 && g_game_bad) SetTextColor(dc, RGB(176, 0, 0));
        else if (id == ID_I1 || id == ID_I2 || id == ID_T1) SetTextColor(dc, RGB(90, 90, 90));
        else if (id == ID_STATUS && g_failed) SetTextColor(dc, RGB(176, 0, 0));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_DROPFILES: {
        wchar_t f[1200];
        if (!g_running && DragQueryFileW((HDROP)wp, 0, f, 1200)) {  // (una carpeta; un archivo, la suya)
            if (!is_dir(f)) {
                wchar_t *s = wcsrchr(f, L'\\');
                if (s) s[s == f + 2 ? 1 : 0] = 0;  // (en la raíz de una unidad, «D:\»)
            }
            SetDlgItemTextW(w, ID_GAME, f);
            describe_game(w);
        }
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_GAME_B: pick_game(w); return 0;
        case ID_DEST_B: pick_dest(w); return 0;
        case ID_GAME:
            if (HIWORD(wp) == EN_KILLFOCUS) describe_game(w);  // (escrita a mano)
            return 0;
        case ID_DEST:
            if (HIWORD(wp) == EN_KILLFOCUS) show_free_space(w), suggest_game(w);
            return 0;
        case ID_PLAY: {
            wchar_t exe[1300];
            swprintf(exe, 1300, L"%ls\\smrpg-recomp.exe", g_dest);
            ShellExecuteW(w, NULL, exe, L"--play", g_dest, SW_SHOWNORMAL);
            DestroyWindow(w);
            return 0;
        }
        case ID_GO:
            if (g_running) {
                if (MessageBoxW(w, L"¿Cancelar la instalación? Lo ya hecho se guarda: volver a instalar sigue desde ahí.",
                                APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES)
                    InterlockedExchange(&g_cancel, 1);
            } else if (g_finished && g_ok) {
                DestroyWindow(w);
            } else {
                start_install(w);
            }
            return 0;
        }
        break;
    case WM_APP_PROGRESS: {
        wchar_t s[512];
        double f;
        EnterCriticalSection(&g_cs);
        wcscpy(s, g_status);
        f = g_frac;
        LeaveCriticalSection(&g_cs);
        SetDlgItemTextW(w, ID_STATUS, s);
        SendDlgItemMessageW(w, ID_PROG, PBM_SETPOS, (WPARAM)(f * 1000), 0);
        return 0;
    }
    case WM_APP_DONE:
        g_running = false, g_finished = g_ok;  // (si falla, se puede corregir y volver a intentar)
        g_failed = !g_ok && !g_cancel;
        if (g_thread) WaitForSingleObject(g_thread, INFINITE), CloseHandle(g_thread), g_thread = NULL;
        SendMessageW(w, WM_APP_PROGRESS, 0, 0);
        set_running(w, false);
        InvalidateRect(GetDlgItem(w, ID_STATUS), NULL, TRUE);
        if (!g_ok) {
            SetDlgItemTextW(w, ID_GO, g_cancel ? L"Instalar" : L"Reintentar");
            if (!g_auto && !g_cancel) MessageBoxW(w, g_error, L"No se ha podido instalar", MB_ICONERROR);
        }
        if (g_auto) DestroyWindow(w);
        return 0;
    case WM_CLOSE:
        if (g_running) {
            if (MessageBoxW(w, L"¿Cancelar la instalación y cerrar? Lo ya hecho se guarda.", APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            InterlockedExchange(&g_cancel, 1);
            if (g_thread) WaitForSingleObject(g_thread, 15000);  // (lo que esté escribiendo; la compilación no se espera)
        }
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// Salir. Con el constructor cargado, el proceso termina en seco: sus hilos pueden estar acabando un archivo (al cancelar
// no se esperan), y los destructores de LLVM y de Python no deben correr con ellos cortados a medias. Los registros van
// con WriteFile, sin búfer: no queda nada por escribir.
static int salir(int code) {
    if (g_construir) TerminateProcess(GetCurrentProcess(), (UINT)code);
    return code;
}

// Control Flow Guard (#337 de LAS Recomp): unicorn, que el generador del código del juego usa dentro de este proceso,
// sale con longjmp del código que genera, y con CFG activo Windows lo toma por un ataque y cierra el proceso de golpe
// (0xC0000409; reproducido con CFG forzado por la directiva del proceso). El instalador no lleva CFG en su cabecera,
// pero Windows puede imponerlo a todos los programas (Protección contra vulnerabilidades): entonces se vuelve a abrir
// a sí mismo con CFG apagado por la directiva de creación del proceso, que gana a ese ajuste, y este se cierra (con
// --auto espera y devuelve su código). Solo uno de los dos trabaja.
static bool cfg_active(void) {
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY p;
    memset(&p, 0, sizeof p);
    return GetProcessMitigationPolicy(GetCurrentProcess(), ProcessControlFlowGuardPolicy, &p, sizeof p) &&
           p.EnableControlFlowGuard;
}

static bool relaunch_without_cfg(bool wait, DWORD *code) {
    SIZE_T sz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &sz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(sz);
    DWORD64 pol = PROCESS_CREATION_MITIGATION_POLICY_CONTROL_FLOW_GUARD_ALWAYS_OFF;
    if (!al || !InitializeProcThreadAttributeList(al, 1, 0, &sz)) {
        free(al);
        return false;
    }
    bool ok = UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &pol, sizeof pol, NULL, NULL);
    static wchar_t me[1200], cmd[32768];
    ok = ok && GetModuleFileNameW(NULL, me, 1200);
    swprintf(cmd, 32768, L"%ls --sin-cfg", GetCommandLineW());
    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.lpAttributeList = al;
    PROCESS_INFORMATION pi;
    ok = ok && CreateProcessW(me, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(al);
    free(al);
    if (!ok) return false;
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (!GetExitCodeProcess(pi.hProcess, code)) *code = 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// %LOCALAPPDATA%\Programs\SMRPG Recomp, o la carpeta con el nombre de antes si ahí hay una instalación (actualizarla)
static void default_dest(void) {
    wchar_t p[1200], old[1300];
    if (!ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Programs", p, 1200)) return;
    swprintf(old, 1300, L"%ls\\%ls\\smrpg-recomp.exe", p, OLD_APP_NAME);
    swprintf(g_dest, 1200, L"%ls\\%ls", p, GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES ? OLD_APP_NAME : APP_NAME);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev, (void)cmdline;
    InitializeCriticalSection(&g_cs);
    int argc = 0;
    bool relaunched = false;  // (--sin-cfg: ya es la copia abierta sin Control Flow Guard)
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--empaquetar") && i + 2 < argc) return pack(argv[i + 1], argv[i + 2]);
        if (!wcscmp(argv[i], L"--juego") && i + 1 < argc) norm_path(g_game, argv[++i]);
        else if (!wcscmp(argv[i], L"--destino") && i + 1 < argc) norm_path(g_dest, argv[++i]);
        else if (!wcscmp(argv[i], L"--captura") && i + 1 < argc) norm_path(g_capture, argv[++i]);
        else if (!wcscmp(argv[i], L"--sin-accesos")) g_links = false;
        else if (!wcscmp(argv[i], L"--auto")) g_auto = true;
        else if (!wcscmp(argv[i], L"--oculto")) g_hidden = true;
        else if (!wcscmp(argv[i], L"--solo-copiar")) g_only_copy = true;
        else if (!wcscmp(argv[i], L"--sin-cfg")) relaunched = true;
    }
    if (!g_capture[0] && cfg_active()) {  // (#337: ver cfg_active)
        DWORD code = 1;
        if (!relaunched && relaunch_without_cfg(g_auto, &code)) return g_auto ? (int)code : 0;
        g_cfg_blocked = true;  // (build_game lo explica: el constructor ni se carga)
    }
    if (!g_dest[0]) default_dest();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (g_auto && g_hidden) {  // (pruebas, sin ventana)
        install_thread(NULL);
        return salir(g_ok ? 0 : 1);
    }
    if (!g_capture[0] && !g_only_copy && !payload_present())
        MessageBoxW(NULL, L"Este instalador está incompleto: le falta el programa (se construye con `make instalador`).",
                    APP_NAME, MB_ICONWARNING);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"SMRPGRecompInstalador";
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    UINT dpi = GetDpiForSystem();
    RECT r = {0, 0, MulDiv(CLIENT_W, (int)dpi, 96), MulDiv(CLIENT_H, (int)dpi, 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    // (pruebas: --captura, o SMRPG_VENTANA_FUERA=1 con --auto: la ventana entera, con sus hilos, sin que se vea)
    wchar_t fuera[4] = L"";
    bool offscreen = g_capture[0] || (GetEnvironmentVariableW(L"SMRPG_VENTANA_FUERA", fuera, 4) && fuera[0] == L'1');
    if (offscreen) x = -4000, y = -4000;
    wchar_t title[128];
    swprintf(title, 128, L"Instalar %ls %hs", APP_NAME, INSTALADOR_VERSION);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, title, style, x, y, r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
    if (!g_wnd) return 1;
    ShowWindow(g_wnd, offscreen ? SW_SHOWNOACTIVATE : show);
    UpdateWindow(g_wnd);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return salir(g_ok || g_capture[0] ? 0 : 1);
}

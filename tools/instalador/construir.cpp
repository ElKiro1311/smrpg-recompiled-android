// construir.dll: construye el código del juego (extracted\lasgame.dll) DENTRO del proceso que la carga, el instalador
// (tools/instalador/instalador.c), sin procesos hijos (antes: smrpg-recomp.exe --build-game, el generador congelado y
// un zig por cada archivo, 30 procesos a la vez; ahora todo dentro de un solo proceso, sin procesos hijos):
//   1. el generador de LAS Recomp (gen.py) en un Python embebido (python312.dll de la distribución «embeddable» de
//      python.org, aislada por su python312._pth: no mira el Python ni las variables del PC), en un hilo: exefs\sdk y
//      exefs\main a C, como `make gen`;
//   2. clang como biblioteca (libclang-cpp y libLLVM de MSYS2: las mismas herramientas y la misma línea -cc1 que
//      `make`) compila cada trozo de C en un grupo de hilos, a medida que el generador los escribe;
//   3. lld como biblioteca (su controlador MinGW, la línea de enlace de `make`) enlaza lasgame.dll contra la biblioteca
//      de importación de smrpg-recomp.exe, con game_overrides.o antes que el código generado: con varias definiciones
//      de una función gana la primera (--allow-multiple-definition).
// Todo lo que usa va con ella, en construir\: Python y los paquetes del generador, las cabeceras de clang y de
// MinGW-w64 que incluye el código del juego, el CRT de MinGW-w64 y libgcc para enlazar. Del PC de quien instala, solo
// Windows (10 u 11 de 64 bits).
// La CPU: el código del juego se compila para x86-64-v3 (AVX2, FMA, BMI2..., como `make`) si la CPU lo tiene; si no,
// para x86-64-v2 o x86-64 (más lento, pero funciona): lasgame.dll.cpu lo dice y smrpg-recomp.exe lo comprueba.
// Reanudar: lo generado se reutiliza si el juego y el generador son los mismos (gen.key) y cada objeto si su C no
// cambió (<trozo>.src) ni la forma de compilar (base.txt). Registro: <carpeta>\construccion.log.
#include <windows.h>
#include <bcrypt.h>
#include <cpuid.h>
#include <psapi.h>

#include <malloc.h>
#include <setjmp.h>

#include <algorithm>
#include <exception>
#include <new>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "lld/Common/Driver.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

LLD_HAS_DRIVER(mingw)

#ifndef GAMELIB_ID
#define GAMELIB_ID "dev"
#endif
#define CONSTRUIR_VERSION "construir 1"

typedef void (*AvanceFn)(void *ctx, double frac, const wchar_t *paso);

namespace {

// ------------------------------------------------------------------ texto, archivos, registro
std::string u8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
std::wstring wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::wstring lp(const std::wstring &p) {  // \\?\ delante: rutas largas
    return p.compare(0, 4, L"\\\\?\\") == 0 ? p : L"\\\\?\\" + p;
}
bool exists(const std::wstring &p) { return GetFileAttributesW(lp(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
bool read_file(const std::wstring &p, std::string &out) {
    out.clear();
    HANDLE h = CreateFileW(lp(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart < (1ll << 31);
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = sz.QuadPart == 0 || (ReadFile(h, &out[0], (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart);
    }
    CloseHandle(h);
    return ok;
}
bool write_file(const std::wstring &p, const std::string &data) {  // (.tmp y se renombra: nunca a medias)
    std::wstring t = p + L".tmp";
    HANDLE h = CreateFileW(lp(t).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &put, NULL) && put == data.size());
    CloseHandle(h);
    return ok && MoveFileExW(lp(t).c_str(), lp(p).c_str(), MOVEFILE_REPLACE_EXISTING);
}
void mkdirs(const std::wstring &p) {
    std::wstring l = lp(p);
    for (size_t i = 7; i < l.size(); i++)
        if (l[i] == L'\\') { l[i] = 0; CreateDirectoryW(l.c_str(), NULL); l[i] = L'\\'; }
    CreateDirectoryW(l.c_str(), NULL);
}
void remove_tree(const std::wstring &dir) {  // (solo lo que esta DLL creó: la caché vieja, .gamebuild)
    DWORD a = GetFileAttributesW(lp(dir).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) {  // (un enlace: se quita el enlace, sin entrar en lo que apunta)
        RemoveDirectoryW(lp(dir).c_str());
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(lp(dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) remove_tree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(lp(p).c_str());
            else DeleteFileW(lp(p).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(lp(dir).c_str());
}
uint64_t file_bytes(const std::wstring &p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(lp(p).c_str(), GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

// "" si BCrypt falla: quien compara toma "" como distinto de todo (si no, cualquier archivo «coincidiría» con cualquiera)
std::string sha256_hex(const std::string &data) {
    static BCRYPT_ALG_HANDLE alg;
    static std::once_flag once;
    std::call_once(once, [] {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0))) alg = NULL;
    });
    unsigned char h[32] = {0};
    if (!alg || !BCRYPT_SUCCESS(BCryptHash(alg, NULL, 0, (PUCHAR)data.data(), (ULONG)data.size(), h, 32))) return "";
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    return hex;
}
std::string file_sha(const std::wstring &p) {
    std::string d;
    return read_file(p, d) ? sha256_hex(d) : std::string();
}

// el contenido de una carpeta entera (sus rutas relativas y lo que tiene cada archivo, en orden)
void list_files(const std::wstring &dir, const std::wstring &rel, std::vector<std::wstring> &out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(lp(dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) list_files(dir + L"\\" + n, rel + n + L"\\", out);
        else out.push_back(rel + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
std::string dir_sha(const std::wstring &dir) {
    std::vector<std::wstring> fs;
    list_files(dir, L"", fs);
    std::sort(fs.begin(), fs.end());
    std::string all;
    for (auto &f : fs) all += u8(f) + " " + file_sha(dir + L"\\" + f) + "\n";
    return sha256_hex(all);
}

struct Log {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::mutex m;
    void open(const std::wstring &p) {
        h = CreateFileW(lp(p).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    }
    void close() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    void operator()(const char *fmt, ...) {
        char b[4096];
        SYSTEMTIME t;
        GetLocalTime(&t);
        int n = snprintf(b, sizeof b, "%02d:%02d:%02d [construir] ", t.wHour, t.wMinute, t.wSecond);
        va_list ap;
        va_start(ap, fmt);
        n += vsnprintf(b + n, sizeof b - n - 2, fmt, ap);
        va_end(ap);
        if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
        b[n++] = '\n';
        std::lock_guard<std::mutex> g(m);
        DWORD w;
        if (h != INVALID_HANDLE_VALUE) WriteFile(h, b, (DWORD)n, &w, NULL);
    }
} g_log;

// ------------------------------------------------------------------ la CPU (CPUID)
std::string cpu_level() {
    char env[32];
    DWORD en = GetEnvironmentVariableA("SMRPG_CONSTRUIR_CPU", env, sizeof env);  // (pruebas: solo los tres niveles)
    if (en && en < sizeof env && (!strcmp(env, "x86-64") || !strcmp(env, "x86-64-v2") || !strcmp(env, "x86-64-v3")))
        return env;
    unsigned a, b, c, d, c1, d1, b7 = 0, c81 = 0;
    if (!__get_cpuid(1, &a, &b, &c1, &d1)) return "x86-64";
    if (__get_cpuid_max(0, NULL) >= 7) __cpuid_count(7, 0, a, b7, c, d);
    if (__get_cpuid(0x80000001, &a, &b, &c81, &d)) {}
    bool sse3 = c1 & 1, ssse3 = c1 & (1 << 9), sse41 = c1 & (1 << 19), sse42 = c1 & (1 << 20), popcnt = c1 & (1 << 23);
    bool cx16 = c1 & (1 << 13), lahf = c81 & 1;
    bool v2 = sse3 && ssse3 && sse41 && sse42 && popcnt && cx16 && lahf;
    bool osxsave = c1 & (1 << 27), avx = c1 & (1 << 28), fma = c1 & (1 << 12), f16c = c1 & (1 << 29), movbe = c1 & (1 << 22);
    bool avx2 = b7 & (1 << 5), bmi1 = b7 & (1 << 3), bmi2 = b7 & (1 << 8), lzcnt = c81 & (1 << 5);
    bool ymm = false;
    if (osxsave) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        ymm = (lo & 6) == 6;  // (el sistema guarda los registros XMM e YMM)
    }
    if (v2 && avx && ymm && avx2 && fma && bmi1 && bmi2 && f16c && movbe && lzcnt) return "x86-64-v3";
    return v2 ? "x86-64-v2" : "x86-64";
}

// ------------------------------------------------------------------ hilos con pila grande, y su red
// clang y LLVM recursan muy hondo en las funciones enormes del código generado: con los 2 MB de un hilo normal de este
// proceso, MemorySSAUpdater::recursePhi desbordó la pila (visto con gdb, 0xC00000FD, el proceso caía sin aviso). El
// clang.exe de MSYS2 (el de `make`) corre con 16 MB y el clang de zig (0.47) con 46: estos hilos reservan 64 MB (solo
// espacio de direcciones; la memoria se compromete a medida que se usa). Van con prioridad por debajo de lo normal, para
// que el PC siga respondiendo mientras compilan.
constexpr SIZE_T kPila = 64u << 20;

DWORD WINAPI hilo_grande_main(void *p) {
    std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()> *>(p));
    ULONG g = 256u << 10;  // (si aun así se acabase la pila, queda sitio para recogerlo: guard_veh)
    SetThreadStackGuarantee(&g);
    (*f)();
    return 0;
}

HANDLE hilo_grande(std::function<void()> fn) {
    auto *p = new std::function<void()>(std::move(fn));
    HANDLE h = CreateThread(NULL, kPila, hilo_grande_main, p, STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED, NULL);
    if (!h) {
        delete p;
        return NULL;
    }
    SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(h);
    return h;
}

// La red: un fallo dentro de clang/LLVM en un hilo de compilación (una excepción de Windows, o un error grave de LLVM,
// que si no llamaría a exit y cerraría el instalador) vuelve a compile_unit como error de ese archivo. Solo en esos
// hilos (TLS): en el del generador, unicorn provoca y recoge sus propios fallos de acceso (visto con gdb), y el
// CrashRecoveryContext de LLVM, con su manejador para todo el proceso, se desactivaba entero con el primero de ellos.
struct Guard {
    jmp_buf jb;
    DWORD code;
    char why[300];
};
DWORD g_guard_tls = TLS_OUT_OF_INDEXES;
PVOID g_guard_veh;
// Tras recoger un fallo, el proceso queda tocado: lo que clang tenía a medias no se liberó, y si el fallo llegó con un
// cerrojo tomado (del montón, de LLVM) sigue tomado. Las llamadas siguientes no construyen: piden reabrir el
// instalador (lo compilado se guarda, así que sale barato)
std::atomic<bool> g_tainted{false};

Guard *guard_here() {
    if (g_guard_tls == TLS_OUT_OF_INDEXES) return nullptr;
    DWORD le = GetLastError();  // (TlsGetValue lo pone a 0: no tocárselo al código del hilo que trata su excepción)
    Guard *g = static_cast<Guard *>(TlsGetValue(g_guard_tls));
    SetLastError(le);
    return g;
}

LONG CALLBACK guard_veh(EXCEPTION_POINTERS *e) {
    Guard *g = guard_here();
    if (!g) return EXCEPTION_CONTINUE_SEARCH;
    switch (e->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        g->code = e->ExceptionRecord->ExceptionCode;
        TlsSetValue(g_guard_tls, NULL);
        longjmp(g->jb, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void guard_fatal(void *, const char *reason, bool) {  // (report_fatal_error, y sin memoria: report_bad_alloc_error)
    if (Guard *g = guard_here()) {
        snprintf(g->why, sizeof g->why, "%s", reason ? reason : "");
        g->code = 0;
        TlsSetValue(g_guard_tls, NULL);
        longjmp(g->jb, 1);
    }
    g_log("error grave de LLVM fuera de una compilación: %s", reason ? reason : "");
    llvm::sys::Process::Exit(1);  // (en el enlace, con el CrashRecoveryContext de lldMain, vuelve a lldMain)
}

void guard_install() {
    static std::once_flag once;
    std::call_once(once, [] {
        g_guard_tls = TlsAlloc();
        g_guard_veh = AddVectoredExceptionHandler(1, guard_veh);
        llvm::install_fatal_error_handler(guard_fatal, nullptr);
        llvm::install_bad_alloc_error_handler(guard_fatal, nullptr);
        // sin memoria en un `new` (std::vector, la IR...): sin excepciones, terminaría con abort sin decir nada
        std::set_new_handler([] { guard_fatal(nullptr, "sin memoria", false); });
        std::set_terminate([] {
            g_log("terminado por un error interno (std::terminate)");
            abort();
        });
    });
}

// fn(ctx) con la red puesta: false si falló (why dice cómo). Tras un fallo, el hilo no debe seguir compilando: lo que
// clang tenía a medias queda sin liberar.
__attribute__((noinline)) bool run_guarded(void (*fn)(void *), void *ctx, std::string &why) {
    Guard g;
    g.code = 0, g.why[0] = 0;
    if (setjmp(g.jb)) {
        if (g.code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        char b[400];
        if (g.code)
            snprintf(b, sizeof b, "excepción 0x%08lX%s", g.code,
                     g.code == EXCEPTION_STACK_OVERFLOW     ? " (se acabó la pila)"
                     : g.code == EXCEPTION_ACCESS_VIOLATION ? " (acceso a memoria no válido)"
                                                            : "");
        else if (!strcmp(g.why, "sin memoria"))
            snprintf(b, sizeof b, "sin memoria");
        else
            snprintf(b, sizeof b, "error grave de LLVM: %s", g.why);
        why = b;
        return false;
    }
    TlsSetValue(g_guard_tls, &g);
    fn(ctx);
    TlsSetValue(g_guard_tls, NULL);
    return true;
}

// ------------------------------------------------------------------ las rutas
struct Paths {
    std::wstring dest, game, work, tools, py, res_inc, mingw_inc, mingw_lib, src, implib;
};

std::wstring module_dir() {
    HMODULE m = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)&module_dir, &m);
    wchar_t p[1200];
    DWORD n = GetModuleFileNameW(m, p, 1200);
    std::wstring s(p, n);
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? s : s.substr(0, k);
}

// ------------------------------------------------------------------ el generador (Python embebido)
struct Py {
    HMODULE dll = NULL;
    int (*IsInitialized)(void) = nullptr;
    void (*InitializeEx)(int) = nullptr;
    void *(*SaveThread)(void) = nullptr;
    int (*GILEnsure)(void) = nullptr;
    void (*GILRelease)(int) = nullptr;
    int (*RunSimpleString)(const char *) = nullptr;
};
Py g_py;
std::mutex g_py_lock;

bool py_load(const std::wstring &dir, std::string &err) {
    std::lock_guard<std::mutex> g(g_py_lock);
    if (g_py.dll) return true;
    HMODULE h = LoadLibraryExW((dir + L"\\python312.dll").c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { err = "no se pudo cargar python312.dll (error " + std::to_string(GetLastError()) + ")"; return false; }
#define PYF(f, n) g_py.f = (decltype(g_py.f))(void *)GetProcAddress(h, n)
    PYF(IsInitialized, "Py_IsInitialized");
    PYF(InitializeEx, "Py_InitializeEx");
    PYF(SaveThread, "PyEval_SaveThread");
    PYF(GILEnsure, "PyGILState_Ensure");
    PYF(GILRelease, "PyGILState_Release");
    PYF(RunSimpleString, "PyRun_SimpleString");
#undef PYF
    if (!g_py.IsInitialized || !g_py.InitializeEx || !g_py.SaveThread || !g_py.GILEnsure || !g_py.GILRelease || !g_py.RunSimpleString) {
        err = "python312.dll no tiene la API esperada";
        return false;
    }
    g_py.dll = h;
    if (!g_py.IsInitialized()) {
        // (aislado también de las variables del PC —PYTHONUTF8, PYTHONMALLOC...—: python312._pth fija las rutas, pero
        // esas se leen antes; las marcas globales de 3.12 lo apagan todo, como python -I -E -s)
        for (const char *flag : {"Py_IgnoreEnvironmentFlag", "Py_IsolatedFlag", "Py_NoUserSiteDirectory"})
            if (int *f = (int *)(void *)GetProcAddress(h, flag)) *f = 1;
        g_py.InitializeEx(0);  // (sin manejadores de señales: es una biblioteca dentro del instalador)
        g_py.SaveThread();     // (suelta el GIL: cada ejecución lo toma con PyGILState_Ensure, desde cualquier hilo)
    }
    return true;
}

// la ruta como expresión de Python: sus bytes UTF-8 en hexadecimal (cualquier carácter, también comillas o barras al final)
std::string pystr(const std::wstring &p) {
    std::string s = u8(p), out = "bytes.fromhex('";
    char h[3];
    for (unsigned char ch : s) snprintf(h, sizeof h, "%02x", ch), out += h;
    return out + "').decode('utf-8')";
}

// gen.py <name> <nso> <base> <outdir> [--hle <hle>]: su progreso (stderr) a <log>, su código de salida a <rc>
int py_gen(const Paths &P, const char *name, const char *base, const std::wstring &log, std::string &err) {
    std::wstring wn = wide(name), nso = P.game + L"\\exefs\\" + wn, out = P.work + L"\\" + wn;
    std::wstring rc = P.work + L"\\gen_" + wn + L".rc";
    DeleteFileW(lp(rc).c_str());
    std::string code =
        "import sys, runpy\n"
        "for _m in ('gen', 'lifter', 'analyze', 'nso', 'passes', 'patches', 'simd_c', 'game_addresses'):\n"
        "    sys.modules.pop(_m, None)\n"
        "sys.argv = ['gen.py', '" + std::string(name) + "', " + pystr(nso) + ", '" + base + "', " + pystr(out) +
        (strcmp(name, "sdk") == 0 ? ", '--hle', " + pystr(P.src + L"\\hle_symbols.txt") : std::string()) + "]\n"
        "_log = open(" + pystr(log) + ", 'w', encoding='utf-8', buffering=1)\n"
        "_old_err, _old_out = sys.stderr, sys.stdout\n"
        "sys.stderr = sys.stdout = _log\n"
        "_rc = 0\n"
        "try:\n"
        "    runpy.run_path(" + pystr(P.py + L"\\gen\\tools\\gen.py") + ", run_name='__main__')\n"
        "except SystemExit as _e:\n"
        "    _rc = _e.code if isinstance(_e.code, int) else (0 if _e.code is None else 1)\n"
        "    if _e.code is not None and not isinstance(_e.code, int):\n"
        "        print(_e.code, file=_log)\n"
        "except BaseException:\n"
        "    import traceback\n"
        "    traceback.print_exc(file=_log)\n"
        "    _rc = 1\n"
        "finally:\n"
        "    sys.stderr, sys.stdout = _old_err, _old_out\n"
        "    _log.close()\n"
        "with open(" + pystr(rc) + ", 'w') as _f:\n"
        "    _f.write(str(_rc))\n";
    int st = g_py.GILEnsure();
    int r = g_py.RunSimpleString(code.c_str());
    g_py.GILRelease(st);
    std::string rcs;
    if (r != 0 || !read_file(rc, rcs)) { err = std::string("el generador (") + name + ") no terminó"; return 1; }
    int v = atoi(rcs.c_str());
    if (v != 0) {
        std::string l;
        read_file(log, l);
        size_t k = l.find_last_not_of("\r\n");
        l = k == std::string::npos ? "" : l.substr(0, k + 1);
        size_t s = l.find_last_of('\n');
        err = std::string("el generador (") + name + ") falló: " + (s == std::string::npos ? l : l.substr(s + 1));
    }
    return v;
}

// lo que el generador lleva escrito: ": progress <hechas>/<todas> files <trozos>" (su registro)
void gen_progress(const std::wstring &log, uint64_t &ci, uint64_t &all, int &files) {
    std::string t;
    if (!read_file(log, t)) return;
    size_t p = t.rfind(": progress ");
    if (p == std::string::npos) return;
    unsigned long long a = 0, b = 0;
    int f = 0;
    if (sscanf(t.c_str() + p, ": progress %llu/%llu files %d", &a, &b, &f) == 3) {
        ci = a, all = b;
        if (f > files) files = f;
    }
}

// ------------------------------------------------------------------ compilar (clang como biblioteca)
struct Unit {
    std::wstring src, obj;
    std::string stem, define;  // define: "-D" extra (gamelib_entry: el id de la biblioteca)
    uint64_t bytes = 0;
};

std::vector<std::string> cc1_args(const Paths &P, const std::string &cpu) {
    // la línea -cc1 de `make` (clang -### con GENFLAGS: -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label
    // -fvisibility=hidden -D_FILE_OFFSET_BITS=64 -march=x86-64-v3), sin -disable-free (aquí la memoria se devuelve: un
    // proceso compila mil archivos) ni rutas de depuración; las cabeceras, las que van en la carpeta construir
    return {"-triple", "x86_64-w64-windows-gnu", "-O1", "-emit-obj", "-clear-ast-before-backend", "-disable-llvm-verifier",
            "-discard-value-names", "-mrelocation-model", "pic", "-pic-level", "2", "-mframe-pointer=none",
            "-relaxed-aliasing", "-fmath-errno", "-ffp-contract=on", "-fno-rounding-math", "-mconstructor-aliases",
            "-funwind-tables=2", "-fno-sized-deallocation", "-fno-use-init-array", "-target-cpu", cpu,
            "-debugger-tuning=gdb", "-resource-dir", u8(P.tools + L"\\clang"), "-D", "_FILE_OFFSET_BITS=64",
            "-I", u8(P.src), "-internal-isystem", u8(P.res_inc), "-internal-isystem", u8(P.mingw_inc),
            "-Wno-return-type", "-Wno-unused-label", "-ferror-limit", "19", "-fvisibility=hidden", "-fno-use-cxa-atexit",
            "-fgnuc-version=4.2.1", "-fskip-odr-check-in-gmf", "-exception-model=seh", "-faddrsig"};
}

// (pruebas de la red: SMRPG_CONSTRUIR_PRUEBA_FALLO=<trozo>:1 escribe en la dirección 0 y <trozo>:2 agota la pila al
// compilar ese trozo, como si fallara clang)
int crash_test_mode(const std::string &stem) {
    char e[96];
    DWORD n = GetEnvironmentVariableA("SMRPG_CONSTRUIR_PRUEBA_FALLO", e, sizeof e);
    if (!n || n >= sizeof e) return 0;
    std::string s = e;
    size_t k = s.find(':');
    return k != std::string::npos && s.substr(0, k) == stem ? atoi(s.c_str() + k + 1) : 0;
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winfinite-recursion"  // (a propósito: agota la pila)
__attribute__((noinline, optnone)) int burn_stack(int n) {
    volatile char buf[4096];
    buf[0] = (char)n;
    return burn_stack(n + 1) + buf[0];
}
#pragma clang diagnostic pop

// un objeto que se puede reutilizar: COFF de x86-64 (un apagón puede dejar el archivo renombrado pero lleno de ceros)
bool obj_ok(const std::wstring &p) {
    HANDLE h = CreateFileW(lp(p).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char m[20] = {0};
    DWORD got = 0;
    bool ok = ReadFile(h, m, sizeof m, &got, NULL) && got == sizeof m && m[0] == 0x64 && m[1] == 0x86;
    CloseHandle(h);
    return ok;
}

bool compile_unit(const std::vector<std::string> &base, const Unit &u, std::string &diag) {
    std::vector<std::string> a = base;
    if (!u.define.empty()) a.push_back("-D"), a.push_back(u.define);
    std::wstring tmp = u.obj + L".tmp";
    std::string name = u8(u.src);
    size_t k = name.find_last_of("\\/");
    a.push_back("-main-file-name"), a.push_back(k == std::string::npos ? name : name.substr(k + 1));
    a.push_back("-o"), a.push_back(u8(tmp));
    a.push_back("-x"), a.push_back("c"), a.push_back(name);
    std::vector<const char *> argv;
    for (auto &s : a) argv.push_back(s.c_str());
    llvm::raw_string_ostream os(diag);
    struct Job {
        std::vector<const char *> *argv;
        llvm::raw_string_ostream *os;
        bool ok;
        int test;
    } job = {&argv, &os, false, crash_test_mode(u.stem)};
    std::string crash;
    bool ran = run_guarded(
        [](void *p) {
            Job &j = *static_cast<Job *>(p);
            if (j.test == 1) *(volatile int *)(uintptr_t)0 = 1;
            if (j.test == 2) burn_stack(0);
            clang::DiagnosticOptions dopts;
            clang::TextDiagnosticPrinter printer(*j.os, dopts);
            clang::DiagnosticsEngine diags(llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>(), dopts, &printer, false);
            auto inv = std::make_shared<clang::CompilerInvocation>();
            if (!clang::CompilerInvocation::CreateFromArgs(*inv, *j.argv, diags)) return;
            clang::CompilerInstance ci(inv);
            // (un sistema de archivos propio por compilación: su directorio de trabajo no se comparte entre hilos)
            ci.createVirtualFileSystem(
                llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>(llvm::vfs::createPhysicalFileSystem().release()), &printer);
            ci.createDiagnostics(&printer, false);
            clang::EmitObjAction act;
            j.ok = ci.ExecuteAction(act) && !ci.getDiagnostics().hasErrorOccurred();
        },
        &job, crash);
    os.flush();
    bool ok = ran && job.ok;
    if (!ran) {
        diag = "clang se detuvo: " + crash + (diag.empty() ? std::string() : "\n" + diag);
        g_tainted = true;  // (lo que clang tenía a medias queda así: ver g_tainted)
    }
    if (ok) {  // (un antivirus puede tener abierto el objeto anterior un momento)
        for (int i = 0; i < 20; i++) {
            if (MoveFileExW(lp(tmp).c_str(), lp(u.obj).c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
            Sleep(100);
        }
        diag = "no se pudo escribir " + u8(u.obj) + " (error " + std::to_string(GetLastError()) + ")";
        ok = false;
    }
    DeleteFileW(lp(tmp).c_str());
    return ok;
}

// ------------------------------------------------------------------ enlazar (lld como biblioteca)
bool g_lld_spent;  // (lld salió por su vía de error grave: no se puede volver a usar en este proceso)

bool link_lib(const Paths &P, const std::vector<std::wstring> &objs, const std::wstring &out, std::string &diag) {
    std::string L = u8(P.mingw_lib);
    // (--threads=1: con más, lld deja un grupo de hilos de LLVM vivo en el proceso, y al salir su destructor estático
    // espera a hilos que Windows ya ha matado: el proceso se quedaba colgado al cerrarse. Medido: enlazar así, 0,6 s)
    std::vector<std::string> a = {"ld.lld", "-m", "i386pep", "--shared", "-Bdynamic", "-e", "DllMainCRTStartup",
                                  "--enable-auto-image-base", "--threads=1", "-o", u8(out), L + "\\dllcrt2.o",
                                  L + "\\crtbegin.o", "-L" + L, u8(P.implib), "--allow-multiple-definition"};
    for (auto &o : objs) a.push_back(u8(o));
    for (const char *l : {"-lmingw32", "-lgcc_s", "-lgcc", "-lmoldname", "-lmingwex", "-lmsvcrt", "-lpthread", "-ladvapi32",
                          "-lshell32", "-luser32", "-lkernel32", "-lmingw32", "-lgcc_s", "-lgcc", "-lmoldname", "-lmingwex",
                          "-lmsvcrt", "-lkernel32"})
        a.push_back(l);
    a.push_back(L + "\\crtend.o");
    std::vector<const char *> argv;
    for (auto &s : a) argv.push_back(s.c_str());
    llvm::raw_string_ostream os(diag);
    // (lld, ante un error grave o al pasar de 20 errores, llama a exit: con el CrashRecoveryContext encendido, lldMain lo
    // recoge y vuelve aquí. Solo durante el enlace: el generador ya acabó, y ningún hilo de compilación sigue vivo)
    llvm::CrashRecoveryContext::Enable();
    lld::Result r = lld::lldMain(argv, os, os, {{lld::MinGW, &lld::mingw::link}});
    llvm::CrashRecoveryContext::Disable();
    os.flush();
    if (!r.canRunAgain) g_lld_spent = true;
    return r.retCode == 0;
}

// ------------------------------------------------------------------ la construcción
// El generador y los hilos de compilación trabajan con su propio estado, en el montón, no con el de la llamada: al
// cancelar o fallar, la llamada vuelve enseguida y ellos acaban por su cuenta lo que tengan entre manos (un hilo no se
// puede cortar a medias sin dejar el proceso tocado, y un trozo grande tarda minutos); la siguiente construcción los
// espera (g_last_gen, g_last_build). Si se cierra el instalador, el proceso entero termina con ellos.
struct GenShared {
    Paths P;
    std::wstring log[2];                       // su registro (si el de la vez anterior no se pudo borrar, otro nombre)
    std::string key;                           // gen.key, que escribe al acabar bien los dos
    std::atomic<bool> started[2] = {{false}, {false}};  // (su registro solo se lee una vez empezado: no el viejo)
    std::atomic<int> files_final[2] = {{0}, {0}};       // los trozos de cada uno, leídos al acabar (antes de state = 1)
    std::atomic<int> state[2] = {{0}, {0}};  // main, sdk: 0 en marcha, 1 bien, 2 mal
    std::atomic<bool> running{false};
    std::mutex m;
    std::string err;
};
std::shared_ptr<GenShared> g_last_gen;
const char *const g_mods[2] = {"main", "sdk"};

struct Build {
    Paths P;
    std::vector<std::string> base;
    std::vector<Unit> units;
    std::deque<size_t> queue;
    std::mutex qm;
    std::condition_variable qcv;
    std::atomic<size_t> ndone{0}, nreused{0};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<bool> failed{false}, stop{false};
    std::string fail_msg;       // (con qm)
    std::atomic<int> alive{0};  // hilos de compilación en marcha
};
std::shared_ptr<Build> g_last_build;

void add_unit(Build &b, const std::wstring &src, const std::string &stem, const std::string &define) {
    Unit u;
    u.src = src, u.stem = stem, u.define = define;
    u.obj = b.P.work + L"\\" + wide(stem) + L".o";
    u.bytes = file_bytes(src);
    std::lock_guard<std::mutex> g(b.qm);
    b.units.push_back(u);
    b.queue.push_back(b.units.size() - 1);
    b.qcv.notify_one();
}

void compile_worker(const std::shared_ptr<Build> &bs) {
    Build &b = *bs;
    for (;;) {
        Unit u;
        {
            std::unique_lock<std::mutex> g(b.qm);
            b.qcv.wait(g, [&] { return b.stop || !b.queue.empty(); });
            if (b.queue.empty() || b.failed) break;
            u = b.units[b.queue.front()];
            b.queue.pop_front();
        }
        // ¿sirve el objeto de antes? (el mismo C, y la misma forma de compilar: base.txt)
        std::string src;
        std::string sha = read_file(u.src, src) ? sha256_hex(src + "\n" + u.define) : std::string();
        std::wstring srcf = b.P.work + L"\\" + wide(u.stem) + L".src";
        std::string prev;
        if (!sha.empty() && read_file(srcf, prev) && prev == sha && obj_ok(u.obj)) {
            b.nreused++, b.ndone++, b.bytes_done += u.bytes;
            continue;
        }
        DeleteFileW(lp(srcf).c_str());  // (su .src solo existe mientras su .o es el de ese C)
        std::string diag;
        bool ok = !sha.empty() && compile_unit(b.base, u, diag);
        if (!ok) {
            if (sha.empty()) diag = "no se pudo leer " + u8(u.src);
            if (diag.empty()) diag = "clang no dijo por qué";
            write_file(b.P.work + L"\\" + wide(u.stem) + L".log", diag);
            std::lock_guard<std::mutex> g(b.qm);
            if (!b.failed) {
                b.failed = true;
                size_t k = diag.find('\n');
                b.fail_msg = "el código del juego no compila (" + u.stem + "): " + (k == std::string::npos ? diag : diag.substr(0, k));
            }
            b.stop = true;
            b.qcv.notify_all();
            break;
        }
        write_file(srcf, sha);
        b.ndone++, b.bytes_done += u.bytes;
    }
    b.alive--;
}

int fail(wchar_t *why, int cap, const std::string &msg) {
    g_log("ERROR: %s", msg.c_str());
    std::wstring w = wide(msg);
    if (why && cap > 0) wcsncpy(why, w.c_str(), cap - 1), why[cap - 1] = 0;
    return 1;
}

}  // namespace

extern "C" __declspec(dllexport) int construir_juego(const wchar_t *dest, int jobs, AvanceFn avance, void *ctx,
                                                     volatile long *cancelar, wchar_t *why, int why_cap) {
    if (why && why_cap > 0) why[0] = 0;
    DWORD t0 = GetTickCount();
    Paths P;
    P.dest = dest;
    P.game = P.dest + L"\\extracted";
    P.work = P.game + L"\\.construir";
    P.tools = module_dir();
    P.py = P.tools + L"\\python";
    P.res_inc = P.tools + L"\\clang\\include";
    P.mingw_inc = P.tools + L"\\mingw\\include";
    P.mingw_lib = P.tools + L"\\mingw\\lib";
    P.src = P.dest + L"\\gamelib-src";
    P.implib = P.dest + L"\\smrpg-recomp.dll.a";
    g_log.open(P.dest + L"\\construccion.log");
    struct Close { ~Close() { g_log.close(); } } close_log;
    auto step = [&](double f, const wchar_t *s) { if (avance) avance(ctx, f, s); };
    auto cancelled = [&] { return cancelar && *cancelar; };

    // lo que hace falta: el juego extraído y lo que va en construir\ y gamelib-src
    std::string nso;
    if (!read_file(P.game + L"\\exefs\\main", nso) || nso.size() < 0x60 || memcmp(nso.data(), "NSO0", 4))
        return fail(why, why_cap, "no está el programa del juego (extracted\\exefs\\main)");
    char bid[17];
    for (int i = 0; i < 8; i++) snprintf(bid + 2 * i, 3, "%02x", (unsigned char)nso[0x40 + i]);
    std::string full_id = std::string(GAMELIB_ID) + "-" + bid;
    // (sin python312.zip o con él dañado, Python no arranca y llama a abort: se mira antes)
    for (const std::wstring &f : {P.py + L"\\python312.dll", P.py + L"\\python312.zip", P.py + L"\\python312._pth",
                                  P.py + L"\\vcruntime140.dll", P.py + L"\\gen\\tools\\gen.py", P.res_inc + L"\\stdint.h",
                                  P.mingw_inc + L"\\stdio.h", P.mingw_lib + L"\\dllcrt2.o", P.src + L"\\cpu.h",
                                  P.src + L"\\gamelib_entry.c", P.src + L"\\game_overrides.c", P.implib})
        if (!exists(f) || !file_bytes(f)) return fail(why, why_cap, "falta " + u8(f) + ": vuelve a ejecutar el instalador");
    if (g_tainted)
        return fail(why, why_cap, "una construcción anterior falló dentro de clang y dejó este proceso tocado: cierra el "
                                  "instalador y vuelve a abrirlo (lo ya compilado se guarda)");
    if (g_lld_spent)
        return fail(why, why_cap, "el enlazador falló antes y no se puede volver a usar: cierra el instalador y vuelve a abrirlo");
    std::string cpu = cpu_level();
    // (lo que siga en marcha de una construcción cancelada o fallida: acaba solo, y aquí se espera; un trozo grande
    // tarda ~3 min: si en 20 no ha acabado, algo se quedó trabado)
    for (DWORD tw = GetTickCount(); (g_last_gen && g_last_gen->running) || (g_last_build && g_last_build->alive > 0);) {
        if (cancelled()) return fail(why, why_cap, "cancelado");
        if (g_tainted || GetTickCount() - tw > 20 * 60 * 1000)
            return fail(why, why_cap, "lo que quedó en marcha de la vez anterior no termina: cierra el instalador y vuelve a "
                                      "abrirlo (lo ya compilado se guarda)");
        step(0, L"esperando a que acabe lo que quedó en marcha de la vez anterior");
        Sleep(200);
    }
    guard_install();
    mkdirs(P.work);
    MEMORYSTATUSEX ms = {sizeof ms};
    GlobalMemoryStatusEx(&ms);
    g_log("%s: %s, CPU %s, %d hilos de compilación, %.1f GB de RAM, id %s", CONSTRUIR_VERSION, u8(P.game).c_str(),
          cpu.c_str(), jobs, ms.ullTotalPhys / 1073741824.0, full_id.c_str());

    // la forma de compilar (base.txt): si cambia, ningún objeto anterior vale. Sin rutas (mover la carpeta no cambia lo
    // que sale) y con el contenido de las cabeceras de clang y de MinGW-w64; las del port (cpu.h, simd.h...) y el
    // generador van en GAMELIB_ID
    std::vector<std::string> base = cc1_args(P, cpu);
    Paths Pn = P;
    Pn.tools = L"<construir>", Pn.res_inc = L"<construir>\\clang\\include", Pn.mingw_inc = L"<construir>\\mingw\\include";
    Pn.src = L"<gamelib-src>";
    std::string sig = std::string(CONSTRUIR_VERSION) + "\n" + GAMELIB_ID + "\n" + cpu + "\n";
    for (auto &s : cc1_args(Pn, cpu)) sig += s + " ";
    sig += "\n" + std::to_string(file_bytes(P.tools + L"\\libclang-cpp.dll")) + " " +
           std::to_string(file_bytes(P.tools + L"\\libLLVM-23.dll")) + " " + dir_sha(P.res_inc) + " " + dir_sha(P.mingw_inc) + "\n";
    std::string old;
    bool same_base = read_file(P.work + L"\\base.txt", old) && old == sig;
    if (!same_base) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(lp(P.work + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = fd.cFileName;
                if (n.size() > 2 && (n.substr(n.size() - 2) == L".o" || (n.size() > 4 && n.substr(n.size() - 4) == L".src")))
                    DeleteFileW(lp(P.work + L"\\" + n).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        write_file(P.work + L"\\base.txt", sig);
    }

    // el generador: se reutiliza lo generado si el juego y el generador son los mismos (gen.key)
    std::string key = std::string(CONSTRUIR_VERSION) + GAMELIB_ID + sha256_hex(nso) + file_sha(P.game + L"\\exefs\\sdk") +
                      file_sha(P.src + L"\\hle_symbols.txt");
    for (const wchar_t *f : {L"gen.py", L"lifter.py", L"analyze.py", L"nso.py", L"passes.py", L"patches.py", L"simd_c.py",
                             L"game_addresses.py"})
        key += file_sha(P.py + L"\\gen\\tools\\" + f);
    key = sha256_hex(key);
    std::string done;
    int files[2] = {0, 0};  // trozos de main y de sdk
    bool reuse_gen = read_file(P.work + L"\\gen.key", done) && done.compare(0, 64, key) == 0 &&
                     sscanf(done.c_str() + 64, " %d %d", &files[0], &files[1]) == 2 && files[0] > 0 && files[1] > 0;
    const char *const *mods = g_mods;
    auto gs = std::make_shared<GenShared>();
    gs->P = P;
    gs->key = key;
    if (reuse_gen) {
        gs->state[0] = 1, gs->state[1] = 1;
        gs->files_final[0] = files[0], gs->files_final[1] = files[1];
        g_log("lo generado sirve (main %d trozos, sdk %d)", files[0], files[1]);
    } else {
        // (#337 de LAS Recomp: con Control Flow Guard activo, unicorn hace caer el proceso de golpe; el instalador se
        // vuelve a abrir sin él, así que aquí solo llega si no pudo)
        PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg = {};
        if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessControlFlowGuardPolicy, &cfg, sizeof cfg) &&
            cfg.EnableControlFlowGuard)
            return fail(why, why_cap, "Windows impone Control Flow Guard a este proceso y el generador (unicorn) no puede "
                                      "funcionar con él");
        if (cancelled()) return fail(why, why_cap, "cancelado");
        files[0] = files[1] = 0;
        DeleteFileW(lp(P.work + L"\\gen.key").c_str());
        for (int i = 0; i < 2; i++) {
            gs->log[i] = P.work + L"\\gen_" + wide(mods[i]) + L".log";
            DeleteFileW(lp(gs->log[i]).c_str());
            if (exists(gs->log[i]))  // (abierto por otro programa: no se lee el de la vez anterior por error)
                gs->log[i] = P.work + L"\\gen_" + wide(mods[i]) + L"." + std::to_wstring(GetTickCount()) + L".log";
        }
        std::string err;
        if (!py_load(P.py, err)) return fail(why, why_cap, "el generador no arranca: " + err);
        gs->running = true;
        g_last_gen = gs;
        HANDLE h = hilo_grande([gs] {
            // el sdk primero (8 s), después main (45 s): sus trozos se compilan mientras. Cada uno, al acabar bien:
            // primero su número final de trozos (de su registro entero), después state = 1
            for (int i : {1, 0}) {
                std::string e;
                gs->started[i] = true;
                int r = py_gen(gs->P, g_mods[i], i == 0 ? "0x7000000000" : "0x7005000000", gs->log[i], e);
                uint64_t ci = 0, all = 0;
                int n = 0;
                if (r == 0) {
                    gen_progress(gs->log[i], ci, all, n);
                    if (n <= 0) r = 1, e = std::string("el registro del generador (") + g_mods[i] + ") no dice cuántos trozos escribió";
                }
                if (r != 0) {
                    {
                        std::lock_guard<std::mutex> g(gs->m);
                        gs->err = e;
                    }
                    gs->state[i] = 2;
                    if (gs->state[1 - i] == 0) gs->state[1 - i] = 2;
                    break;
                }
                gs->files_final[i] = n;
                gs->state[i] = 1;
            }
            // (lo generado sirve para la próxima vez aunque esta se cancele mientras se compila)
            if (gs->state[0] == 1 && gs->state[1] == 1)
                write_file(gs->P.work + L"\\gen.key", gs->key + " " + std::to_string(gs->files_final[0]) + " " +
                                                         std::to_string(gs->files_final[1]) + "\n");
            gs->running = false;
        });
        if (!h) {
            gs->running = false;
            return fail(why, why_cap, "no se pudo crear el hilo del generador (error " + std::to_string(GetLastError()) + ")");
        }
        CloseHandle(h);  // (suelto: acaba solo)
    }

    // los trozos, a medida que salen; un grupo de hilos los compila
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    auto bs = std::make_shared<Build>();
    Build &b = *bs;
    b.P = P, b.base = base;
    std::vector<Unit> &units = b.units;
    std::mutex &qm = b.qm;
    std::condition_variable &qcv = b.qcv;
    std::atomic<size_t> &ndone = b.ndone, &nreused = b.nreused;
    std::atomic<uint64_t> &bytes_done = b.bytes_done;
    std::atomic<bool> &failed = b.failed, &stop = b.stop;
    add_unit(b, P.src + L"\\gamelib_entry.c", "gamelib_entry", "GAMELIB_ID=\"" + full_id + "\"");
    add_unit(b, P.src + L"\\game_overrides.c", "game_overrides", "");
    int njobs = jobs < 1 ? 1 : jobs > 64 ? 64 : jobs;
    std::vector<HANDLE> pool;
    g_last_build = bs;
    for (int i = 0; i < njobs; i++) {
        b.alive++;
        HANDLE h = hilo_grande([bs] { compile_worker(bs); });
        if (h) pool.push_back(h);
        else b.alive--;
    }
    if (pool.empty()) {
        std::lock_guard<std::mutex> g(qm);
        failed = true, stop = true;
        return fail(why, why_cap, "no se pudo crear ningún hilo de compilación (error " + std::to_string(GetLastError()) + ")");
    }
    int queued[2] = {0, 0};
    bool tables[2] = {false, false};
    uint64_t gci[2] = {0, 0}, gall[2] = {0, 0};
    double shown = 0;
    for (;;) {
        if (cancelled()) {
            std::lock_guard<std::mutex> g(qm);
            if (!failed) failed = true, b.fail_msg = "cancelado (lo compilado se guarda)";
            stop = true;
            b.queue.clear();
            qcv.notify_all();
        }
        bool gens_done = true;
        for (int i = 0; i < 2; i++) {
            // (el estado primero: con state = 1, files_final ya es el número entero de trozos, y la tabla, que va la
            // última, nunca sale antes que alguno de sus trozos)
            int st = gs->state[i];
            if (st == 1 && files[i] < gs->files_final[i]) files[i] = gs->files_final[i];
            if (!reuse_gen && st == 0 && gs->started[i]) gen_progress(gs->log[i], gci[i], gall[i], files[i]);
            if (st == 2 && !failed) {
                std::string e;
                {
                    std::lock_guard<std::mutex> g(gs->m);
                    e = gs->err;
                }
                std::lock_guard<std::mutex> g2(qm);
                failed = true, b.fail_msg = e.empty() ? "el generador falló" : e;
                stop = true;
                qcv.notify_all();
            }
            if (st != 1) gens_done = false;
            for (; !failed && queued[i] < files[i]; queued[i]++) {
                char stem[32];
                snprintf(stem, sizeof stem, "%s_%03d", mods[i], queued[i]);
                add_unit(b, P.work + L"\\" + wide(mods[i]) + L"\\" + wide(stem) + L".c", stem, "");
            }
            if (st == 1 && !tables[i] && !failed) {
                tables[i] = true;
                add_unit(b, P.work + L"\\" + wide(mods[i]) + L"\\" + wide(mods[i]) + L"_table.c", std::string(mods[i]) + "_table", "");
            }
        }
        size_t total, nd = ndone;
        {
            std::lock_guard<std::mutex> g(qm);
            total = units.size();
        }
        if (failed && stop) break;
        if (gens_done && nd == total) break;
        // el avance: lo generado (15 %), lo compilado (mitad bytes, mitad archivos) y el enlace (el 5 % final)
        double gf = reuse_gen ? 1 : 0;
        if (!reuse_gen) {
            for (int i = 0; i < 2; i++)
                gf += (gs->state[i] == 1 ? 1.0 : gall[i] ? (double)gci[i] / (double)gall[i] : 0) * (i == 0 ? 0.85 : 0.15);
        }
        uint64_t all_bytes = 0;
        {
            std::lock_guard<std::mutex> g(qm);
            for (auto &u : units) all_bytes += u.bytes;
        }
        double est_files = gens_done ? (double)total : gf > 0.05 ? (double)total / (gf < 1 ? gf : 1) : 1056;
        double est_bytes = gens_done ? (double)all_bytes : gf > 0.05 ? (double)all_bytes / gf : 7.6e8;
        double cf = 0.5 * (double)nd / est_files + 0.5 * (double)bytes_done / (est_bytes > 1 ? est_bytes : 1);
        if (cf > 1) cf = 1;
        double f = 0.15 * gf + 0.80 * cf;
        if (f > shown) shown = f;
        wchar_t s[200];
        if (!gens_done && gf < 1)
            swprintf(s, 200, L"traduciendo el código del juego a C y compilando (%zu archivos listos)", nd);
        else
            swprintf(s, 200, L"compilando (%zu de %zu archivos)", nd, total);
        step(shown, s);
        Sleep(150);
    }
    std::string fail_msg;
    {
        std::lock_guard<std::mutex> g(qm);
        stop = true;
        fail_msg = b.fail_msg;
        qcv.notify_all();
    }
    if (failed) {
        // (los hilos acaban por su cuenta el archivo que tengan entre manos; la siguiente construcción los espera)
        for (HANDLE h : pool) CloseHandle(h);
        int busy = b.alive;
        if (busy) g_log("%d hilos de compilación acaban en segundo plano el archivo que tenían", busy);
        return fail(why, why_cap, fail_msg);
    }
    for (HANDLE h : pool) WaitForSingleObject(h, INFINITE), CloseHandle(h);  // (sin trabajo: salen enseguida)
    while (gs->running) Sleep(10);  // (ya dijo que acabó: sale enseguida; gen.key ya lo escribió él)
    g_log("compilados %zu archivos (%zu reutilizados) en %lu s", units.size(), (size_t)nreused, (GetTickCount() - t0) / 1000);

    // el enlace: gamelib_entry, game_overrides (antes que el código generado) y los trozos
    step(0.95, L"enlazando");
    std::vector<std::wstring> objs;
    for (auto &u : units) objs.push_back(u.obj);
    std::wstring out = P.game + L"\\lasgame.dll", tmp = P.work + L"\\lasgame.dll.tmp";
    std::string diag;
    DWORD tl = GetTickCount();
    bool linked = link_lib(P, objs, tmp, diag);
    g_log("enlace: %.1f s", (GetTickCount() - tl) / 1000.0);
    if (!linked) {
        write_file(P.work + L"\\link.log", diag);
        // (algo de lo guardado no vale —un objeto estropeado, una cuenta de trozos mala—: la próxima vez, todo de nuevo)
        DeleteFileW(lp(P.work + L"\\gen.key").c_str());
        DeleteFileW(lp(P.work + L"\\base.txt").c_str());
        size_t k = diag.find('\n');
        return fail(why, why_cap, "el código del juego no enlaza: " + (k == std::string::npos ? diag : diag.substr(0, k)) +
                                      " (lo guardado se descarta: al volver a instalar se genera y compila todo de nuevo)");
    }
    // (sin .cpu, el programa exige lo más alto, x86-64-v3: mientras se cambia la biblioteca nunca hay código marcado
    // para una CPU que no es la suya)
    DeleteFileW(lp(out + L".cpu").c_str());
    if (!MoveFileExW(lp(tmp).c_str(), lp(out).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        return fail(why, why_cap, "no se pudo escribir extracted\\lasgame.dll (¿está el juego abierto?)");
    write_file(out + L".id", full_id + "\n");
    write_file(out + L".cpu", cpu + "\n");
    remove_tree(P.game + L"\\.gamebuild");  // (la caché de la 0.47.0, con zig: ya no sirve)
    PROCESS_MEMORY_COUNTERS pm = {sizeof pm};
    GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm);
    g_log("lasgame.dll lista: %.0f MB, %lu s en total, memoria máxima del proceso %.1f GB", file_bytes(out) / 1e6,
          (GetTickCount() - t0) / 1000, pm.PeakWorkingSetSize / 1073741824.0);
    step(1, L"listo");
    return 0;
}

// (si alguien descargase construir.dll, su manejador de excepciones no debe quedarse apuntando a código que ya no está)
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && !reserved && g_guard_veh) RemoveVectoredExceptionHandler(g_guard_veh);
    return TRUE;
}

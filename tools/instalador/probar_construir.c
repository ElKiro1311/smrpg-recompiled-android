// probar_construir.exe <construir.dll> <carpeta instalada> [hilos] [cancelar_s] [repetir]: prueba construir.dll fuera
// del instalador, en este mismo proceso (el avance por la salida estándar). Código de salida: el de construir_juego (0 =
// bien). Con cancelar_s (> 0), cancela a los tantos segundos (mide cuánto tarda en volver) y después vuelve a construir
// en el mismo proceso: la segunda espera a lo que dejó en marcha la primera y reutiliza lo que ya se compiló. Con
// «repetir», si la construcción falla, la repite una vez en el mismo proceso (la red: SMRPG_CONSTRUIR_PRUEBA_FALLO).
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>

typedef void (*AvanceFn)(void *ctx, double frac, const wchar_t *paso);
typedef int (*ConstruirFn)(const wchar_t *dest, int jobs, AvanceFn avance, void *ctx, volatile long *cancelar,
                           wchar_t *why, int why_cap);

static volatile long g_cancel;
static DWORD g_cancel_ms;

static void avance(void *ctx, double f, const wchar_t *paso) {
    static int last = -1;
    static wchar_t last_paso[64];
    int k = (int)(f * 100);
    (void)ctx;
    if (k == last && !wcsncmp(paso, last_paso, 63)) return;
    last = k;
    wcsncpy(last_paso, paso, 63);
    char s[600];
    WideCharToMultiByte(CP_UTF8, 0, paso, -1, s, sizeof s, NULL, NULL);
    printf("%3d %%  %s\n", k, s);
    fflush(stdout);
}

static DWORD WINAPI canceller(void *p) {
    Sleep(g_cancel_ms);
    printf("--- cancelando ---\n");
    fflush(stdout);
    InterlockedExchange(&g_cancel, 1);
    (void)p;
    return 0;
}

static int run(ConstruirFn f, const wchar_t *dest, int jobs, const char *name) {
    wchar_t why[1024] = L"";
    DWORD t0 = GetTickCount();
    int r = f(dest, jobs, avance, NULL, &g_cancel, why, 1024);
    char w[2048];
    WideCharToMultiByte(CP_UTF8, 0, why, -1, w, sizeof w, NULL, NULL);
    printf("%s: resultado %d en %.1f s%s%s\n", name, r, (GetTickCount() - t0) / 1000.0, w[0] ? ": " : "", w);
    fflush(stdout);
    return r;
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) { fprintf(stderr, "uso: probar_construir <construir.dll> <carpeta> [hilos] [cancelar_s]\n"); return 2; }
    HMODULE m = LoadLibraryExW(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { fprintf(stderr, "no carga %ls (error %lu)\n", argv[1], GetLastError()); return 2; }
    ConstruirFn f = (ConstruirFn)(void *)GetProcAddress(m, "construir_juego");
    if (!f) { fprintf(stderr, "sin construir_juego\n"); return 2; }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int jobs = argc > 3 && _wtoi(argv[3]) > 0 ? _wtoi(argv[3]) : (int)si.dwNumberOfProcessors;
    if (argc > 4 && _wtoi(argv[4]) > 0) {
        g_cancel_ms = (DWORD)_wtoi(argv[4]) * 1000;
        HANDLE h = CreateThread(NULL, 0, canceller, NULL, 0, NULL);
        DWORD t = GetTickCount();
        run(f, argv[2], jobs, "primera (cancelada)");
        printf("volvió %.1f s después de cancelar\n", (GetTickCount() - t - g_cancel_ms) / 1000.0);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        InterlockedExchange(&g_cancel, 0);
        return run(f, argv[2], jobs, "segunda");
    }
    int r = run(f, argv[2], jobs, "construcción");
    if (r != 0 && argc > 5 && !wcscmp(argv[5], L"repetir")) r = run(f, argv[2], jobs, "repetida");
    return r;
}

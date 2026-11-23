// con_cfg.exe <programa> [argumentos...]: lo arranca con Control Flow Guard FORZADO por la directiva del proceso (como
// cuando Windows lo impone a todos los programas, #337 de LAS Recomp), espera y dice cómo terminó.
#include <stdio.h>
#include <wchar.h>
#include <windows.h>

#ifndef PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY
#define PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY 0x00020007
#endif
#define CFG_ALWAYS_ON (0x00000001ull << 40)

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) return 2;
    wchar_t cmd[8192] = L"";
    for (int i = 1; i < argc; i++) {
        wcscat(cmd, L"\"");
        wcscat(cmd, argv[i]);
        wcscat(cmd, L"\" ");
    }
    SIZE_T sz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &sz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, sz);
    DWORD64 pol = CFG_ALWAYS_ON;
    if (!InitializeProcThreadAttributeList(al, 1, 0, &sz) ||
        !UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &pol, sizeof pol, NULL, NULL)) {
        printf("atributos: error %lu\n", GetLastError());
        return 2;
    }
    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.lpAttributeList = al;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(argv[1], cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) {
        printf("CreateProcess: error %lu\n", GetLastError());
        return 2;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("con_cfg: terminó con 0x%08lX (%lu)%s\n", code, code, code == 0xC0000409 ? " = fallo de seguridad (CFG)" : "");
    return 0;
}

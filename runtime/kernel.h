// Kernel object types shared by kernel.c and the IPC/service layer.
#pragma once
#include <setjmp.h>

#include "rt.h"

#include "guest_addr.h"
#define STACK_BASE GADDR(0x7200000000ull)
#define STACK_SIZE 0x0040000000ull

typedef enum { KOBJ_THREAD = 1, KOBJ_EVENT, KOBJ_SESSION, KOBJ_SHMEM, KOBJ_TMEM } KType;
typedef enum { WAIT_NONE = 0, WAIT_MUTEX, WAIT_CONDVAR, WAIT_ARBITER } WaitKind;

typedef struct KObject {
    KType type;
    int refs;
    bool signaled;
} KObject;

typedef KObject KEvent;

struct KThread {
    KObject obj;  // signaled when the thread exits
    Cpu cpu;
#ifdef _WIN32
    uintptr_t host;
#else
    pthread_t host;
#endif
    uint64_t entry, tlr, id;
    uint32_t handle;
    int prio, core;
    bool cancel, in_sync_wait;
    bool is_main;  // the process's main thread (kernel_create_main_thread)
    // SMRPG: pausa cooperativa (SetThreadActivity / GetThreadContext3: el «stop the world» del GC de IL2CPP).
    // pause_req: pedida; paused: parado en un punto seguro (kernel_pause_point); in_svc: dentro de una llamada al
    // sistema, con su estado de invitado ya guardado en cpu.
    volatile bool pause_req, paused, in_svc;
    // SMRPG: sus últimas llamadas al host (funciones HLE y llamadas al sistema), para los volcados de errores y cuelgues
    struct { const char *name; uint64_t x[4], lr; } calls[32];
    unsigned calls_n;
    // wait state (protected by g_klock)
    WaitKind wait_kind;
    uint64_t wait_addr, wait_mutex;
    uint32_t wait_tag, wait_result;
    KThread *wnext;
    KThread *snext;        // in WaitSynchronization (g_syncwaiters)
    pthread_cond_t cond;   // each thread sleeps on its own condition; wakers signal it directly
    // in-place reboot (kernel_stop_threads): thread_main's setjmp point, where the thread unwinds to and ends
    jmp_buf unwind;
    bool started, finished;
    KThread *anext;        // every thread of this boot (g_klock)
    // (diagnostics: kernel_dump_threads) when the current kernel wait began; the handles of a WaitSynchronization
    uint64_t wait_since;
    uint64_t sync_handles;
    int32_t sync_count;
};

typedef struct KSharedMem {
    KObject obj;
    uint64_t size, guest_addr;  // guest_addr: where the guest mapped it (0 if not mapped)
    void (*on_map)(struct KSharedMem *s);  // HLE owner fills initial contents
    void *owner;
} KSharedMem;

KSharedMem *kernel_create_shared_memory(uint64_t size, void (*on_map)(KSharedMem *), uint32_t *handle);

typedef struct {
    KObject obj;
    uint64_t addr, size;
} KTransferMem;

extern pthread_mutex_t g_klock;
extern KThread *g_waitlist;

KObject *handle_get(uint32_t h);
uint32_t kernel_new_handle(KObject *o);
void kernel_signal(KObject *o);  // caller holds g_klock
KEvent *kernel_create_event_pair(uint32_t *writable, uint32_t *readable);
KThread *kernel_current_thread(void);
// (diagnostics) the "[hang]" dump (kernel.c): every guest thread and what it waits on, the kernel trace
void kernel_dump_threads(const char *prefix);
// The renderer's part of the hang dump (runtime/nvn/rqueue.c), called from the hang watch thread every second:
// check fills why and returns true when it sees a hang (limit_ns: LARECOMP_HANG_DUMP); state logs its side.
void kernel_hang_hooks(bool (*check)(uint64_t limit_ns, char *why, size_t n), void (*state)(const char *prefix));
uint64_t kernel_heap_size(void);  // bytes from HEAP_BASE set by SetHeapSize
// SMRPG: datos del proceso que main.c lee del main.npdm, para GetInfo (ID de programa y «system resource», que activa
// la memoria virtual de nn::os).
void kernel_set_process_info(uint64_t program_id, uint64_t system_resource_size);
// SMRPG: el hilo actual se para aquí si tiene una pausa pedida (punto seguro: FN_ENTER, rt_svc, llamadas al host).
void kernel_pause_point(void);
// SMRPG: anota una llamada del hilo actual al host (nombre, x0-x3, desde dónde) e imprime las últimas de este hilo.
void kernel_note_call(const char *name, const uint64_t *x, uint64_t lr);
void kernel_print_current_calls(void);
// SMRPG: el resultado que el juego dejó con am SetTerminateResult (0 = ninguno), para explicar un svcBreak.
extern _Atomic uint32_t g_terminate_result;

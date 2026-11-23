// SMRPG: nn::ro (módulos NRO cargados en tiempo de ejecución), implementado en el host.
//
// En la Switch, nn::ro::Initialize se conecta al servicio ldr:ro y usa las listas de módulos que prepara el rtld al
// arrancar (nn::ro::detail::g_pAutoLoadList, g_pManualLoadList, g_pLookupGlobalManualFunctionPointer). Este runtime no
// tiene ni ese servicio ni el rtld: esos punteros quedan a 0 y nn::ro::InitializeWithPortName escribe en la dirección 0.
//
// SMRPG (Unity) importa estas 8 funciones de nn::ro y su RomFS no trae ningún .nro ni .nrr: solo llama a Initialize y
// busca símbolos por nombre (los [DllImport("__Internal")] de Unity, como el plugin de CRIWARE). Es lo mismo que hace
// nxnative con Fire Emblem Engage (dm/host/hle_ro.c). Cargar un módulo, mirar dentro de uno o registrar un NRR falla y
// queda en el registro, por si algún día se llama.
#include "../hle.h"

uint64_t loader_find_symbol(const char *name);

#define RO_FAIL (22u | (1u << 9))  // un fallo del módulo ro (22): el juego solo mira si es un éxito

HLE_FUNC("_ZN2nn2ro10InitializeEv", ro_initialize) {
    LOG("[ro] Initialize (sin servicio ldr:ro: los símbolos se buscan en los módulos cargados)\n");
    c->x[0] = 0;
}

// nn::ro::LookupSymbol(uintptr_t *out, const char *name): igual que enlaza el loader (runtime/loader.c resolve), primero
// la versión del host de esa función y si no la definición de main o del sdk.
HLE_FUNC("_ZN2nn2ro12LookupSymbolEPmPKc", ro_lookup_symbol) {
    uint64_t out = c->x[0];
    const char *name = (const char *)(uintptr_t)c->x[1];
    uint64_t a = 0;
    if (name) {
        HostFn impl = hle_lookup_impl(name);
        a = impl ? hle_register(name, impl) : loader_find_symbol(name);
    }
    static int logged;
    if (!a || logged++ < 40)
        LOG("[ro] LookupSymbol %s -> %llx%s\n", name ? name : "(null)", (unsigned long long)a, a ? "" : " (no está)");
    if (!a) { c->x[0] = RO_FAIL; return; }
    if (out) *(uint64_t *)(uintptr_t)out = a;
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", ro_load_module) {
    LOG("[ro] LoadModule: este port no carga módulos NRO\n");
    c->x[0] = RO_FAIL;
}

HLE_FUNC("_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", ro_unload_module) { (void)c; }

HLE_FUNC("_ZN2nn2ro13GetBufferSizeEPmPKv", ro_get_buffer_size) {
    if (c->x[0]) *(uint64_t *)(uintptr_t)c->x[0] = 0;
    LOG("[ro] GetBufferSize: este port no carga módulos NRO\n");
    c->x[0] = RO_FAIL;
}

HLE_FUNC("_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", ro_lookup_module_symbol) {
    LOG("[ro] LookupModuleSymbol %s: no hay módulos NRO\n", c->x[2] ? (const char *)(uintptr_t)c->x[2] : "(null)");
    c->x[0] = RO_FAIL;
}

// Los NRR solo registran huellas de módulos que aquí nunca se cargan: éxito.
HLE_FUNC("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", ro_register_module_info) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", ro_unregister_module_info) { (void)c; }

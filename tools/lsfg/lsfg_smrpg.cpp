// (SMRPG) Lossless Scaling Frame Generation para el port: la biblioteca aparte (tools/lsfg/lsfg_smrpg.h). Lo de aquí es
// la parte del port; lo demás es de lsfg-vk-android (GameNative, rama release, MIT): su «framegen», y de su capa, la
// tabla de recursos de extract.cpp, la traducción de trans.cpp y el sombreador de mipmaps que trae (mipmaps_replacement)
// - con el traductor DXBC -> SPIR-V de DXVK (zlib) y pe-parse (MIT) que usa. Su licencia: third_party/lsfg-vk-android.
// Nada sale de aquí como excepción: las funciones de C devuelven un código y el motivo en err. El traductor de DXVK
// avisa con dxvk::DxvkError, que no deriva de std::exception (un Lossless.dll dañado o de otra versión): por eso los
// sombreadores del modo pedido se traducen todos al iniciarse, y un fallo sale ahí, con su nombre, y no al crear el
// contexto en pleno juego.
#include "lsfg_smrpg.h"

#include "common/exception.hpp"
#include "lsfg_3_1.hpp"
#include "lsfg_3_1p.hpp"

#include <dxbc_modinfo.h>
#include <dxbc_module.h>
#include <dxbc_reader.h>
#include <pe-parse/parse.h>
#include <thirdparty/spirv.hpp>
#include <util_error.h>

#include <volk.h>  // (sus punteros: el de vkCreateComputePipelines se cambia por pc_create)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "mipmaps_replacement_spv.h"

#ifdef _WIN32
#define LSFG_SMRPG_API __declspec(dllexport)
#else
#define LSFG_SMRPG_API __attribute__((visibility("default")))
#endif

namespace {

// (de lsfg-vk src/extract/extract.cpp) el recurso RT_RCDATA de Lossless.dll con el DXBC de cada sombreador; los «p_»,
// los del modo rendimiento (LSFG_3_1P los pide con ese nombre)
const std::unordered_map<std::string, uint32_t> kNameIdx = {{
    {"mipmaps", 255}, {"alpha[0]", 267}, {"alpha[1]", 268}, {"alpha[2]", 269}, {"alpha[3]", 270},
    {"beta[0]", 275}, {"beta[1]", 276}, {"beta[2]", 277}, {"beta[3]", 278}, {"beta[4]", 279},
    {"gamma[0]", 257}, {"gamma[1]", 259}, {"gamma[2]", 260}, {"gamma[3]", 261}, {"gamma[4]", 262},
    {"delta[0]", 257}, {"delta[1]", 263}, {"delta[2]", 264}, {"delta[3]", 265}, {"delta[4]", 266},
    {"delta[5]", 258}, {"delta[6]", 271}, {"delta[7]", 272}, {"delta[8]", 273}, {"delta[9]", 274},
    {"generate", 256},
    {"p_mipmaps", 255}, {"p_alpha[0]", 290}, {"p_alpha[1]", 291}, {"p_alpha[2]", 292}, {"p_alpha[3]", 293},
    {"p_beta[0]", 298}, {"p_beta[1]", 299}, {"p_beta[2]", 300}, {"p_beta[3]", 301}, {"p_beta[4]", 302},
    {"p_gamma[0]", 280}, {"p_gamma[1]", 282}, {"p_gamma[2]", 283}, {"p_gamma[3]", 284}, {"p_gamma[4]", 285},
    {"p_delta[0]", 280}, {"p_delta[1]", 286}, {"p_delta[2]", 287}, {"p_delta[3]", 288}, {"p_delta[4]", 289},
    {"p_delta[5]", 281}, {"p_delta[6]", 294}, {"p_delta[7]", 295}, {"p_delta[8]", 296}, {"p_delta[9]", 297},
    {"p_generate", 256},
}};

std::unordered_map<uint32_t, std::vector<uint8_t>> g_res;     // (los recursos leídos de Lossless.dll)
std::unordered_map<std::string, std::vector<uint8_t>> g_spv;  // (los sombreadores del modo, ya traducidos)
bool g_perf, g_orig_mipmaps;
// initializeExternal lanzó a medias en ese modo (0 calidad, 1 rendimiento): deja su instancia puesta sin dispositivo,
// su finalize no la quita y los init siguientes volverían sin hacer nada; solo se arregla cerrando el juego
bool g_lib_broken[2];

int on_resource(void *, const peparse::resource &res) {
    if (res.type != peparse::RT_RCDATA || res.buf == nullptr || res.buf->bufLen <= 0) return 0;
    std::vector<uint8_t> d(res.buf->bufLen);
    std::copy_n(res.buf->buf, res.buf->bufLen, d.data());
    g_res[res.name] = std::move(d);
    return 0;
}

// (de lsfg-vk src/extract/trans.cpp) DXBC -> SPIR-V con el compilador de DXVK; los bindings, renumerados 0..n-1
struct BindingOffsets {
    uint32_t bindingIndex{}, bindingOffset{}, setIndex{}, setOffset{};
};
std::vector<uint8_t> translate(const std::vector<uint8_t> &bytecode) {
    dxvk::DxbcReader reader(reinterpret_cast<const char *>(bytecode.data()), bytecode.size());
    dxvk::DxbcModule module(reader);
    const dxvk::DxbcModuleInfo info{};
    auto code = module.compile(info, "CS");
    std::vector<BindingOffsets> offs;
    std::vector<uint32_t> ids;
    for (auto ins : code) {
        if (ins.opCode() == spv::OpDecorate) {
            if (ins.arg(2) == spv::DecorationBinding) {
                const uint32_t id = ins.arg(1);
                offs.resize(std::max(offs.size(), size_t(id + 1)));
                offs[id].bindingIndex = ins.arg(3);
                offs[id].bindingOffset = ins.offset() + 3;
                ids.push_back(id);
            }
            if (ins.arg(2) == spv::DecorationDescriptorSet) {
                const uint32_t id = ins.arg(1);
                offs.resize(std::max(offs.size(), size_t(id + 1)));
                offs[id].setIndex = ins.arg(3);
                offs[id].setOffset = ins.offset() + 3;
            }
        }
        if (ins.opCode() == spv::OpFunction) break;
    }
    std::vector<BindingOffsets> valid;
    for (const auto id : ids)
        if (offs[id].bindingOffset) valid.push_back(offs[id]);
    for (size_t i = 0; i < valid.size(); i++) code.data()[valid.at(i).bindingOffset] = static_cast<uint8_t>(i);
    std::vector<uint8_t> spirv(code.size());
    std::copy_n(reinterpret_cast<uint8_t *>(code.data()), code.size(), spirv.data());
    return spirv;
}
// (el error del traductor, a uno que sí deriva de std::exception y nombra el sombreador)
std::vector<uint8_t> translate_named(const std::string &name, const std::vector<uint8_t> &bytecode) {
    try {
        return translate(bytecode);
    } catch (const dxvk::DxvkError &e) {
        throw std::runtime_error("el sombreador " + name + " de Lossless.dll no se pudo traducir (" + e.message() +
                                 "): ¿Lossless.dll dañado o de otra versión?");
    }
}

bool is_mipmaps(const std::string &name) { return name == "mipmaps" || name == "p_mipmaps"; }

std::vector<uint8_t> load_shader(const std::string &name) {
    if (!g_orig_mipmaps && is_mipmaps(name))
        return std::vector<uint8_t>(mipmaps_replacement_spv, mipmaps_replacement_spv + mipmaps_replacement_spv_len);
    auto spv = g_spv.find(name);
    if (spv != g_spv.end()) return spv->second;
    auto hit = kNameIdx.find(name);
    if (hit == kNameIdx.end()) throw std::runtime_error("sombreador desconocido: " + name);
    auto res = g_res.find(hit->second);
    if (res == g_res.end()) throw std::runtime_error("falta el sombreador " + name + " en Lossless.dll");
    return translate_named(name, res->second);
}

void set_err(char *err, int len, const std::string &msg) {
    if (err && len > 0) snprintf(err, (size_t)len, "%s", msg.c_str());
}

// ---- caché de tuberías. LSFG crea las suyas (una por sombreador: unas 20 de cómputo) sin caché, al crear el primer
// contexto tras iniciarse, y en la Adreno de la AYN Thor eso tardaba 0,75-1,2 s: el parón al activarla en cada partida.
// Tras su initializeExternal (que carga los punteros de volk del dispositivo, también los de cada init siguiente), su
// vkCreateComputePipelines pasa a ser pc_create, que les da una VkPipelineCache leída de cache_path si su cabecera es de
// esta GPU y este controlador; se guarda si cambió después de crear cada contexto, y se cierra en finalize.
PFN_vkCreateComputePipelines g_ccp_orig;
VkPipelineCache g_pc;
VkDevice g_pc_dev;
std::string g_pc_path;
size_t g_pc_saved;
constexpr size_t kPcMax = 64u << 20;  // (más que esto en el archivo: dañado o de otra cosa; se empieza de cero)

VKAPI_ATTR VkResult VKAPI_CALL pc_create(VkDevice dev, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *ci,
                                         const VkAllocationCallbacks *alloc, VkPipeline *out) {
    if (!cache && dev == g_pc_dev) cache = g_pc;
    return g_ccp_orig(dev, cache, n, ci, alloc, out);
}

// la cabecera (VkPipelineCacheHeaderVersionOne) es de esta GPU y de este controlador
bool pc_header_ok(const std::vector<uint8_t> &d, VkPhysicalDevice phys) {
    if (d.size() < 16 + VK_UUID_SIZE || !vkGetPhysicalDeviceProperties) return false;
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(phys, &p);
    uint32_t h[4];
    memcpy(h, d.data(), sizeof h);  // tamaño de la cabecera, versión, vendorID, deviceID
    return h[0] >= 16 + VK_UUID_SIZE && h[0] <= d.size() && h[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           h[2] == p.vendorID && h[3] == p.deviceID && !memcmp(d.data() + 16, p.pipelineCacheUUID, VK_UUID_SIZE);
}

void pc_save() {
    if (!g_pc || !vkGetPipelineCacheData) return;
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_pc_dev, g_pc, &sz, nullptr) != VK_SUCCESS || !sz || sz > kPcMax || sz == g_pc_saved) return;
    std::vector<uint8_t> d(sz);
    if (vkGetPipelineCacheData(g_pc_dev, g_pc, &sz, d.data()) != VK_SUCCESS) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(g_pc_path).parent_path(), ec);
    const std::string tmp = g_pc_path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(d.data(), 1, sz, f) == sz;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(g_pc_path.c_str());  // (en Windows rename no pisa; cortarse aquí pierde la caché, nunca la daña)
        ok = rename(tmp.c_str(), g_pc_path.c_str()) == 0;
    }
    if (ok) g_pc_saved = sz;
    else remove(tmp.c_str());
}

void pc_close() {
    if (vkCreateComputePipelines == pc_create) vkCreateComputePipelines = g_ccp_orig;
    if (!g_pc) return;
    pc_save();
    if (vkDestroyPipelineCache) vkDestroyPipelineCache(g_pc_dev, g_pc, nullptr);
    g_pc = VK_NULL_HANDLE;
    g_pc_dev = VK_NULL_HANDLE;
}

void pc_open(const LsfgSmrpgInit *in) {
    if (!in->cache_path || !*in->cache_path || !vkCreateComputePipelines || !vkCreatePipelineCache) return;
    if (g_pc && (g_pc_dev != in->device || g_pc_path != in->cache_path)) pc_close();
    if (!g_pc) {
        std::vector<uint8_t> d;
        if (FILE *f = fopen(in->cache_path, "rb")) {
            if (fseek(f, 0, SEEK_END) == 0) {
                long n = ftell(f);
                if (n > 0 && (size_t)n <= kPcMax && fseek(f, 0, SEEK_SET) == 0) {
                    d.resize((size_t)n);
                    if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
                }
            }
            fclose(f);
        }
        if (!pc_header_ok(d, in->phys)) d.clear();
        VkPipelineCacheCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        ci.initialDataSize = d.size();
        ci.pInitialData = d.empty() ? nullptr : d.data();
        if (vkCreatePipelineCache(in->device, &ci, nullptr, &g_pc) != VK_SUCCESS) {
            ci.initialDataSize = 0;  // (datos que el controlador no acepta: una vacía)
            ci.pInitialData = nullptr;
            if (vkCreatePipelineCache(in->device, &ci, nullptr, &g_pc) != VK_SUCCESS) g_pc = VK_NULL_HANDLE;
        }
        if (!g_pc) return;
        g_pc_dev = in->device;
        g_pc_path = in->cache_path;
        g_pc_saved = d.size();
    }
    if (vkCreateComputePipelines != pc_create) {
        g_ccp_orig = vkCreateComputePipelines;
        vkCreateComputePipelines = pc_create;
    }
}

}  // namespace

extern "C" {

LSFG_SMRPG_API int lsfg_smrpg_abi(void) { return LSFG_SMRPG_ABI; }

LSFG_SMRPG_API int lsfg_smrpg_init(const LsfgSmrpgInit *in, char *err, int errlen) {
    try {
        if (!in || !in->dll_path || !in->gipa || in->generations < 1 || in->flow_scale <= 0) {
            set_err(err, errlen, "parámetros no válidos");
            return -1;
        }
        const int mode = in->performance != 0;
        if (g_lib_broken[mode]) {
            set_err(err, errlen, "ya falló al iniciarse en este modo y no se deja iniciar otra vez: cierra el juego y vuelve a abrirlo");
            return -5;
        }
        g_res.clear();
        g_spv.clear();
        peparse::parsed_pe *dll = peparse::ParsePEFromFile(in->dll_path);
        if (!dll) {
            set_err(err, errlen, std::string("no se pudo leer ") + in->dll_path);
            return -2;
        }
        peparse::IterRsrc(dll, on_resource, nullptr);
        peparse::DestructParsedPE(dll);
        for (const auto &[name, idx] : kNameIdx)
            if (g_res.find(idx) == g_res.end()) {
                set_err(err, errlen, "a Lossless.dll le falta el sombreador " + name + ": ¿Lossless Scaling al día?");
                return -3;
            }
        g_perf = mode != 0;
        g_orig_mipmaps = in->orig_mipmaps != 0;
        for (const auto &[name, idx] : kNameIdx) {
            if ((name.rfind("p_", 0) == 0) != g_perf || (!g_orig_mipmaps && is_mipmaps(name))) continue;
            g_spv[name] = translate_named(name, g_res.at(idx));
        }
        try {
            if (g_perf)
                LSFG_3_1P::initializeExternal(in->gipa, in->instance, in->phys, in->device, in->queue_family, in->queue,
                                              in->hdr != 0, 1.0f / in->flow_scale, (uint64_t)in->generations, load_shader);
            else
                LSFG_3_1::initializeExternal(in->gipa, in->instance, in->phys, in->device, in->queue_family, in->queue,
                                             in->hdr != 0, 1.0f / in->flow_scale, (uint64_t)in->generations, load_shader);
        } catch (...) {
            g_lib_broken[mode] = true;
            throw;
        }
        pc_open(in);  // (después: su volkLoadDevice acaba de poner los punteros del dispositivo)
        return 0;
    } catch (const std::exception &e) {
        set_err(err, errlen, e.what());
        return -4;
    } catch (const dxvk::DxvkError &e) {
        set_err(err, errlen, e.message());
        return -4;
    } catch (...) {
        set_err(err, errlen, "error desconocido al iniciarse");
        return -4;
    }
}

LSFG_SMRPG_API int lsfg_smrpg_create(const VkImage in[2], const VkImage *out, int n, uint32_t w, uint32_t h, VkFormat fmt,
                                     char *err, int errlen) {
    try {
        std::vector<VkImage> outs(out, out + n);
        VkExtent2D ext{w, h};
        int ctx = g_perf ? LSFG_3_1P::createContextFromImages(in[0], in[1], outs, ext, fmt)
                         : LSFG_3_1::createContextFromImages(in[0], in[1], outs, ext, fmt);
        if (ctx >= 0) pc_save();  // (sus tuberías ya están: si la caché cambió, a disco)
        return ctx;
    } catch (const std::exception &e) {
        set_err(err, errlen, e.what());
        return -1;
    } catch (const dxvk::DxvkError &e) {
        set_err(err, errlen, e.message());
        return -1;
    } catch (...) {
        set_err(err, errlen, "error desconocido al crear el contexto");
        return -1;
    }
}

LSFG_SMRPG_API int lsfg_smrpg_present(int ctx, char *err, int errlen) {
    try {
        std::vector<int> none;
        if (g_perf) LSFG_3_1P::presentContext(ctx, -1, none);
        else LSFG_3_1::presentContext(ctx, -1, none);
        return 0;
    } catch (const LSFG::vulkan_error &e) {
        set_err(err, errlen, e.what());
        return e.error() == VK_TIMEOUT ? -2 : -1;
    } catch (const std::exception &e) {
        set_err(err, errlen, e.what());
        return -1;
    } catch (const dxvk::DxvkError &e) {
        set_err(err, errlen, e.message());
        return -1;
    } catch (...) {
        set_err(err, errlen, "error desconocido al generar");
        return -1;
    }
}

LSFG_SMRPG_API void lsfg_smrpg_destroy(int ctx) {
    try {
        if (g_perf) LSFG_3_1P::deleteContext(ctx);
        else LSFG_3_1::deleteContext(ctx);
    } catch (...) {
    }
}

LSFG_SMRPG_API void lsfg_smrpg_finalize(void) {
    try {
        pc_close();  // (antes que LSFG: guarda la caché y deja su vkCreateComputePipelines como estaba)
        LSFG_3_1P::finalize();
        LSFG_3_1::finalize();
    } catch (...) {
    }
    g_res.clear();
    g_spv.clear();
}

}  // extern "C"

// GLSL -> SPIR-V (shaderc), descriptor layouts from the SPIR-V itself, translated NVN programs, built-in shaders.
#include <pthread.h>
#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "shaders.h"
#include "../screen_filter.h"

void vk_shaders_init(void) {  // (compilers are per thread: check shaderc works at all)
    shaderc_compiler_t c = shaderc_compiler_initialize();
    if (!c) FATAL("shaderc_compiler_initialize failed\n");
    shaderc_compiler_release(c);
}

static shaderc_shader_kind kind_of(int stage) {
    return stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
}

// SPIR-V disk cache: cache/spirv/<hash of stage + source>.spv, so a known shader costs a file read instead of a
// GLSL compile (the driver's pipeline cache only covers the step after this one).
static uint64_t src_hash(const char *src, int stage) {
    uint64_t h = (0xcbf29ce484222325ull ^ (uint64_t)stage) + 3;  // + version of the post-processing (no_contraction)
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}

static uint32_t *cache_load(uint64_t h, size_t *nwords) {
    char path[64];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *d = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d && d[0] != 0x07230203) { free(d); d = NULL; }
    if (d) *nwords = (size_t)n / 4;
    return d;
}

static void cache_store(uint64_t h, const uint32_t *spv, size_t nwords) {
    static int made;
    if (!made) { made = 1; mkdir("cache", 0755); mkdir("cache/spirv", 0755); }
    char path[64], tmp[80];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)&made ^ (unsigned long long)(uintptr_t)spv);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(spv, 4, nwords, f) == nwords;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) remove(tmp);  // (another thread may have stored it first)
}

// Vertex shaders: every float add/sub/mul/div/fma gets NoContraction, so no compiler fuses or splits them. Depth
// pre-pass and colour pass are different shaders computing the same position, and the colour pass tests EQUAL:
// any difference in fusing leaves holes (foliage flickered black). GLSL's `precise gl_Position` only covers what
// glslang traces back from the position, which the translated register code defeats.
static bool float_arith(const uint32_t *w, uint32_t wc, uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136:  // FNegate FAdd FSub FMul FDiv
    case 142: case 143: case 144: case 145: case 146: case 148:  // vector/matrix products, Dot
        return true;
    case 12: return wc >= 5 && w[4] == 50;  // ExtInst GLSL.std.450 Fma
    default: return false;
    }
}

static uint32_t *no_contraction(uint32_t *spv, size_t *nwords) {
    enum { OP_DECORATE = 71, DEC_NO_CONTRACTION = 42 };
    size_t n = *nwords, insert = 0, count = 0;
    uint32_t bound = spv[3];
    uint8_t *done = calloc(bound, 1);  // already decorated (glslang does it for what it traces from precise)
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (!insert && op >= 19 && op <= 39) insert = i;  // first type declaration: end of the annotations
        if (op == OP_DECORATE && wc >= 3 && spv[i + 2] == DEC_NO_CONTRACTION && spv[i + 1] < bound) done[spv[i + 1]] = 1;
        i += wc;
    }
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) count++;
        i += wc;
    }
    if (!insert || !count) { free(done); return spv; }
    uint32_t *out = malloc((n + count * 3) * 4);
    memcpy(out, spv, insert * 4);
    size_t o = insert;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) {
            done[spv[i + 2]] = 1;
            out[o++] = 3u << 16 | OP_DECORATE;
            out[o++] = spv[i + 2];  // result id
            out[o++] = DEC_NO_CONTRACTION;
        }
        i += wc;
    }
    memcpy(out + o, spv + insert, (n - insert) * 4);
    *nwords = o + (n - insert);
    free(done);
    free(spv);
    return out;
}

// SPIR-V words of src, or NULL (error logged with the source). *nwords set on success. Thread-safe: one shaderc
// compiler per thread (a shared one behind a lock serialised all the pipeline workers).
// No spirv-opt by default: it rewrote the depth pre-pass and colour-pass vertex shaders differently enough that
// their depths stopped matching (EQUAL test: black holes in foliage), and it was most of the compile time; the
// driver optimises anyway. LARECOMP_VK_OPT=1 turns it back on (experiments).
uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords) {
    uint64_t h = src_hash(src, stage);
    static int nocache = -1;
    if (nocache < 0) nocache = getenv("LARECOMP_VK_NO_SPIRV_CACHE") != NULL;
    uint32_t *out = nocache ? NULL : cache_load(h, nwords);
    if (out) return out;
    static __thread shaderc_compiler_t comp;
    if (!comp) comp = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(o, getenv("LARECOMP_VK_OPT") ? shaderc_optimization_level_performance
                                                                                  : shaderc_optimization_level_zero);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, src, strlen(src), kind_of(stage), name, "main", o);
    shaderc_compile_options_release(o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        static int nlog;
        if (nlog++ < 20) LOG("[shader] %s: GLSL compile failed:\n%s\n%s\n", name, shaderc_result_get_error_message(r), src);
    } else {
        size_t n = shaderc_result_get_length(r);
        out = malloc(n);
        memcpy(out, shaderc_result_get_bytes(r), n);
        *nwords = n / 4;
        if (stage == STAGE_VERTEX) out = no_contraction(out, nwords);
        if (!nocache) cache_store(h, out, *nwords);
    }
    shaderc_result_release(r);
    return out;
}

VkShaderModule vk_module(const uint32_t *spv, size_t nwords) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nwords * 4, .pCode = spv};
    VkShaderModule m;
    return vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name) {
    size_t n;
    uint32_t *spv = vk_glsl_to_spirv(src, stage, name, &n);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    return m;
}

// ================================================================== reflection
// Just enough SPIR-V parsing to list the descriptor bindings a module declares (set, binding, type).
enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
       OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71 };
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_SET = 34 };
enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12 };

int vk_reflect(const uint32_t *spv, size_t n, VkBindingInfo *out, int max) {
    if (n < 5 || spv[0] != 0x07230203) return 0;
    uint32_t bound = spv[3];
    int32_t *binding = malloc(bound * sizeof *binding), *set = malloc(bound * sizeof *set);
    uint8_t *flags = calloc(bound, 1);    // 1 Block, 2 BufferBlock
    uint32_t *op = calloc(bound, sizeof *op), *ref = calloc(bound, sizeof *ref), *sc = calloc(bound, sizeof *sc);
    for (uint32_t i = 0; i < bound; i++) binding[i] = set[i] = -1;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, o = spv[i] & 0xFFFF;
        if (!wc) break;
        const uint32_t *w = spv + i;
        if (o == OP_DECORATE && wc >= 3 && w[1] < bound) {
            if (w[2] == DEC_BINDING && wc >= 4) binding[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_SET && wc >= 4) set[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_BLOCK) flags[w[1]] |= 1;
            else if (w[2] == DEC_BUFFER_BLOCK) flags[w[1]] |= 2;
        } else if ((o == OP_TYPE_IMAGE || o == OP_TYPE_SAMPLER || o == OP_TYPE_SAMPLED_IMAGE || o == OP_TYPE_STRUCT) && wc >= 2 && w[1] < bound) {
            op[w[1]] = o;
        } else if ((o == OP_TYPE_ARRAY || o == OP_TYPE_RUNTIME_ARRAY) && wc >= 3 && w[1] < bound) {
            op[w[1]] = o;
            ref[w[1]] = w[2];
        } else if (o == OP_TYPE_POINTER && wc >= 4 && w[1] < bound) {
            op[w[1]] = o;
            sc[w[1]] = w[2];
            ref[w[1]] = w[3];
        } else if (o == OP_VARIABLE && wc >= 4 && w[2] < bound && w[1] < bound) {
            op[w[2]] = o;
            ref[w[2]] = w[1];  // pointer type
            sc[w[2]] = w[3];
        }
        i += wc;
    }
    int cnt = 0;
    for (uint32_t id = 0; id < bound && cnt < max; id++) {
        if (op[id] != OP_VARIABLE || binding[id] < 0) continue;
        uint32_t t = ref[ref[id]];  // pointee
        while (t < bound && (op[t] == OP_TYPE_ARRAY || op[t] == OP_TYPE_RUNTIME_ARRAY)) t = ref[t];
        VkDescriptorType dt;
        if (t >= bound) continue;
        if (op[t] == OP_TYPE_SAMPLED_IMAGE) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (op[t] == OP_TYPE_IMAGE) dt = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (op[t] == OP_TYPE_SAMPLER) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (op[t] == OP_TYPE_STRUCT && (sc[id] == SC_STORAGE_BUFFER || (flags[t] & 2))) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (op[t] == OP_TYPE_STRUCT && sc[id] == SC_UNIFORM) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else continue;
        out[cnt++] = (VkBindingInfo){(uint32_t)(set[id] < 0 ? 0 : set[id]), (uint32_t)binding[id], dt};
    }
    free(binding); free(set); free(flags); free(op); free(ref); free(sc);
    return cnt;
}

VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding lb[96];
    uint32_t k = 0;
    for (int i = 0; i < n && k < 96; i++)
        if (b[i].set == set) lb[k++] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].type, 1, stages, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = k, .pBindings = lb};
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

// ================================================================== built-in shaders
static const char *builtin_src(const char *name, int *stage) {
    if (!strcmp(name, "present.vert")) {
        *stage = STAGE_VERTEX;
        // pv[0].x: flip (lower-left origin), pv[1]: uv crop (origin, size). Fullscreen triangle; Vulkan NDC y points down.
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; vec4 crop; } pc;\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  vec2 u = vec2(p.x, pc.a.x > 0.5 ? 1.0 - p.y : p.y);\n"
               "  uv = pc.crop.xy + u * pc.crop.zw;\n"
               "}\n";
    }
    if (!strcmp(name, "present.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform F { layout(offset = 32) vec4 pf; };\n"  // (the screen filter)
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 suv = uv;\n"
               // (SMRPG, del PC 0.49.1-beta) The game's own final image comes with its first row and last column
               // half-blended with black (at native 1:1 too): its post chain leaves the last row/column of its
               // buffers unwritten and its final blit blends them in. pf.y (LARECOMP_NO_EDGE_FIX=1): off. The outer
               // samples land on the second texel's centre: the edge replicates the neighbour instead of the blend.
               "  if (pf.y < 0.5) {\n"
               "    vec2 ts = vec2(textureSize(t, 0));\n"
               "    if (ts.x > 3.0 && ts.y > 3.0)\n"
               "      suv = clamp(suv, 1.5 / ts, 1.0 - 1.5 / ts);\n"
               "  }\n"
               "  vec3 c = texture(t, suv).rgb;\n"
               SCREEN_FILTER_SRC("vec3")
               "  o = vec4(c, 1.0);\n"
               "}\n";
    }
    // ---- decoded video, NV12 -> RGBA (exec.c vk_convert_nv12): limited range BT.601, as swscale did (the movies
    // declare no matrix). Fullscreen triangle; Vulkan NDC y points down, so uv = p.
    if (!strcmp(name, "nv12.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  uv = p;\n"
               "}\n";
    }
    if (!strcmp(name, "nv12.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D ty;\n"
               "layout(set = 0, binding = 1) uniform sampler2D tuv;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  float y = (texture(ty, uv).r - 16.0 / 255.0) * (255.0 / 219.0);\n"
               "  vec2 c = (texture(tuv, uv).rg - 128.0 / 255.0) * (255.0 / 224.0);\n"
               "  o = vec4(clamp(vec3(y + 1.402 * c.y, y - 0.344136 * c.x - 0.714136 * c.y, y + 1.772 * c.x), 0.0, 1.0), 1.0);\n"
               "}\n";
    }
    // ---- post passes before the present (Video tab "Anti-aliasing" / "Upscaler"): exec.c vk_present_draw.
    // Each draws a fullscreen triangle into an image the size of its output; push constants: two vec4.
    // Depth copy that upsamples (exec.c depth_upsample_draw): bilinear depth from the source region. a.xy = the
    // destination rectangle's origin, a.zw = source texels per destination pixel; b.xy / b.zw = the source region's
    // first / last texel. Texel fetches: depth formats need no linear-filter support this way.
    if (!strcmp(name, "depthup.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } p;\n"
               "void main() {\n"
               "  vec2 s = p.b.xy + (gl_FragCoord.xy - p.a.xy) * p.a.zw - 0.5;\n"
               "  vec2 fl = floor(s);\n"
               "  vec2 f = s - fl;\n"
               "  ivec2 lo = ivec2(p.b.xy), hi = ivec2(p.b.zw);\n"
               "  ivec2 i0 = clamp(ivec2(fl), lo, hi), i1 = clamp(ivec2(fl) + ivec2(1), lo, hi);\n"
               "  float d00 = texelFetch(t, i0, 0).r;\n"
               "  float d10 = texelFetch(t, ivec2(i1.x, i0.y), 0).r;\n"
               "  float d01 = texelFetch(t, ivec2(i0.x, i1.y), 0).r;\n"
               "  float d11 = texelFetch(t, i1, 0).r;\n"
               "  gl_FragDepth = mix(mix(d00, d10, f.x), mix(d01, d11, f.x), f.y);\n"
               "}\n";
    }
    if (!strcmp(name, "post.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "}\n";
    }
    // SMAA 1x (third_party/smaa, runtime/nvn/smaa_data.c): the reference SMAA.hlsl in its GLSL mode, High preset,
    // with an entry point per pass. a = SMAA_RT_METRICS (1 / size, size). The vertex-stage offset helpers run per
    // pixel here. Luma edges (t0 = colour) -> blending weights (t0 = edges, t1 = area, t2 = search) -> neighbourhood
    // blending (t0 = colour, t1 = weights). Vulkan's texture origin is top-left, as SMAA expects (no flips).
    if (!strncmp(name, "smaa_", 5)) {
        *stage = STAGE_FRAGMENT;
        static char *src[3];
        int k = !strcmp(name, "smaa_edge.frag") ? 0 : !strcmp(name, "smaa_weight.frag") ? 1 : !strcmp(name, "smaa_blend.frag") ? 2 : -1;
        if (k < 0) return NULL;
        if (!src[k]) {
            extern const char smaa_hlsl_src[];
            static const char *head =
                "#version 450\n"
                "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
                "#define SMAA_RT_METRICS pc.a\n"
                "#define SMAA_GLSL_4\n"
                "#define SMAA_PRESET_HIGH\n";
            static const char *mains[3] = {
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off[3];\n"
                "  SMAAEdgeDetectionVS(uv, off);\n"
                "  o = vec4(SMAALumaEdgeDetectionPS(uv, off, t0), 0.0, 0.0);\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy, pix;\n"
                "  vec4 off[3];\n"
                "  SMAABlendingWeightCalculationVS(uv, pix, off);\n"
                "  o = SMAABlendingWeightCalculationPS(uv, pix, off, t0, t1, t2, vec4(0.0));\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off;\n"
                "  SMAANeighborhoodBlendingVS(uv, off);\n"
                "  o = vec4(SMAANeighborhoodBlendingPS(uv, off, t0, t1).rgb, 1.0);\n"
                "}\n"};
            size_t n = strlen(head) + strlen(smaa_hlsl_src) + strlen(mains[k]) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s", head, smaa_hlsl_src, mains[k]);
        }
        return src[k];
    }
    // AMD FidelityFX Super Resolution 1 (MIT), EASU: edge-adaptive upscale. a = (in / out scale xy, offset zw),
    // b.xy = input size. 12 taps around the source pixel, direction / edge length from luma, Lanczos-2-like
    // weights stretched along the edge, clamped to the nearest 4 (no ringing).
    if (!strcmp(name, "easu.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void setf(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
               "  float dx = lD - lB, dy = lE - lA;\n"
               "  float lx = clamp(abs(dx) / max(max(abs(lD - lC), abs(lC - lB)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  float ly = clamp(abs(dy) / max(max(abs(lE - lC), abs(lC - lA)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  dir += vec2(dx, dy) * w;\n"
               "  len += (lx * lx + ly * ly) * w;\n"
               "}\n"
               "void tapf(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {\n"
               "  vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;\n"
               "  float d2 = min(dot(v, v), clp);\n"
               "  float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;\n"
               "  wb *= wb; wa *= wa;\n"
               "  float w = (1.5625 * wb - 0.5625) * wa;\n"
               "  ac += c * w; aw += w;\n"
               "}\n"
               "void main() {\n"
               "  vec2 pp = floor(gl_FragCoord.xy) * pc.a.xy + pc.a.zw;\n"
               "  vec2 fpf = floor(pp); pp -= fpf;\n"
               "  ivec2 fp = ivec2(fpf);\n"
               "  //    b c\n"
               "  //  e f g h\n"
               "  //  i j k l\n"
               "  //    n o\n"
               "  vec3 bC = tap(fp + ivec2(0, -1)), cC = tap(fp + ivec2(1, -1));\n"
               "  vec3 eC = tap(fp + ivec2(-1, 0)), fC = tap(fp), gC = tap(fp + ivec2(1, 0)), hC = tap(fp + ivec2(2, 0));\n"
               "  vec3 iC = tap(fp + ivec2(-1, 1)), jC = tap(fp + ivec2(0, 1)), kC = tap(fp + ivec2(1, 1)), lC = tap(fp + ivec2(2, 1));\n"
               "  vec3 nC = tap(fp + ivec2(0, 2)), oC = tap(fp + ivec2(1, 2));\n"
               "  float bL = lum(bC), cL = lum(cC), eL = lum(eC), fL = lum(fC), gL = lum(gC), hL = lum(hC);\n"
               "  float iL = lum(iC), jL = lum(jC), kL = lum(kC), lL = lum(lC), nL = lum(nC), oL = lum(oC);\n"
               "  vec2 dir = vec2(0.0);\n"
               "  float len = 0.0;\n"
               "  setf(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
               "  setf(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
               "  setf(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
               "  setf(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
               "  float dr = dot(dir, dir);\n"
               "  bool zro = dr < 1.0 / 32768.0;\n"
               "  dir = zro ? vec2(1.0, 0.0) : dir * inversesqrt(dr);\n"
               "  len = len * 0.5; len *= len;\n"
               "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
               "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
               "  float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;\n"
               "  vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));\n"
               "  vec3 ac = vec3(0.0);\n"
               "  float aw = 0.0;\n"
               "  tapf(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);\n"
               "  tapf(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);\n"
               "  tapf(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);\n"
               "  tapf(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);\n"
               "  tapf(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);\n"
               "  tapf(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);\n"
               "  tapf(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);\n"
               "  tapf(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);\n"
               "  tapf(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);\n"
               "  tapf(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);\n"
               "  tapf(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);\n"
               "  tapf(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);\n"
               "  o = vec4(min(mx, max(mn, ac / aw)), 1.0);\n"
               "}\n";
    }
    // (SMRPG Android) Snapdragon Game Super Resolution 1 (SGSR 1), Copyright (c) 2025 Qualcomm Innovation Center, Inc.,
    // BSD-3-Clause (third_party/snapdragon-gsr): de sgsr1_shader_mobile.frag, modo RGBA (el verde como luma), umbral de
    // borde 8/255 y nitidez 2 (los de Qualcomm para móviles). Escalado con un filtro de 12 muestras tipo Lanczos y
    // afilado adaptativo en una sola pasada, hecho para las Adreno. a.xy: la coordenada de textura por píxel de salida
    // (contenido / salida / tamaño de la textura); a.zw: 1 / tamaño de la textura; b.xy: tamaño de la textura; b.zw: la
    // mayor coordenada de reunión (gather) dentro del contenido (con resolución dinámica, el resto guarda imágenes viejas).
    if (!strcmp(name, "sgsr1.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float fastLanczos2(float x) { float wA = x - 4.0; float wB = x * wA - wA; wA *= wA; return wB * wA; }\n"
               "vec2 weightY(float dx, float dy, float c, float std) {\n"
               "  float x = (dx * dx + dy * dy) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);\n"
               "  float w = fastLanczos2(x);\n"
               "  return vec2(w, w * c);\n"
               "}\n"
               "void main() {\n"
               "  const float edgeThreshold = 8.0 / 255.0, edgeSharpness = 2.0;\n"
               "  vec4 vi = vec4(pc.a.zw, pc.b.xy);\n"
               "  vec2 uv = gl_FragCoord.xy * pc.a.xy;\n"
               "  vec4 color = vec4(textureLod(t, uv, 0.0).xyz, 0.0);\n"
               "  vec2 imgCoord = uv * vi.zw + vec2(-0.5, 0.5);\n"
               "  vec2 imgCoordPixel = floor(imgCoord);\n"
               "  vec2 coord = min(imgCoordPixel * vi.xy, pc.b.zw);\n"
               "  vec2 pl = imgCoord - imgCoordPixel;\n"
               "  vec4 left = textureGather(t, coord, 1);\n"
               "  float edgeVote = abs(left.z - left.y) + abs(color.y - left.y) + abs(color.y - left.z);\n"
               "  if (edgeVote > edgeThreshold) {\n"
               "    coord.x += vi.x;\n"
               "    vec4 right = textureGather(t, coord + vec2(vi.x, 0.0), 1);\n"
               "    vec4 upDown;\n"
               "    upDown.xy = textureGather(t, coord + vec2(0.0, -vi.y), 1).wz;\n"
               "    upDown.zw = textureGather(t, coord + vec2(0.0, vi.y), 1).yx;\n"
               "    float mean = (left.y + left.z + right.x + right.w) * 0.25;\n"
               "    left -= vec4(mean); right -= vec4(mean); upDown -= vec4(mean);\n"
               "    color.w = color.y - mean;\n"
               "    float sum = abs(left.x) + abs(left.y) + abs(left.z) + abs(left.w) + abs(right.x) + abs(right.y) + abs(right.z) +\n"
               "                abs(right.w) + abs(upDown.x) + abs(upDown.y) + abs(upDown.z) + abs(upDown.w);\n"
               "    float std = 2.181818 / sum;\n"
               "    vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);\n"
               "    aWY += weightY(pl.x, pl.y - 2.0, upDown.w, std);\n"
               "    aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std);\n"
               "    aWY += weightY(pl.x, pl.y - 1.0, left.y, std);\n"
               "    aWY += weightY(pl.x, pl.y, left.z, std);\n"
               "    aWY += weightY(pl.x + 1.0, pl.y, left.w, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std);\n"
               "    aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std);\n"
               "    aWY += weightY(pl.x - 2.0, pl.y, right.z, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y, right.w, std);\n"
               "    float finalY = aWY.y / aWY.x;\n"
               "    float maxY = max(max(left.y, left.z), max(right.x, right.w));\n"
               "    float minY = min(min(left.y, left.z), min(right.x, right.w));\n"
               "    finalY = clamp(edgeSharpness * finalY, minY, maxY);\n"
               "    float deltaY = clamp(finalY - color.w, -23.0 / 255.0, 23.0 / 255.0);\n"
               "    color.xyz = clamp(color.xyz + deltaY, 0.0, 1.0);\n"
               "  }\n"
               "  o = vec4(color.xyz, 1.0);\n"
               "}\n";
    }
    // (SMRPG Android) Snapdragon Game Super Resolution 2 (SGSR 2), Copyright (c) 2024 Qualcomm Innovation Center, Inc.,
    // BSD-3-Clause (third_party/snapdragon-gsr): su variante de 2 pasadas con sombreadores de fragmentos (la que Qualcomm
    // recomienda para móviles), de sgsr2_convert.fs / sgsr2_upscale.fs, adaptada al runtime (exec.c gsr2_dispatch):
    // coordenadas por gl_FragCoord, lo dibujado en la esquina de imágenes más grandes (la resolución dinámica de FSR),
    // la profundidad invertida de URP, y los vectores de movimiento de URP en vez de los codificados de su ejemplo.
    // 1) gsr2conv.frag, al tamaño de lo dibujado (rw x rh): t0 = la profundidad, t1 = los vectores de URP (RG16F,
    // ahora - antes en UV de lo dibujado). Sale (movimiento x2 = en NDC, como lo espera SGSR 2; recorte por
    // profundidad; 0; 0). a.xy = 1 / tamaño de las imágenes, a.zw = (rw, rh); b.x = tan(campo vertical / 2) * rw / rh,
    // b.y = 1 con la profundidad invertida, b.zw = multiplicadores de los vectores (2, 2; LARECOMP_GSR2_MV para pruebas).
    // El 2x2 central se toma por el orden de textureGather de Vulkan (x = (i0, j1), y = (i1, j1), z = (i1, j0),
    // w = (i0, j0)): el original lo leía con otro orden y comparaba filas de una por encima y dos por debajo.
    if (!strcmp(name, "gsr2conv.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
               "layout(set = 0, binding = 1) uniform sampler2D mv;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float near4(vec4 v, bool rev) { return rev ? max(max(v.x, v.y), max(v.z, v.w)) : min(min(v.x, v.y), min(v.z, v.w)); }\n"
               "float nearer(float x, float y, bool rev) { return rev ? max(x, y) : min(x, y); }\n"
               "void main() {\n"
               "  bool rev = pc.b.y > 0.5;\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  vec2 rcp = pc.a.xy, rs = pc.a.zw;\n"
               "  vec2 lim = (rs - 1.0) * rcp;\n"
               "  vec2 c = vec2(p) * rcp;\n"
               "  vec4 g00 = textureGather(dep, min(c, lim), 0);\n"
               "  vec4 g10 = textureGather(dep, min(c + vec2(2.0 * rcp.x, 0.0), lim), 0);\n"
               "  vec4 g01 = textureGather(dep, min(c + vec2(0.0, 2.0 * rcp.y), lim), 0);\n"
               "  vec4 g11 = textureGather(dep, min(c + 2.0 * rcp, lim), 0);\n"
               "  float dc = nearer(nearer(g00.y, g10.x, rev), nearer(g01.z, g11.w, rev), rev);\n"
               "  float depthclip = 0.0;\n"
               "  bool far = rev ? dc <= 1.0e-5 : dc >= 1.0 - 1.0e-5;\n"
               "  if (!far) {\n"
               "    float sep = 1.37e-05 * pc.b.x * length(rs) * (rev ? dc : 1.0 - dc);\n"
               "    float w = clamp(sep / (abs(dc - near4(g00, rev)) + 1.19e-07), 0.0, 1.0);\n"
               "    w += clamp(sep / (abs(dc - near4(g10, rev)) + 1.19e-07), 0.0, 1.0);\n"
               "    w += clamp(sep / (abs(dc - near4(g01, rev)) + 1.19e-07), 0.0, 1.0);\n"
               "    w += clamp(sep / (abs(dc - near4(g11, rev)) + 1.19e-07), 0.0, 1.0);\n"
               "    depthclip = clamp(1.0 - w * 0.25, 0.0, 1.0);\n"
               "  }\n"
               "  o = vec4(texelFetch(mv, p, 0).xy * pc.b.zw, depthclip, 0.0);\n"
               "}\n";
    }
    // 2) gsr2up.frag, al tamaño de la salida (ow x oh), en la historia nueva: t0 = la historia anterior (ow x oh),
    // t1 = lo de gsr2conv (rw x rh), t2 = el color de la escena (texelFetch: lo dibujado, en la esquina). Las tres van en
    // la esquina de imágenes más grandes: sus coordenadas, en píxeles y con textureSize. a = (rw, rh,
    // ow, oh), b = (desplazamiento de subpíxel del fotograma en píxeles de lo dibujado, minLerpContribution, reset).
    // scaleRatio sale de los tamaños, como en el ejemplo de Qualcomm. La historia con NaN o infinitos no se usa.
    // Tres cosas de las variantes de compute de Qualcomm (glsl_2_pass_cs / glsl_3_pass_cs) que la de fragmentos no trae:
    // - el color se comprime a [0, 1) con c / (1 + max(c)) antes de la caja y de la mezcla (su preExposure = 1): la de
    //   fragmentos es para LDR (su ejemplo dibuja en RGBA8 sRGB) y la escena de URP es HDR; la historia se guarda
    //   comprimida y gsr2copy la expande al escribir la escena;
    // - el ancho del filtro, con la ampliación hasta 1,99: a x3 los 5 pesos de Lanczos podían valer 0 (0 / 0 = NaN);
    // - con reset, solo lo de este fotograma (con pesos de suma negativa la fórmula aún mezclaba la historia vieja).
    if (!strcmp(name, "gsr2up.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D hist;\n"
               "layout(set = 0, binding = 1) uniform sampler2D mdc;\n"
               "layout(set = 0, binding = 2) uniform sampler2D col;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float FastLanczos(float base) { float y = base - 1.0; float y2 = y * y; return (0.75 * y + y2) * y2; }\n"
               "vec3 tm(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }\n"
               "void main() {\n"
               "  vec2 rs = pc.a.xy, os = pc.a.zw, jit = pc.b.xy;\n"
               "  float biasmax = min(os.x / rs.x, 1.99);\n"
               "  float scalefactor = min(20.0, pow((os.x * os.y) / (rs.x * rs.y), 3.0));\n"
               "  vec2 Hruv = gl_FragCoord.xy / os;\n"
               "  vec2 Jitteruv = clamp(Hruv + jit / rs, 0.0, 1.0);\n"
               "  ivec2 InputPos = min(ivec2(Jitteruv * rs), ivec2(rs) - 1);\n"
               "  ivec2 hi = ivec2(rs) - 1;\n"
               "  vec2 msz = vec2(textureSize(mdc, 0)), hsz = vec2(textureSize(hist, 0));\n"
               "  vec3 mda = textureLod(mdc, min(Jitteruv * rs, rs - 0.5) / msz, 0.0).xyz;\n"
               "  vec2 Motion = mda.xy;\n"
               "  vec2 PrevUV = clamp(Hruv - 0.5 * Motion, 0.0, 1.0);\n"
               "  float depthfactor = mda.z;\n"
               "  vec3 HistoryColor = textureLod(hist, min(PrevUV * os, os - 0.5) / hsz, 0.0).xyz;\n"
               "  if (any(isnan(HistoryColor)) || any(isinf(HistoryColor))) HistoryColor = vec3(0.0);\n"
               "  vec4 Upsampledcw = vec4(0.0);\n"
               "  float biasmin = max(1.0, 0.3 + 0.3 * biasmax);\n"
               "  float kernelbias = mix(biasmax, biasmin, 0.25 * depthfactor) * 0.5;\n"
               "  float motion_viewport_len = length(Motion * os);\n"
               "  float curvebias = mix(-2.0, -3.0, clamp(motion_viewport_len * 0.02, 0.0, 1.0));\n"
               "  vec3 rectboxcenter = vec3(0.0), rectboxvar = vec3(0.0);\n"
               "  float rectboxweight = 0.0;\n"
               "  vec2 srcpos = vec2(InputPos) + vec2(0.5) - jit;\n"
               "  float kernelbias2 = kernelbias * kernelbias;\n"
               "  vec2 srcpos_srcOutputPos = srcpos - Hruv * rs;\n"
               "  vec3 rectboxmin = vec3(1.0e30), rectboxmax = vec3(-1.0e30);\n"
               "  const ivec2 offs[5] = ivec2[5](ivec2(0, 1), ivec2(1, 0), ivec2(-1, 0), ivec2(0, 0), ivec2(0, -1));\n"
               "  for (int i = 0; i < 5; i++) {\n"
               "    vec3 samplecolor = tm(texelFetch(col, clamp(InputPos + offs[i], ivec2(0), hi), 0).xyz);\n"
               "    vec2 baseoffset = srcpos_srcOutputPos + vec2(offs[i]);\n"
               "    float baseoffset_dot = dot(baseoffset, baseoffset);\n"
               "    float weight = FastLanczos(clamp(baseoffset_dot * kernelbias2, 0.0, 1.0));\n"
               "    Upsampledcw += vec4(samplecolor * weight, weight);\n"
               "    float boxweight = exp(baseoffset_dot * curvebias);\n"
               "    rectboxmin = min(rectboxmin, samplecolor);\n"
               "    rectboxmax = max(rectboxmax, samplecolor);\n"
               "    vec3 wsample = samplecolor * boxweight;\n"
               "    rectboxcenter += wsample;\n"
               "    rectboxvar += samplecolor * wsample;\n"
               "    rectboxweight += boxweight;\n"
               "  }\n"
               "  rectboxweight = 1.0 / rectboxweight;\n"
               "  rectboxcenter *= rectboxweight;\n"
               "  rectboxvar *= rectboxweight;\n"
               "  rectboxvar = sqrt(abs(rectboxvar - rectboxcenter * rectboxcenter));\n"
               "  vec3 up = abs(Upsampledcw.w) > 1.0e-6 ? Upsampledcw.xyz / Upsampledcw.w : tm(texelFetch(col, InputPos, 0).xyz);\n"
               "  Upsampledcw.xyz = clamp(up, rectboxmin - vec3(0.075), rectboxmax + vec3(0.075));\n"
               "  if (pc.b.w > 0.5) { o = vec4(Upsampledcw.xyz, 0.0); return; }\n"
               "  Upsampledcw.w = Upsampledcw.w * (1.0 / 3.0);\n"
               "  float baseupdate = 1.0 - depthfactor;\n"
               "  baseupdate = min(baseupdate, mix(baseupdate, Upsampledcw.w * 10.0, clamp(10.0 * motion_viewport_len, 0.0, 1.0)));\n"
               "  baseupdate = min(baseupdate, mix(baseupdate, Upsampledcw.w, clamp(motion_viewport_len * 0.05, 0.0, 1.0)));\n"
               "  float basealpha = baseupdate;\n"
               "  float boxscale = max(depthfactor, clamp(motion_viewport_len * 0.05, 0.0, 1.0));\n"
               "  vec3 sboxvar = rectboxvar * mix(scalefactor, 1.0, boxscale);\n"
               "  rectboxmax = min(rectboxmax, rectboxcenter + sboxvar);\n"
               "  rectboxmin = max(rectboxmin, rectboxcenter - sboxvar);\n"
               "  vec3 clampedcolor = clamp(HistoryColor, rectboxmin, rectboxmax);\n"
               "  float startLerpValue = (abs(mda.x) + abs(mda.y)) > 0.000001 ? 0.0 : pc.b.z;\n"
               "  float lerpcontribution = (any(greaterThan(rectboxmin, HistoryColor)) || any(greaterThan(HistoryColor, rectboxmax))) ? startLerpValue : 1.0;\n"
               "  HistoryColor = mix(clampedcolor, HistoryColor, clamp(lerpcontribution, 0.0, 1.0));\n"
               "  basealpha = mix(min(basealpha, 0.1), basealpha, clamp(lerpcontribution, 0.0, 1.0));\n"
               "  float alphasum = max(1.192e-07, basealpha + Upsampledcw.w);\n"
               "  float alpha = clamp(Upsampledcw.w / alphasum + pc.b.w, 0.0, 1.0);\n"
               "  o = vec4(mix(HistoryColor, Upsampledcw.xyz, alpha), 0.0);\n"
               "}\n";
    }
    // 3) gsr2copy.frag: la historia nueva (t0, ow x oh, comprimida) a la escena, expandida (la inversa de tm de gsr2up),
    // en su formato, con alfa 1
    if (!strcmp(name, "gsr2copy.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec3 c = texelFetch(t, ivec2(gl_FragCoord.xy), 0).rgb;\n"
               "  c = any(isnan(c)) ? vec3(0.0) : clamp(c, 0.0, 1.0);\n"
               "  o = vec4(c / max(1.0 - max(c.r, max(c.g, c.b)), 1.0e-4), 1.0);\n"
               "}\n";
    }
    // FSR 1 RCAS: contrast-adaptive sharpening of the EASU output (same size). a.x = sharpness (exp2(-stops)),
    // b.xy = image size. The lobe is limited so the result stays inside the neighbours' range; noise is damped.
    if (!strcmp(name, "rcas.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void main() {\n"
               "  ivec2 ip = ivec2(gl_FragCoord.xy);\n"
               "  //    b\n"
               "  //  d e f\n"
               "  //    h\n"
               "  vec3 b = tap(ip + ivec2(0, -1)), d = tap(ip + ivec2(-1, 0)), e = tap(ip), f = tap(ip + ivec2(1, 0)), h = tap(ip + ivec2(0, 1));\n"
               "  float bL = lum(b), dL = lum(d), eL = lum(e), fL = lum(f), hL = lum(h);\n"
               "  float nz = 0.25 * (bL + dL + fL + hL) - eL;\n"
               "  float rng = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
               "  nz = 1.0 - 0.5 * clamp(abs(nz) / max(rng, 1.0 / 65536.0), 0.0, 1.0);\n"
               "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
               "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));\n"
               "  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));\n"
               "  vec3 lobeRGB = max(-hitMin, hitMax);\n"
               "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.a.x * nz;\n"
               "  o = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1.0);\n"
               "}\n";
    }
    // (SMRPG) Reducción con filtro de caja (supersampling, o una imagen interna mayor que la ventana): la huella de cada
    // píxel de salida en la imagen (a.xy: tamaño del contenido, a.zw: el de la salida) con b.x x b.y muestras bilineales
    // repartidas por ella (b.zw: tamaño de la textura), promediadas en espacio lineal (la imagen va codificada en gamma).
    // Con escala entera (x4, x16) cada muestra cae en el centro de un texel: media exacta.
    if (!strcmp(name, "downsample.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 sc = pc.a.xy / pc.a.zw;\n"
               "  vec2 p0 = floor(gl_FragCoord.xy) * sc;\n"
               "  int nx = int(pc.b.x), ny = int(pc.b.y);\n"
               "  vec3 acc = vec3(0.0);\n"
               "  for (int y = 0; y < ny; y++)\n"
               "    for (int x = 0; x < nx; x++) {\n"
               "      vec2 s = p0 + (vec2(x, y) + 0.5) * sc / vec2(nx, ny);\n"
               "      acc += pow(texture(t, s / pc.b.zw).rgb, vec3(2.2));\n"
               "    }\n"
               "  o = vec4(pow(acc / float(nx * ny), vec3(1.0 / 2.2)), 1.0);\n"
               "}\n";
    }
    // (SMRPG) HDR10: la ventana SDR (gamma sRGB) a PQ (ST 2084) en Rec.2020. El blanco SDR va al «blanco de papel»
    // (a.x nits); con a.z > 0 las luces (lo más brillante de la imagen, por encima de ~60 %) se realzan hacia el pico
    // (a.y nits) con una curva suave, para que el HDR se note sin cambiar los tonos medios.
    if (!strcmp(name, "hdr10.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 lin(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }\n"
               "vec3 pq(vec3 nits) {\n"
               "  vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));\n"
               "  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));\n"
               "}\n"
               "void main() {\n"
               "  vec3 c = lin(texelFetch(t, ivec2(gl_FragCoord.xy), 0).rgb);\n"
               "  float paper = max(pc.a.x, 1.0), peak = max(pc.a.y, paper);\n"
               "  float l = max(c.r, max(c.g, c.b));\n"
               "  float hi = clamp((l - 0.6) / 0.4, 0.0, 1.0);\n"
               "  float boost = 1.0 + pc.a.z * (peak / paper - 1.0) * hi * hi;\n"
               "  vec3 n709 = c * paper * boost;\n"
               "  const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);\n"
               "  o = vec4(pq(max(to2020 * n709, vec3(0.0))), 1.0);\n"
               "}\n";
    }
    // (SMRPG) «Negro puro» (exec.c lut_black_fix): la LUT de etalonaje de URP (t = su copia) con el negro a 0. Su texel
    // (0, 0) es lo que sale para el negro, n: (c - m) / (1 - m) con m = n entero hasta 2/255 (el velo del juego es 1/255)
    // y cada vez menos hasta 6/255, sin saltos; un negro más alto es a propósito y se deja.
    if (!strcmp(name, "lutblack.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec4 c = texelFetch(t, ivec2(gl_FragCoord.xy), 0);\n"
               "  vec3 n = texelFetch(t, ivec2(0, 0), 0).rgb;\n"
               "  float k = 1.0 - smoothstep(2.0 / 255.0, 6.0 / 255.0, max(n.r, max(n.g, n.b)));\n"
               "  vec3 m = n * k;\n"
               "  o = vec4(clamp((c.rgb - m) / (vec3(1.0) - m), 0.0, 1.0), c.a);\n"
               "}\n";
    }
    if (!strcmp(name, "clear.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, pc.d.x, 1.0);\n"
               "}\n";
    }
    // clear.frag.<f|i|u><index>: writes the push-constant colour to colour target <index>
    if (!strncmp(name, "clear.frag.", 11)) {
        *stage = STAGE_FRAGMENT;
        static __thread char buf[512];
        char k = name[11];
        int idx = atoi(name + 12);
        const char *ty = k == 'i' ? "ivec4" : k == 'u' ? "uvec4" : "vec4";
        const char *cv = k == 'i' ? "floatBitsToInt(pc.c)" : k == 'u' ? "floatBitsToUint(pc.c)" : "pc.c";
        snprintf(buf, sizeof buf,
                 "#version 450\n"
                 "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
                 "layout(location = %d) out %s o;\n"
                 "void main() { o = %s; }\n", idx, ty, cv);
        return buf;
    }
    if (!strcmp(name, "clear.frag.none")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\nvoid main() {}\n";
    }
    return NULL;
}

VkShaderModule vk_builtin_shader(const char *name) {
    static struct { char name[32]; VkShaderModule m; } cache[64];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) return cache[i].m;
    int stage = 0;
    const char *src = builtin_src(name, &stage);
    if (!src) FATAL("no built-in shader %s\n", name);
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    if (!m) FATAL("built-in shader %s failed to compile\n", name);
    if (n < 64) { snprintf(cache[n].name, sizeof cache[n].name, "%s", name); cache[n++].m = m; }
    return m;
}

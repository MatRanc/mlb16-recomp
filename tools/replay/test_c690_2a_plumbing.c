/*
 * Offline plumbing gate for the c690 0x2A assembler: SPU path with 0x2A
 * skipped, exported planar inputs, one GPU dispatch, and the bytes the RSX
 * walker reads at the ring EA.  See the comment before main().
 *
 * Build (research; needs the captured lifted module):
 *   clang -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
 *     -I <ps3recomp-play>/runtime/spu -I <ps3recomp-play>/include \
 *     -I <mlb16-recomp>/tools/replay \
 *     -o /tmp/opencode/gpu2a/test_c690_2a_plumbing \
 *     <mlb16-recomp>/tools/replay/test_c690_2a_plumbing.c \
 *     <mlb16-recomp>/tools/replay/reg_10f78.c \
 *     <recomp-work>/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
 *     <ps3recomp-play>/runtime/spu/job_c690_native.c \
 *     -l:libvulkan.so.1 -lm
 *
 * Run:
 *   test_c690_2a_plumbing /tmp/opencode/fncap21b/0x10F78 \
 *       c690_2a_assembler.comp /tmp/opencode/gpu2a/c690_2a.spv \
 *       --out-prefix /tmp/opencode/plumb
 *
 * The record count is derived from the SPU output cursor; --count N overrides
 * it (the phase-2a "193 valid" is the draw count, the handler writes 196).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "spu_context.h"
#include "spu_helpers.h"

volatile int g_spu_direct = 1;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context* ctx) = NULL;
uint8_t g_spu_stack_reset_hint[1024];
unsigned g_spu_stack_reset_count = 0;
int g_spu_direct_dummy;

typedef void (*spu_fn)(spu_context*);
typedef struct { uint32_t addr; spu_fn fn; } reg_ent;
extern const reg_ent g_c690_reg[];
extern const int g_c690_reg_n;
static int g_terminal;
static spu_fn lookup_fn(uint32_t a) {
    a &= SPU_LS_MASK;
    for (int i = 0; i < g_c690_reg_n; i++) if (g_c690_reg[i].addr == a) return g_c690_reg[i].fn;
    return NULL;
}
void c690_stats_on_direct(const spu_context* ctx, uint32_t lsa) { (void)ctx; (void)lsa; }
void spu_indirect_branch(spu_context* ctx) {
    spu_fn fn = lookup_fn(ctx->pc);
    if (!fn) { g_terminal = 1; return; }
    fn(ctx);
}
void spu_drain_call(spu_context* ctx, uint32_t ret) {
    for (;;) {
        while (g_spu_trampoline_fn) {
            void (*tf)(spu_context*) = g_spu_trampoline_fn;
            if (tf == spu_indirect_branch && (ctx->pc & SPU_LS_MASK) == (ret & SPU_LS_MASK)) { g_spu_trampoline_fn = NULL; return; }
            g_spu_trampoline_fn = NULL; tf(ctx);
            if (g_terminal) return;
        }
        if ((ctx->pc & SPU_LS_MASK) == (ret & SPU_LS_MASK)) return;
        if (g_terminal) return;
        spu_fn fn = lookup_fn(ctx->pc);
        if (fn) { g_spu_trampoline_fn = spu_indirect_branch; continue; }
        g_terminal = 1; return;
    }
}
void spu_img_restore(spu_context* c, int32_t i) { c->image_id = i; }
void spu_stop(spu_context* ctx) { (void)ctx; }
int g_c690_stats = 0, g_c690_corpus_on = 0;
int c690_native_cull_maybe(void) { return 0; }
int c690_native_cull_enter(spu_context* c) { (void)c; return 0; }
void c690_native_cull_leave(spu_context* c) { (void)c; }
void c690_corpus_stage_enter(const spu_context* c, uint32_t a) { (void)c; (void)a; }
void c690_corpus_stage_exit(const spu_context* c, uint32_t a) { (void)c; (void)a; }
int c690_cull_bypass_mode(void) { return 0; }
int c690_cull_fix_begin(spu_context* c, int f) { (void)c; (void)f; return 0; }
void c690_cull_fix_mid(spu_context* c) { (void)c; }
void c690_cull_fix_end(spu_context* c) { (void)c; }
int c690_native_screen_try(spu_context* c, uint32_t r, void* l) { (void)c; (void)r; (void)l; return 0; }
void c690_corpus_cull_enter(const spu_context* c, uint32_t a) { (void)c; (void)a; }
void c690_corpus_cull_exit(const spu_context* c, uint32_t a) { (void)c; (void)a; }
int c690_cull_fix_maybe(void) { return 0; }
int c690_cull_fix_enter(spu_context* c, int f) { (void)c; (void)f; return 0; }
void c690_cull_fix_leave(spu_context* c) { (void)c; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    switch (channel) {
    case MFC_LSA: ctx->mfc_lsa = value._u32[0]; break;
    case MFC_EAH: ctx->mfc_eah = value._u32[0]; break;
    case MFC_EAL: ctx->mfc_eal = value._u32[0]; break;
    case MFC_Size: ctx->mfc_size = value._u32[0]; break;
    case MFC_TagID: ctx->mfc_tag = value._u32[0]; break;
    case MFC_WrTagMask: ctx->mfc_tag_mask = value._u32[0]; break;
    case MFC_WrTagUpdate: ctx->mfc_tag_status = 0; break;
    default: break;
    }
}
u128 spu_rdch(spu_context* ctx, uint32_t channel) {
    u128 r; r._u32[0] = r._u32[1] = r._u32[2] = r._u32[3] = 0;
    switch (channel) {
    case MFC_RdTagStat: r._u32[0] = 0; break;
    case MFC_RdTagMask: r._u32[0] = ctx->mfc_tag_mask; break;
    case MFC_RdAtomicStat: r._u32[0] = 0; break;
    case SPU_RdDec: r._u32[0] = 0xFFFFFFFFu; break;
    case SPU_RdMachStat: r._u32[0] = 1; break;
    default: break;
    }
    return r;
}
uint32_t spu_rchcnt(spu_context* ctx, uint32_t c) { (void)ctx; (void)c; return 0; }
void spu_fncap_capture(const spu_context* c, uint32_t a) { (void)c; (void)a; }
void spu_register_function(uint32_t a, void (*f)(spu_context*)) { (void)a; (void)f; }

#define CHECK(expr) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "Vulkan error %d at %s:%d: %s\n", (int)_r, __FILE__, __LINE__, #expr); exit(1); } } while (0)

#define OUT_BASE  0x2F870u
#define REC_SIZE  36u
#define REC_COUNT 193u
#define OUT_BYTES (REC_SIZE * REC_COUNT)

typedef struct {
    uint32_t uv_base, col_base, pos_base, nrm_base;
    uint32_t out_base, count, pad0, pad1;
} PushConstants;

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void load_exact(const char* path, void* dst, size_t want) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    if (fread(dst, 1, want, f) != want) { fprintf(stderr, "%s: short read\n", path); exit(1); }
    fclose(f);
}
static uint32_t* read_file_u32(const char* path, size_t* out_words) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (sz & 3)) { fprintf(stderr, "%s: bad SPIR-V size %ld\n", path, sz); exit(1); }
    uint32_t* p = (uint32_t*)malloc((size_t)sz);
    if (!p) exit(1);
    if (fread(p, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "%s: short read\n", path); exit(1); }
    fclose(f);
    if (p[0] != 0x07230203u) { fprintf(stderr, "%s: not SPIR-V\n", path); exit(1); }
    *out_words = (size_t)sz / 4;
    return p;
}
static uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no memory type\n"); exit(1);
}
static void make_buffer(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize size,
                        VkBuffer* buf, VkDeviceMemory* mem, void** mapped) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                               .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    CHECK(vkCreateBuffer(dev, &bci, NULL, buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, *buf, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_memory_type(phys, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CHECK(vkAllocateMemory(dev, &mai, NULL, mem));
    CHECK(vkBindBufferMemory(dev, *buf, *mem, 0));
    CHECK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, mapped));
}


/* The generated module in this build carries the capture wrappers; the
 * plumbing harness does not use them, so provide the off-path symbols. */
int g_c690_tail_on = 0;
void c690_corpus_tail_cmd_enter(const struct spu_context* c, uint32_t a, uint32_t o)
{ (void)c; (void)a; (void)o; }
void c690_corpus_tail_cmd_exit(const struct spu_context* c) { (void)c; }
void c690_corpus_tail_2a_enter(const struct spu_context* c) { (void)c; }
void c690_corpus_tail_2a_exit(const struct spu_context* c) { (void)c; }

/* -------------------------------------------------------------------------
 * Plumbing test: SPU path with 0x2A skipped -> exported planar inputs ->
 * one GPU 0x2A dispatch -> the bytes the walker would see at the ring EA.
 *
 * The SPU 0x2A handler writes its 36-byte records into LS at ctx+0xD0
 * (the output cursor); the job's submit epilogue then copies the LS range
 * [ring_lsa, ring_lsa+ring_size) to the guest ring EA.  This harness:
 *   1. runs the lifted SPU 0x10F78 on the captured IN LS/GPRs (reference),
 *   2. exports only the four planar arrays (nrm/pos/col/uv) + job table into
 *      a compact buffer (4 * count * 16 bytes + output),
 *   3. runs c690_2a_assembler.comp on that compact buffer,
 *   4. compares the GPU records with the SPU records at the output base,
 *   5. applies the ring copy mapping to both LS images and compares the
 *      ring bytes (the bytes the RSX walker reads).
 *
 * Default ring mapping is the real PUT in corpus_edge3s1 job_000000:
 *   PUT ea=0x11765D00 lsa=0x2F000 size=0x30D0
 * which covers the 0x2A output at LS 0x2F870.
 * ------------------------------------------------------------------------- */

#define PLUMB_LS_BYTES 262144u
#define PLUMB_REC_SIZE 36u
#define PLUMB_OUT_BASE_DEFAULT 0x2F870u
#define PLUMB_COUNT_DEFAULT 193u
#define PLUMB_RING_LSA_DEFAULT 0x2F000u
#define PLUMB_RING_SIZE_DEFAULT 0x30D0u
#define PLUMB_RING_EA_DEFAULT 0x11765D00u

typedef struct {
    uint32_t uv_base, col_base, pos_base, nrm_base;
    uint32_t out_base, count, pad0, pad1;
} PlumbPush;

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_file(const char* path, const void* data, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    if (n && fwrite(data, 1, n, f) != n) { perror(path); exit(1); }
    fclose(f);
}

static void run_spu_2a(const uint8_t* ls_in, const uint8_t* gpr_in,
                       uint8_t* ls_out, uint32_t* entry_r3, uint32_t* out_base)
{
    static spu_context c;
    spu_context_init(&c, 0);
    memcpy(c.ls, ls_in, PLUMB_LS_BYTES);
    memcpy(c.gpr, gpr_in, 128 * 16);
    const uint32_t r3 = c.gpr[3]._u32[0] & SPU_LS_MASK;
    *entry_r3 = r3;
    *out_base = be32(c.ls + ((r3 + 0xD0u) & SPU_LS_MASK));
    c.image_id = 6;
    c.pc = 0x10F78;
    c.status = 0;
    c.stop_code = 0;
    g_spu_trampoline_fn = NULL;
    g_terminal = 0;
    spu_fn f = lookup_fn(0x10F78);
    if (!f) { fprintf(stderr, "no lifted 0x10F78\n"); exit(1); }
    f(&c);
    while (!g_terminal && g_spu_trampoline_fn) {
        void (*tf)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = NULL;
        tf(&c);
    }
    memcpy(ls_out, c.ls, PLUMB_LS_BYTES);
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp/opencode/fncap21b/0x10F78";
    const char* comp_path = argc > 2 ? argv[2] : "c690_2a_assembler.comp";
    const char* spv_path = argc > 3 ? argv[3] : "/tmp/opencode/gpu2a/c690_2a.spv";
    uint32_t count = 0;   /* 0 = derive from the SPU output cursor */
    uint32_t ring_lsa = PLUMB_RING_LSA_DEFAULT;
    uint32_t ring_size = PLUMB_RING_SIZE_DEFAULT;
    uint32_t ring_ea = PLUMB_RING_EA_DEFAULT;
    const char* out_prefix = "/tmp/opencode/c690_2a_plumbing";

    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--count") && i + 1 < argc) count = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ring-lsa") && i + 1 < argc) ring_lsa = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ring-size") && i + 1 < argc) ring_size = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ring-ea") && i + 1 < argc) ring_ea = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--out-prefix") && i + 1 < argc) out_prefix = argv[++i];
    }

    static uint8_t ls_in[PLUMB_LS_BYTES];
    static uint8_t gpr_in[128 * 16];
    static uint8_t spu_ls[PLUMB_LS_BYTES];
    static uint8_t gpu_ls[PLUMB_LS_BYTES];
    char path[1200];
    snprintf(path, sizeof path, "%s/ls.bin", dir);  load_exact(path, ls_in, sizeof ls_in);
    snprintf(path, sizeof path, "%s/gpr.bin", dir); load_exact(path, gpr_in, sizeof gpr_in);

    uint32_t entry_r3 = 0, out_base = 0;
    run_spu_2a(ls_in, gpr_in, spu_ls, &entry_r3, &out_base);
    const uint32_t end_cursor = be32(spu_ls + ((entry_r3 + 0xD0u) & SPU_LS_MASK));
    if (!count) {
        const uint32_t delta = end_cursor - out_base;
        count = (delta && delta % PLUMB_REC_SIZE == 0 && delta / PLUMB_REC_SIZE <= 4096)
                    ? delta / PLUMB_REC_SIZE : PLUMB_COUNT_DEFAULT;
    }
    if (!count || count > 4096) { fprintf(stderr, "bad count %u\n", count); return 1; }
    uint32_t tab[4];
    for (int i = 0; i < 4; i++)
        tab[i] = be32(ls_in + ((entry_r3 + 0x260u + (uint32_t)i * 4u) & SPU_LS_MASK));

    const uint32_t arr_bytes = count * 16u;
    const uint32_t nrm_off = 0;
    const uint32_t pos_off = arr_bytes;
    const uint32_t col_off = 2u * arr_bytes;
    const uint32_t uv_off  = 3u * arr_bytes;
    const uint32_t out_off = 4u * arr_bytes;
    const uint32_t buf_bytes = (out_off + count * PLUMB_REC_SIZE + 3u) & ~3u;

    uint8_t* export_in = (uint8_t*)calloc(1, buf_bytes);
    uint8_t* export_out = (uint8_t*)calloc(1, buf_bytes);
    if (!export_in || !export_out) { fprintf(stderr, "oom\n"); return 1; }
    memcpy(export_in + nrm_off, ls_in + tab[0], arr_bytes);
    memcpy(export_in + pos_off, ls_in + tab[1], arr_bytes);
    memcpy(export_in + col_off, ls_in + tab[2], arr_bytes);
    memcpy(export_in + uv_off,  ls_in + tab[3], arr_bytes);
    memcpy(export_out, export_in, buf_bytes);

    /* Compile GLSL -> SPIR-V. */
    char compile_cmd[4096];
    snprintf(compile_cmd, sizeof compile_cmd,
             "glslangValidator -V --target-env vulkan1.0 -o \"%s\" \"%s\"", spv_path, comp_path);
    int rc = system(compile_cmd);
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) { fprintf(stderr, "shader compile failed\n"); return 1; }
    size_t spv_words = 0;
    uint32_t* spv = read_file_u32(spv_path, &spv_words);

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "c690-2a-plumbing", .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t phys_count = 0; CHECK(vkEnumeratePhysicalDevices(inst, &phys_count, NULL));
    if (!phys_count) { fprintf(stderr, "no Vulkan devices\n"); return 1; }
    VkPhysicalDevice* phys_list = (VkPhysicalDevice*)malloc(phys_count * sizeof(*phys_list));
    CHECK(vkEnumeratePhysicalDevices(inst, &phys_count, phys_list));
    VkPhysicalDevice phys = VK_NULL_HANDLE; uint32_t qfamily = UINT32_MAX;
    for (uint32_t d = 0; d < phys_count && phys == VK_NULL_HANDLE; d++) {
        uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys_list[d], &qn, NULL);
        VkQueueFamilyProperties* qp = (VkQueueFamilyProperties*)malloc(qn * sizeof(*qp));
        vkGetPhysicalDeviceQueueFamilyProperties(phys_list[d], &qn, qp);
        for (uint32_t q = 0; q < qn; q++)
            if (qp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) { phys = phys_list[d]; qfamily = q; break; }
        free(qp);
    }
    if (phys == VK_NULL_HANDLE) { fprintf(stderr, "no compute queue\n"); return 1; }
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys, &props);
    printf("Device: %s, compute queue family %u\n", props.deviceName, qfamily);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfamily, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev; CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, qfamily, 0, &queue);

    VkBuffer in_buf, out_buf; VkDeviceMemory in_mem, out_mem; void *in_map, *out_map;
    make_buffer(dev, phys, buf_bytes, &in_buf, &in_mem, &in_map);
    make_buffer(dev, phys, buf_bytes, &out_buf, &out_mem, &out_map);
    memcpy(in_map, export_in, buf_bytes);
    memcpy(out_map, export_in, buf_bytes);

    VkDescriptorSetLayoutBinding bind[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bind };
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

    PlumbPush pc = { .uv_base = uv_off, .col_base = col_off, .pos_base = pos_off,
                     .nrm_base = nrm_off, .out_base = out_off, .count = count };
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(pc) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spv_words * 4, .pCode = spv };
    VkShaderModule module; CHECK(vkCreateShaderModule(dev, &smci, NULL, &module));
    VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" },
        .layout = layout };
    VkPipeline pipe; CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
    VkDescriptorPoolSize dps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet dset; CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));
    VkDescriptorBufferInfo dbi[2] = {
        { .buffer = in_buf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = out_buf, .offset = 0, .range = VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wds[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[1] } };
    vkUpdateDescriptorSets(dev, 2, wds, 0, NULL);

    VkCommandPoolCreateInfo cpci2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfamily };
    VkCommandPool cmd_pool; CHECK(vkCreateCommandPool(dev, &cpci2, NULL, &cmd_pool));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    const uint32_t groups = (count + 63u) / 64u;

    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, groups, 1, 1);
    CHECK(vkEndCommandBuffer(cmd));
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    CHECK(vkResetFences(dev, 1, &fence));

    const uint8_t* gpu_records = (const uint8_t*)out_map + out_off;
    const uint8_t* spu_records = spu_ls + out_base;
    const size_t rec_bytes = (size_t)count * PLUMB_REC_SIZE;
    size_t rec_diff = 0, rec_first = (size_t)-1;
    for (size_t i = 0; i < rec_bytes; i++) {
        if (gpu_records[i] != spu_records[i]) {
            if (rec_first == (size_t)-1) rec_first = i;
            rec_diff++;
        }
    }
    size_t outside = 0;
    for (uint32_t i = 0; i < buf_bytes; i++) {
        if (i >= out_off && i < out_off + rec_bytes) continue;
        if (((const uint8_t*)out_map)[i] != export_in[i]) outside++;
    }

    /* The ring copy: LS [ring_lsa, ring_lsa+ring_size) -> guest EA ring_ea. */
    memcpy(gpu_ls, ls_in, PLUMB_LS_BYTES);
    memcpy(gpu_ls + out_base, gpu_records, rec_bytes);
    const uint8_t* spu_ring = spu_ls + ring_lsa;
    const uint8_t* gpu_ring = gpu_ls + ring_lsa;
    size_t ring_diff = 0, ring_first = (size_t)-1;
    for (uint32_t i = 0; i < ring_size; i++) {
        if (spu_ring[i] != gpu_ring[i]) {
            if (ring_first == (size_t)-1) ring_first = i;
            ring_diff++;
        }
    }
    const int in_ring = (out_base >= ring_lsa && (uint64_t)out_base + rec_bytes <= (uint64_t)ring_lsa + ring_size);

    char p[1200];
    snprintf(p, sizeof p, "%s_spu_ring.bin", out_prefix); write_file(p, spu_ring, ring_size);
    snprintf(p, sizeof p, "%s_gpu_ring.bin", out_prefix); write_file(p, gpu_ring, ring_size);
    snprintf(p, sizeof p, "%s_spu_records.bin", out_prefix); write_file(p, spu_records, rec_bytes);
    snprintf(p, sizeof p, "%s_gpu_records.bin", out_prefix); write_file(p, gpu_records, rec_bytes);

    printf("c690 0x2A plumbing: SPU skip + exported inputs -> GPU -> ring bytes\n");
    printf("Capture: %s\n", dir);
    printf("entry r3=0x%05X out_base=0x%05X end_cursor=0x%05X count=%u\n",
           entry_r3, out_base, end_cursor, count);
    printf("source table: nrm=0x%05X pos=0x%05X col=0x%05X uv=0x%05X\n",
           tab[0], tab[1], tab[2], tab[3]);
    printf("export: %u bytes (%u arrays + %zu output), buffer %u bytes\n",
           buf_bytes, 4u * arr_bytes, rec_bytes, buf_bytes);
    printf("GPU vs SPU records: %zu differing bytes", rec_diff);
    if (rec_first != (size_t)-1) printf(" (first +0x%zX)", rec_first);
    printf("\n");
    printf("GPU writes outside output: %zu byte(s)\n", outside);
    printf("ring mapping: LS 0x%05X..0x%05X -> EA 0x%08X (%u bytes, output inside=%s)\n",
           ring_lsa, ring_lsa + ring_size, ring_ea, ring_size, in_ring ? "yes" : "no");
    printf("ring bytes: %zu differing", ring_diff);
    if (ring_first != (size_t)-1) printf(" (first +0x%zX)", ring_first);
    printf("\n");
    const int pass = (rec_diff == 0 && outside == 0 && ring_diff == 0 && in_ring);
    printf("RESULT: %s\n", pass ? "PASS (bit-exact at the ring EA)" : "FAIL");

    vkUnmapMemory(dev, in_mem); vkUnmapMemory(dev, out_mem);
    vkDestroyFence(dev, fence, NULL); vkDestroyCommandPool(dev, cmd_pool, NULL);
    vkDestroyDescriptorPool(dev, pool, NULL); vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyShaderModule(dev, module, NULL); vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyBuffer(dev, in_buf, NULL); vkDestroyBuffer(dev, out_buf, NULL);
    vkFreeMemory(dev, in_mem, NULL); vkFreeMemory(dev, out_mem, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    free(spv); free(phys_list); free(export_in); free(export_out);
    return pass ? 0 : 1;
}

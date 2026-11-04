/*
 * Offline Vulkan compute cost-model harness for the c690 0x2A vertex
 * assembler, batching ALL jobs of a burst into ONE dispatch (phase 2b).
 *
 * For every job the batch kernel c690_2a_batch.comp reads one complete 256 KiB
 * Local Store image (big-endian 32-bit lanes) and writes 36-byte interleaved
 * vertex records.  The harness:
 *   - loads N captured 0x10F78 input directories (repeated/tiled to J jobs);
 *   - builds the reference output per unique capture with the lifted SPU
 *     module (used by test_c690_2a_shader.c), or from `ls_out.bin` if present;
 *   - uploads the J concatenated LS images once (host-visible coherent, timed);
 *   - records ONE command buffer containing ONE vkCmdDispatch for all J jobs,
 *     submits once and waits on a fence;
 *   - reports GPU time / us per job, command-buffer record CPU time, submit
 *     CPU time, total upload/init time, and byte-exactness of every job;
 *   - also measures J=1 and J=64 reference points for scaling.
 *
 * -------------------------------------------------------------------------
 * Build (research; needs the captured lifted module):
 *   clang -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
 *     -I /home/m/Documents/mlb16-online/ps3recomp-play/runtime/spu \
 *     -I /home/m/Documents/mlb16-online/ps3recomp-play/include \
 *     -I /home/m/Documents/mlb16-online/mlb16-recomp/tools/replay \
 *     -o /tmp/opencode/gpu2a/test_c690_2a_batch \
 *     /home/m/Documents/mlb16-online/mlb16-recomp/tools/replay/test_c690_2a_batch.c \
 *     /tmp/opencode/c690gpu/reg_c690.c \
 *     /home/m/Documents/mlb16-online/recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c \
 *     /home/m/Documents/mlb16-online/ps3recomp-play/runtime/spu/job_c690_native.c \
 *     -l:libvulkan.so.1 -lm
 *
 * Run (default: one capture tiled to J=429, host-visible storage):
 *   test_c690_2a_batch \
 *     /home/m/Documents/mlb16-online/mlb16-recomp/tools/replay/c690_2a_batch.comp \
 *     /tmp/opencode/gpu2a/c690_2a_batch.spv \
 *     --jobs 429 --iters 9 \
 *     /tmp/opencode/fncap21b/0x10F78
 *
 * Compute-only scaling (device-local storage + host-visible staging upload,
 * read back after the timed interval):
 *   test_c690_2a_batch \
 *     /home/m/Documents/mlb16-online/mlb16-recomp/tools/replay/c690_2a_batch.comp \
 *     /tmp/opencode/gpu2a/c690_2a_batch.spv \
 *     --jobs 429 --iters 9 --device-local \
 *     /tmp/opencode/fncap21b/0x10F78
 *
 * Multiple captures are tiled in order: job j uses dirs[j % ndirs].
 * The default J=429 with one dir replicates that capture 429 times.
 * `ls_out.bin` in a capture dir (if present) is used as the reference output;
 * otherwise the lifted SPU module is run once per unique capture.
 * Env C690_2A_COUNT overrides the per-job vertex count (default 193).
 *
 * Measurement notes:
 *   - Default storage is host-visible coherent (as requested): the GPU reads
 *     and writes system memory, so the burst is latency/PCIe bound and the
 *     per-job time is much higher than the kernel's device-local cost.
 *   - `--device-local` keeps the J LS images in VRAM; upload/readback are
 *     excluded from the GPU interval.  Use this for the compute cost model.
 *   - Timings are the median of --iters runs; recording and submit CPU times
 *     are measured separately around vkEndCommandBuffer and vkQueueSubmit.
 * -------------------------------------------------------------------------
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

#define LS_BYTES   262144u
#define OUT_BASE   0x2F870u
#define REC_SIZE   36u
#define DEFAULT_COUNT 193u
#define BASE_WORD  0x1F260u   /* u32 LE words: nrm, pos, col, uv0 */
#define DEFAULT_JOBS 429u

typedef struct { uint32_t uv_base, col_base, pos_base, nrm_base, out_base, count, pad0, pad1; } Job;
typedef struct { uint32_t job_count, ls_words, max_count, pad0; } PushConstants;

typedef struct {
    const char* dir;
    uint8_t ls[LS_BYTES];
    uint8_t ref_out[REC_SIZE * 4096]; /* room for any realistic count */
    uint32_t count;
} Capture;

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}
static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}
static double median(double* v, int n) {
    qsort(v, (size_t)n, sizeof(double), cmp_double);
    return n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}
static void load_exact(const char* path, void* dst, size_t want) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    if (fread(dst, 1, want, f) != want) { fprintf(stderr, "%s: short read\n", path); exit(1); }
    fclose(f);
}
static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
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
static void make_buffer_props(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize size,
                              VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                              VkBuffer* buf, VkDeviceMemory* mem, void** mapped) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                               .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    CHECK(vkCreateBuffer(dev, &bci, NULL, buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, *buf, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_memory_type(phys, req.memoryTypeBits, props) };
    CHECK(vkAllocateMemory(dev, &mai, NULL, mem));
    CHECK(vkBindBufferMemory(dev, *buf, *mem, 0));
    if (mapped) CHECK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, mapped));
}
static void make_buffer(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize size,
                        VkBuffer* buf, VkDeviceMemory* mem, void** mapped) {
    make_buffer_props(dev, phys, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      buf, mem, mapped);
}

/* Reference output for one capture: `ls_out.bin` if present, else lifted SPU. */
static void run_reference(Capture* cap, uint32_t count) {
    char path[1200];
    snprintf(path, sizeof path, "%s/ls_out.bin", cap->dir);
    if (file_exists(path)) {
        uint8_t* tmp = (uint8_t*)malloc(LS_BYTES);
        if (!tmp) exit(1);
        load_exact(path, tmp, LS_BYTES);
        memcpy(cap->ref_out, tmp + OUT_BASE, (size_t)count * REC_SIZE);
        free(tmp);
        printf("  reference[%s]: ls_out.bin\n", cap->dir);
        return;
    }

    static spu_context c_ref;
    spu_context_init(&c_ref, 0);
    uint8_t gpr0[128 * 16];
    snprintf(path, sizeof path, "%s/gpr.bin", cap->dir);
    load_exact(path, gpr0, sizeof gpr0);
    memcpy(c_ref.ls, cap->ls, LS_BYTES);
    memcpy(c_ref.gpr, gpr0, sizeof gpr0);
    c_ref.image_id = 6; c_ref.pc = 0x10F78; c_ref.status = 0; c_ref.stop_code = 0;
    g_spu_trampoline_fn = NULL; g_terminal = 0;
    spu_fn f = lookup_fn(0x10F78);
    if (!f) { fprintf(stderr, "no lifted 0x10F78\n"); exit(1); }
    f(&c_ref);
    while (!g_terminal && g_spu_trampoline_fn) {
        void (*tf)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = NULL; tf(&c_ref);
    }
    memcpy(cap->ref_out, c_ref.ls + OUT_BASE, (size_t)count * REC_SIZE);
    printf("  reference[%s]: lifted SPU 0x10F78\n", cap->dir);
}

typedef struct {
    uint32_t J;
    double gpu_ms, us_per_job, record_us, submit_us;
    size_t diff_bytes, outside_bytes, bad_jobs;
    size_t first_job, first_byte;
    int pass;
} Result;

static Result run_J(VkDevice dev, VkQueue queue, VkCommandBuffer cmd, VkFence fence,
                    VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet dset,
                    VkBuffer out_buf,
                    const Capture* caps, int ndirs, uint32_t J, uint32_t max_count,
                    uint32_t ls_words, int iters,
                    const void* in_map, const void* verify_map,
                    VkBuffer staging_out, VkDeviceSize out_bytes, int readback) {
    const uint32_t groups = (max_count + 63u) / 64u;
    PushConstants pc = { .job_count = J, .ls_words = ls_words, .max_count = max_count, .pad0 = 0 };
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    Result r; memset(&r, 0, sizeof r); r.J = J;

    /* Warm up this dispatch size (pipeline/descriptor caching). */
    CHECK(vkResetCommandBuffer(cmd, 0));
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, groups, J, 1);
    CHECK(vkEndCommandBuffer(cmd));
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    CHECK(vkResetFences(dev, 1, &fence));

    double* gpu = (double*)malloc((size_t)iters * sizeof(double));
    double* rec = (double*)malloc((size_t)iters * sizeof(double));
    double* sub = (double*)malloc((size_t)iters * sizeof(double));
    if (!gpu || !rec || !sub) exit(1);

    for (int it = 0; it < iters; it++) {
        CHECK(vkResetCommandBuffer(cmd, 0));
        double t0 = now_sec();
        CHECK(vkBeginCommandBuffer(cmd, &cbbi));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0, NULL);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, groups, J, 1);
        CHECK(vkEndCommandBuffer(cmd));
        double t1 = now_sec();
        double t2 = now_sec();
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        double t3 = now_sec();
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        double t4 = now_sec();
        CHECK(vkResetFences(dev, 1, &fence));
        rec[it] = (t1 - t0) * 1e6;
        sub[it] = (t3 - t2) * 1e6;
        gpu[it] = (t4 - t2) * 1e3;
    }
    r.gpu_ms = median(gpu, iters);
    r.record_us = median(rec, iters);
    r.submit_us = median(sub, iters);
    r.us_per_job = r.gpu_ms * 1e3 / (double)J;
    free(gpu); free(rec); free(sub);

    /* Optional device-local readback (outside the timed GPU interval). */
    if (readback) {
        CHECK(vkResetCommandBuffer(cmd, 0));
        CHECK(vkBeginCommandBuffer(cmd, &cbbi));
        VkBufferCopy bc = { .srcOffset = 0, .dstOffset = 0, .size = out_bytes };
        vkCmdCopyBuffer(cmd, out_buf, staging_out, 1, &bc);
        CHECK(vkEndCommandBuffer(cmd));
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(dev, 1, &fence));
    }

    /* Verify every job byte-for-byte against its reference, and check that
     * nothing outside the output region was touched. */
    const uint8_t* out_host = (const uint8_t*)verify_map;
    for (uint32_t j = 0; j < J; j++) {
        const Capture* cap = &caps[j % (uint32_t)ndirs];
        const uint8_t* got = out_host + (size_t)j * LS_BYTES + OUT_BASE;
        const uint8_t* want = cap->ref_out;
        size_t nb = (size_t)cap->count * REC_SIZE;
        int bad = 0;
        for (size_t b = 0; b < nb; b++) {
            if (got[b] != want[b]) {
                if (r.bad_jobs == 0 && r.diff_bytes == 0) { r.first_job = j; r.first_byte = b; }
                r.diff_bytes++; bad = 1;
            }
        }
        if (bad) r.bad_jobs++;
        const uint8_t* img = out_host + (size_t)j * LS_BYTES;
        const uint8_t* src = (const uint8_t*)in_map + (size_t)j * LS_BYTES;
        for (uint32_t b = 0; b < LS_BYTES; b++) {
            if (b >= OUT_BASE && b < OUT_BASE + nb) continue;
            if (img[b] != src[b]) r.outside_bytes++;
        }
    }
    r.pass = (r.diff_bytes == 0 && r.outside_bytes == 0 && r.bad_jobs == 0);
    return r;
}

int main(int argc, char** argv) {
    const char* comp_path = "c690_2a_batch.comp";
    const char* spv_path = "/tmp/opencode/gpu2a/c690_2a_batch.spv";
    uint32_t wanted_J = DEFAULT_JOBS;
    uint32_t count = DEFAULT_COUNT;
    int iters = 9;
    int device_local = 0;
    const char* dirs[64]; int ndirs = 0;

    const char* env_count = getenv("C690_2A_COUNT");
    if (env_count && *env_count) count = (uint32_t)strtoul(env_count, NULL, 0);
    if (count == 0 || count > 4096) { fprintf(stderr, "bad count %u\n", count); return 1; }

    int comp_seen = 0, spv_seen = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--jobs") && i + 1 < argc) {
            wanted_J = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
            iters = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--device-local")) {
            device_local = 1;
        } else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--jobs J] [--iters N] [--device-local] [comp.spv out.spv] <dir>...\n", argv[0]);
            return 0;
        } else if (!comp_seen && strstr(argv[i], ".comp")) {
            comp_path = argv[i]; comp_seen = 1;
        } else if (!spv_seen && strstr(argv[i], ".spv")) {
            spv_path = argv[i]; spv_seen = 1;
        } else {
            if (ndirs < 64) dirs[ndirs++] = argv[i];
        }
    }
    if (ndirs == 0) dirs[ndirs++] = "/tmp/opencode/fncap21b/0x10F78";
    if (iters < 1) iters = 1;
    if (wanted_J < 1) wanted_J = 1;

    /* Reference points J=1, J=64, plus the requested burst, deduplicated. */
    uint32_t Js[8]; int nJs = 0;
    uint32_t base_J[3] = { 1u, 64u, wanted_J };
    for (int i = 0; i < 3; i++) {
        int dup = 0;
        for (int k = 0; k < nJs; k++) if (Js[k] == base_J[i]) dup = 1;
        if (!dup) Js[nJs++] = base_J[i];
    }
    uint32_t maxJ = 0;
    for (int i = 0; i < nJs; i++) if (Js[i] > maxJ) maxJ = Js[i];

    /* --- Load captures --- */
    Capture* caps = (Capture*)calloc((size_t)ndirs, sizeof(Capture));
    if (!caps) exit(1);
    printf("c690 0x2A batch assembler: Vulkan compute vs lifted SPU\n");
    printf("Captures: %d, per-job count=%u, J max=%u, iters=%d\n", ndirs, count, maxJ, iters);
    for (int i = 0; i < ndirs; i++) {
        char path[1200];
        caps[i].dir = dirs[i];
        caps[i].count = count;
        snprintf(path, sizeof path, "%s/ls.bin", dirs[i]);
        load_exact(path, caps[i].ls, LS_BYTES);
        run_reference(&caps[i], count);
    }

    /* --- Compile GLSL -> SPIR-V --- */
    char compile_cmd[4096];
    snprintf(compile_cmd, sizeof compile_cmd,
             "glslangValidator -V --target-env vulkan1.0 -o \"%s\" \"%s\"", spv_path, comp_path);
    int rc = system(compile_cmd);
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) { fprintf(stderr, "shader compile failed\n"); return 1; }
    size_t spv_words = 0;
    uint32_t* spv = read_file_u32(spv_path, &spv_words);

    /* --- Vulkan setup --- */
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "c690-2a-batch-test", .apiVersion = VK_API_VERSION_1_0 };
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
    printf("Device: %s, compute queue family %u, storage=%s\n", props.deviceName, qfamily,
           device_local ? "device-local" : "host-visible-coherent");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfamily, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev; CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, qfamily, 0, &queue);

    VkDeviceSize in_bytes = (VkDeviceSize)maxJ * LS_BYTES;
    VkDeviceSize job_bytes = (VkDeviceSize)maxJ * sizeof(Job);
    VkBuffer in_buf, out_buf, job_buf;
    VkDeviceMemory in_mem, out_mem, job_mem;
    void *in_map, *out_map, *job_map;
    /* Host-visible staging (always); doubles as the storage buffer unless
     * --device-local asks for device-local storage + copy upload/readback. */
    VkBuffer host_in, host_out;
    VkDeviceMemory host_in_mem, host_out_mem;
    void *host_in_map, *host_out_map;
    VkBufferUsageFlags stage_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    make_buffer_props(dev, phys, in_bytes, stage_usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &host_in, &host_in_mem, &host_in_map);
    make_buffer_props(dev, phys, in_bytes, stage_usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &host_out, &host_out_mem, &host_out_map);
    make_buffer(dev, phys, job_bytes, &job_buf, &job_mem, &job_map);
    if (device_local) {
        VkBufferUsageFlags dev_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        make_buffer_props(dev, phys, in_bytes, dev_usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          &in_buf, &in_mem, NULL);
        make_buffer_props(dev, phys, in_bytes, dev_usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          &out_buf, &out_mem, NULL);
        in_map = host_in_map; out_map = host_out_map;
    } else {
        in_buf = host_in; in_mem = host_in_mem; in_map = host_in_map;
        out_buf = host_out; out_mem = host_out_mem; out_map = host_out_map;
    }

    VkDescriptorSetLayoutBinding bind[3] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bind };
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(PushConstants) };
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
    VkDescriptorPoolSize dps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet dset; CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));
    VkDescriptorBufferInfo dbi[3] = {
        { .buffer = in_buf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = out_buf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = job_buf, .offset = 0, .range = VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wds[3] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 2,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[2] } };
    vkUpdateDescriptorSets(dev, 3, wds, 0, NULL);

    VkCommandPoolCreateInfo cpci2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfamily };
    VkCommandPool cmd_pool; CHECK(vkCreateCommandPool(dev, &cpci2, NULL, &cmd_pool));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; CHECK(vkCreateFence(dev, &fci, NULL, &fence));

    /* --- Upload once: J LS images + job table + output init (timed) --- */
    double u0 = now_sec();
    Job* jobs = (Job*)job_map;
    for (uint32_t j = 0; j < maxJ; j++) {
        const Capture* cap = &caps[j % (uint32_t)ndirs];
        memcpy((uint8_t*)host_in_map + (size_t)j * LS_BYTES, cap->ls, LS_BYTES);
        /* Output starts as a copy of the input so untouched LS is checked too. */
        memcpy((uint8_t*)host_out_map + (size_t)j * LS_BYTES, cap->ls, LS_BYTES);
        /* LS 32-bit lanes are big-endian: swap the stored base words. */
        const uint32_t* bw = (const uint32_t*)(cap->ls + BASE_WORD);
        jobs[j].nrm_base = bswap32(bw[0]);
        jobs[j].pos_base = bswap32(bw[1]);
        jobs[j].col_base = bswap32(bw[2]);
        jobs[j].uv_base  = bswap32(bw[3]);
        jobs[j].out_base = OUT_BASE;
        jobs[j].count    = count;
        jobs[j].pad0 = jobs[j].pad1 = 0;
    }
    if (device_local) {
        VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        CHECK(vkBeginCommandBuffer(cmd, &cbbi));
        VkBufferCopy ci = { .srcOffset = 0, .dstOffset = 0, .size = in_bytes };
        VkBufferCopy co = { .srcOffset = 0, .dstOffset = 0, .size = in_bytes };
        vkCmdCopyBuffer(cmd, host_in, in_buf, 1, &ci);
        vkCmdCopyBuffer(cmd, host_out, out_buf, 1, &co);
        CHECK(vkEndCommandBuffer(cmd));
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(dev, 1, &fence));
    }
    double upload_ms = (now_sec() - u0) * 1e3;

    uint32_t ls_words = LS_BYTES / 4u;
    printf("Input image: %u bytes/job (%u words), max count %u -> %u workgroups/job\n",
           LS_BYTES, ls_words, count, (count + 63u) / 64u);
    printf("Upload/init of %u images (%.1f MiB in + %.1f MiB out + job table): %.3f ms\n",
           maxJ, in_bytes / 1048576.0, in_bytes / 1048576.0, upload_ms);
    printf("\n%-6s %12s %12s %12s %12s %10s  %s\n",
           "J", "GPU ms", "GPU us/job", "record us", "submit us", "exact", "detail");

    Result results[8]; int nr = 0;
    for (int i = 0; i < nJs; i++) {
        Result r = run_J(dev, queue, cmd, fence, pipe, layout, dset,
                         out_buf, caps, ndirs, Js[i], count, ls_words,
                         iters, in_map, out_map, host_out, in_bytes, device_local);
        results[nr++] = r;
        printf("%-6u %12.3f %12.3f %12.2f %12.2f %10s", Js[i], r.gpu_ms, r.us_per_job,
               r.record_us, r.submit_us, r.pass ? "YES" : "NO");
        if (!r.pass) {
            printf("   diff=%zu bytes over %zu/%u jobs", r.diff_bytes, r.bad_jobs, Js[i]);
            if (r.diff_bytes) printf("; first job %zu rec+0x%zX", r.first_job, r.first_byte);
            if (r.outside_bytes) printf("; outside=%zuB", r.outside_bytes);
        }
        printf("\n");
    }

    int all_pass = 1, burst_pass = 0;
    for (int i = 0; i < nr; i++) {
        if (!results[i].pass) all_pass = 0;
        if (results[i].J == wanted_J) burst_pass = results[i].pass;
    }
    printf("\nRESULT: %s (burst J=%u %s)\n", all_pass ? "PASS (bit-exact)" : "FAIL",
           wanted_J, burst_pass ? "bit-exact" : "MISMATCH");
    printf("SPIR-V: %s\n", spv_path);

    vkUnmapMemory(dev, job_mem);
    vkDestroyFence(dev, fence, NULL); vkDestroyCommandPool(dev, cmd_pool, NULL);
    vkDestroyDescriptorPool(dev, pool, NULL); vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyShaderModule(dev, module, NULL); vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    if (device_local) {
        vkDestroyBuffer(dev, in_buf, NULL); vkDestroyBuffer(dev, out_buf, NULL);
        vkFreeMemory(dev, in_mem, NULL); vkFreeMemory(dev, out_mem, NULL);
    }
    vkUnmapMemory(dev, host_in_mem); vkUnmapMemory(dev, host_out_mem);
    vkDestroyBuffer(dev, host_in, NULL); vkDestroyBuffer(dev, host_out, NULL);
    vkDestroyBuffer(dev, job_buf, NULL);
    vkFreeMemory(dev, host_in_mem, NULL); vkFreeMemory(dev, host_out_mem, NULL); vkFreeMemory(dev, job_mem, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    free(spv); free(phys_list); free(caps);
    return all_pass ? 0 : 1;
}

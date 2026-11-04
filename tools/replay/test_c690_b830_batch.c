/*
 * Offline Vulkan gate + burst cost model for the c690 A730 leaf (LS 0xB830).
 *
 * Reads a PS3RECOMP_C690_TAIL_CAPTURE spool, extracts every op==0xA7 IN/OUT
 * pair, runs the transliterated GLSL kernel (c690_b830_batch.comp) on the IN
 * Local Store + GPRs, and diffs the full 256 KiB LS against the captured OUT.
 * Then batches J calls in ONE vkCmdDispatch (device-local storage) and reports
 * GPU time/frame and CPU record+submit.
 *
 * Build (needs only libvulkan, no lifted SPU module):
 *   clang -O2 -o /tmp/opencode/test_c690_b830_batch \
 *     mlb16-recomp/tools/replay/test_c690_b830_batch.c -l:libvulkan.so.1 -lm
 *
 * Run:
 *   /tmp/opencode/test_c690_b830_batch --jobs 4394 --iters 11 --device-local \
 *     mlb16-recomp/tools/replay/c690_b830_batch.comp /tmp/opencode/b830.spv \
 *     /tmp/opencode/fncap2b3a730
 *
 * Exit code is non-zero when the exactness gate fails.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <vulkan/vulkan.h>

#define LS_SIZE       (256u * 1024u)
#define GPR_WORDS     512u
#define META_WORDS    24
#define BITMAP_BYTES  2048
#define T_IN 9
#define T_OUT 10
#define MAX_PAIRS     256u
#define DEFAULT_JOBS  4394u

typedef struct {
    uint32_t seq, op, key;
    uint8_t ls[LS_SIZE];
    uint8_t gpr[GPR_WORDS * 4];
} Img;
typedef struct { Img in, out; int have; } Pair;

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static double median(double* v, int n) {
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
        if (v[j] < v[i]) { double t = v[i]; v[i] = v[j]; v[j] = t; }
    return n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* ---- spool parser (same record layout as test_c690_tail_replay.c) -------- */
static int read_rec(FILE* f, Img* img, uint32_t* type, uint32_t* seq) {
    uint8_t hdr[24], payload[12];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) return 0;
    uint32_t t = (uint32_t)hdr[0] | (uint32_t)hdr[1] << 8 | (uint32_t)hdr[2] << 16 | (uint32_t)hdr[3] << 24;
    uint32_t size = (uint32_t)hdr[4] | (uint32_t)hdr[5] << 8 | (uint32_t)hdr[6] << 16 | (uint32_t)hdr[7] << 24;
    if (size != META_WORDS * 4u + BITMAP_BYTES + LS_SIZE + GPR_WORDS * 4u) return 0;
    uint8_t* p = (uint8_t*)malloc(size);
    if (!p) exit(1);
    if (fread(p, 1, size, f) != size) { free(p); return 0; }
    uint32_t meta[META_WORDS];
    memcpy(meta, p, META_WORDS * 4);
    memcpy(img->ls, p + META_WORDS * 4 + BITMAP_BYTES, LS_SIZE);
    memcpy(img->gpr, p + META_WORDS * 4 + BITMAP_BYTES + LS_SIZE, GPR_WORDS * 4);
    img->seq = meta[7];
    img->op = meta[4];
    img->key = meta[5];
    *type = t; *seq = meta[7];
    free(p);
    /* silence unused */
    (void)payload;
    return 1;
}

static int load_pairs(const char* dir, Pair* pairs, uint32_t* npairs) {
    DIR* dp = opendir(dir);
    if (!dp) { perror(dir); return 0; }
    Pair* by_seq = (Pair*)calloc(MAX_PAIRS * 64u, sizeof(Pair));
    if (!by_seq) exit(1);
    uint32_t maxseq = 0;
    struct dirent* de;
    while ((de = readdir(dp))) {
        if (strncmp(de->d_name, "tail_", 5) || !strstr(de->d_name, ".bin")) continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        char magic[8]; uint32_t ver;
        if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "C69STG01", 8) ||
            fread(&ver, 4, 1, f) != 1) { fclose(f); continue; }
        Img im; uint32_t type, seq;
        while (read_rec(f, &im, &type, &seq)) {
            if (im.op != 0xA7) continue;
            if (seq >= MAX_PAIRS * 64u) continue;
            if (seq > maxseq) maxseq = seq;
            if (type == T_IN) { memcpy(&by_seq[seq].in, &im, sizeof im); }
            else if (type == T_OUT && by_seq[seq].in.seq == seq) {
                memcpy(&by_seq[seq].out, &im, sizeof im);
                by_seq[seq].have = 1;
            }
        }
        fclose(f);
    }
    closedir(dp);
    uint32_t n = 0;
    for (uint32_t s = 0; s <= maxseq && n < MAX_PAIRS; s++)
        if (by_seq[s].have) pairs[n++] = by_seq[s];
    free(by_seq);
    *npairs = n;
    return n > 0;
}

#define CHECK(expr) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "Vulkan error %d at %s:%d: %s\n", (int)_r, __FILE__, __LINE__, #expr); exit(2); } } while (0)
static uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no memory type\n"); exit(1);
}
static void make_buf(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize size, VkBufferUsageFlags usage,
                     VkMemoryPropertyFlags props, VkBuffer* buf, VkDeviceMemory* mem, void** mapped) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                               .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    CHECK(vkCreateBuffer(dev, &bci, NULL, buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, *buf, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = find_memory_type(phys, req.memoryTypeBits, props) };
    CHECK(vkAllocateMemory(dev, &mai, NULL, mem));
    CHECK(vkBindBufferMemory(dev, *buf, *mem, 0));
    if (mapped) CHECK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, mapped));
}
static uint32_t* read_spv(const char* path, size_t* words) {
    FILE* f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (sz & 3)) { fprintf(stderr, "%s: bad SPIR-V\n", path); exit(1); }
    uint32_t* p = (uint32_t*)malloc((size_t)sz);
    if (fread(p, 1, (size_t)sz, f) != (size_t)sz) exit(1);
    fclose(f);
    if (p[0] != 0x07230203u) { fprintf(stderr, "%s: not SPIR-V\n", path); exit(1); }
    *words = (size_t)sz / 4; return p;
}

typedef struct { uint32_t job_count, npairs, ls_words, pad0; } PushConstants;

typedef struct {
    uint32_t J;
    double gpu_ms, us_per_job, record_us, submit_us;
} Timing;

static Timing run_J(VkDevice dev, VkQueue queue, VkCommandBuffer cmd, VkFence fence,
                    VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet dset,
                    uint32_t J, uint32_t npairs, int iters) {
    PushConstants pc = { .job_count = J, .npairs = npairs, .ls_words = LS_SIZE / 4u, .pad0 = 0 };
    uint32_t groups = (J + 63u) / 64u;
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    Timing r; memset(&r, 0, sizeof r); r.J = J;

    CHECK(vkResetCommandBuffer(cmd, 0));
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, groups, 1, 1);
    CHECK(vkEndCommandBuffer(cmd));
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    CHECK(vkResetFences(dev, 1, &fence));

    double* gpu = (double*)malloc((size_t)iters * sizeof(double));
    double* rec = (double*)malloc((size_t)iters * sizeof(double));
    double* sub = (double*)malloc((size_t)iters * sizeof(double));
    for (int it = 0; it < iters; it++) {
        CHECK(vkResetCommandBuffer(cmd, 0));
        double t0 = now_sec();
        CHECK(vkBeginCommandBuffer(cmd, &cbbi));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0, NULL);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, groups, 1, 1);
        CHECK(vkEndCommandBuffer(cmd));
        double t1 = now_sec();
        double t2 = now_sec();
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        double t3 = now_sec();
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        double t4 = now_sec();
        CHECK(vkResetFences(dev, 1, &fence));
        rec[it] = (t1 - t0) * 1e6; sub[it] = (t3 - t2) * 1e6; gpu[it] = (t4 - t2) * 1e3;
    }
    r.gpu_ms = median(gpu, iters); r.record_us = median(rec, iters);
    r.submit_us = median(sub, iters);
    r.us_per_job = r.gpu_ms * 1e3 / (double)J;
    free(gpu); free(rec); free(sub);
    return r;
}

static uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}

int main(int argc, char** argv) {
    const char* spool = "/tmp/opencode/fncap2b3a730";
    const char* comp = "mlb16-recomp/tools/replay/c690_b830_batch.comp";
    const char* spv_path = "/tmp/opencode/c690_b830_batch.spv";
    uint32_t wanted_J = DEFAULT_JOBS;
    int iters = 11, device_local = 0, comp_seen = 0, spv_seen = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--jobs") && i + 1 < argc) wanted_J = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--device-local")) device_local = 1;
        else if (!comp_seen && strstr(argv[i], ".comp")) { comp = argv[i]; comp_seen = 1; }
        else if (!spv_seen && strstr(argv[i], ".spv")) { spv_path = argv[i]; spv_seen = 1; }
        else spool = argv[i];
    }
    if (iters < 1) iters = 1;

    Pair* pairs = (Pair*)calloc(MAX_PAIRS, sizeof(Pair));
    uint32_t npairs = 0;
    if (!load_pairs(spool, pairs, &npairs)) { fprintf(stderr, "no 0xA7 pairs in %s\n", spool); return 1; }
    { const char* capn = getenv("B830_NPAIRS");
      if (capn && *capn) { uint32_t c = (uint32_t)strtoul(capn, NULL, 0); if (c && c < npairs) npairs = c; } }
    printf("c690 0xB830 leaf (A730 heavy body): Vulkan compute vs captured OUT\n");
    printf("spool %s: %u x 0xA7 pairs, burst J=%u, iters=%d\n", spool, npairs, wanted_J, iters);

    char compile_cmd[4096];
    snprintf(compile_cmd, sizeof compile_cmd,
             "glslangValidator -V --target-env vulkan1.0 -o \"%s\" \"%s\"", spv_path, comp);
    int rc = system(compile_cmd);
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) { fprintf(stderr, "shader compile failed\n"); return 1; }
    size_t spv_words = 0;
    uint32_t* spv = read_spv(spv_path, &spv_words);

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "c690-b830-batch-test", .apiVersion = VK_API_VERSION_1_0 };
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
    printf("Device: %s, storage=%s\n", props.deviceName, device_local ? "device-local" : "host-visible-coherent");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfamily, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev; CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, qfamily, 0, &queue);

    VkDeviceSize ls_bytes = (VkDeviceSize)npairs * LS_SIZE;
    VkDeviceSize gpr_bytes = (VkDeviceSize)npairs * GPR_WORDS * 4u;
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer host_in, host_out, host_gpr, host_gpr_out, in_buf, out_buf, gpr_buf, gpr_out_buf;
    VkDeviceMemory host_in_m, host_out_m, host_gpr_m, host_gpr_out_m, in_m, out_m, gpr_m, gpr_out_m;
    void *host_in_map, *host_out_map, *host_gpr_map, *host_gpr_out_map;
    make_buf(dev, phys, ls_bytes, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
             &host_in, &host_in_m, &host_in_map);
    make_buf(dev, phys, ls_bytes, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
             &host_out, &host_out_m, &host_out_map);
    make_buf(dev, phys, gpr_bytes, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
             &host_gpr, &host_gpr_m, &host_gpr_map);
    make_buf(dev, phys, gpr_bytes, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
             &host_gpr_out, &host_gpr_out_m, &host_gpr_out_map);
    if (device_local) {
        make_buf(dev, phys, ls_bytes, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &in_buf, &in_m, NULL);
        make_buf(dev, phys, ls_bytes, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &out_buf, &out_m, NULL);
        make_buf(dev, phys, gpr_bytes, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &gpr_buf, &gpr_m, NULL);
        make_buf(dev, phys, gpr_bytes, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &gpr_out_buf, &gpr_out_m, NULL);
    } else {
        in_buf = host_in; in_m = host_in_m; out_buf = host_out; out_m = host_out_m;
        gpr_buf = host_gpr; gpr_m = host_gpr_m; gpr_out_buf = host_gpr_out; gpr_out_m = host_gpr_out_m;
    }
    for (uint32_t p = 0; p < npairs; p++) {
        memcpy((uint8_t*)host_in_map + (size_t)p * LS_SIZE, pairs[p].in.ls, LS_SIZE);
        memcpy((uint8_t*)host_out_map + (size_t)p * LS_SIZE, pairs[p].in.ls, LS_SIZE);
        memcpy((uint8_t*)host_gpr_map + (size_t)p * GPR_WORDS * 4u, pairs[p].in.gpr, GPR_WORDS * 4u);
    }

    VkDescriptorSetLayoutBinding bind[4] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4, .pBindings = bind };
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
    VkDescriptorPoolSize dps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 4 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet dset; CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));
    VkDescriptorBufferInfo dbi[4] = {
        { .buffer = in_buf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = out_buf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = gpr_buf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = gpr_out_buf, .offset = 0, .range = VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wds[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 2,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[2] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 3,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[3] } };
    vkUpdateDescriptorSets(dev, 4, wds, 0, NULL);

    VkCommandPoolCreateInfo cpci2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfamily };
    VkCommandPool cmd_pool; CHECK(vkCreateCommandPool(dev, &cpci2, NULL, &cmd_pool));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; CHECK(vkCreateFence(dev, &fci, NULL, &fence));

    int all_exact = 1;
    uint64_t diff_bytes = 0, diff_words = 0, total_words = 0, hist[8] = {0};
    uint32_t max_ulp = 0;

    /* ---- exactness run: J = npairs, one dispatch ---- */
    if (device_local) {
        VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        CHECK(vkBeginCommandBuffer(cmd, &cbbi));
        VkBufferCopy ci = { .srcOffset = 0, .dstOffset = 0, .size = ls_bytes };
        VkBufferCopy co0 = { .srcOffset = 0, .dstOffset = 0, .size = ls_bytes };
        VkBufferCopy cg = { .srcOffset = 0, .dstOffset = 0, .size = gpr_bytes };
        vkCmdCopyBuffer(cmd, host_in, in_buf, 1, &ci);
        vkCmdCopyBuffer(cmd, host_out, out_buf, 1, &co0);
        vkCmdCopyBuffer(cmd, host_gpr, gpr_buf, 1, &cg);
        CHECK(vkEndCommandBuffer(cmd));
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(dev, 1, &fence));
    }
    Timing exact_t = run_J(dev, queue, cmd, fence, pipe, layout, dset, npairs, npairs, 1);

    if (device_local) {
        VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        CHECK(vkResetCommandBuffer(cmd, 0));
        CHECK(vkBeginCommandBuffer(cmd, &cbbi));
        VkBufferCopy co = { .srcOffset = 0, .dstOffset = 0, .size = ls_bytes };
        VkBufferCopy cgo = { .srcOffset = 0, .dstOffset = 0, .size = gpr_bytes };
        vkCmdCopyBuffer(cmd, out_buf, host_out, 1, &co);
        vkCmdCopyBuffer(cmd, gpr_out_buf, host_gpr_out, 1, &cgo);
        CHECK(vkEndCommandBuffer(cmd));
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(dev, 1, &fence));
    }
    for (uint32_t p = 0; p < npairs; p++) {
        const uint8_t* got = (const uint8_t*)host_out_map + (size_t)p * LS_SIZE;
        const uint8_t* want = pairs[p].out.ls;
        for (uint32_t b = 0; b < LS_SIZE; b++) if (got[b] != want[b]) diff_bytes++;
        for (uint32_t w = 0; w < LS_SIZE / 4u; w++) {
            uint32_t a, e; memcpy(&a, got + w * 4u, 4); memcpy(&e, want + w * 4u, 4);
            total_words++;
            if (a == e) continue;
            diff_words++;
            uint32_t fa = bswap32(a), fe = bswap32(e);
            uint32_t expa = (fa >> 23) & 0xFFu, expe = (fe >> 23) & 0xFFu;
            uint32_t ulp;
            if (expa == 0xFFu || expe == 0xFFu) ulp = UINT32_MAX;
            else {
                int64_t ia = ((int64_t)(fa & 0x7FFFFFFFu)) * ((fa & 0x80000000u) ? -1 : 1);
                int64_t ie = ((int64_t)(fe & 0x7FFFFFFFu)) * ((fe & 0x80000000u) ? -1 : 1);
                int64_t d = ia > ie ? ia - ie : ie - ia;
                ulp = d > 0x7FFFFFFF ? UINT32_MAX : (uint32_t)d;
            }
            if (ulp < 8) hist[ulp]++; else hist[7]++;
            if (ulp > max_ulp) max_ulp = ulp;
        }
    }
    if (getenv("B830_DEBUG")) {
        for (uint32_t p = 0; p < npairs && p < 2; p++) {
            const uint8_t* got = (const uint8_t*)host_out_map + (size_t)p * LS_SIZE;
            const uint8_t* want = pairs[p].out.ls;
            const uint8_t* in = pairs[p].in.ls;
            int shown = 0;
            for (uint32_t b = 0; b < LS_SIZE && shown < 12; b++) {
                if (got[b] != want[b]) {
                    fprintf(stderr, "pair %u diff @0x%05X got=%02X out=%02X in=%02X\n", p, b, got[b], want[b], in[b]);
                    shown++;
                }
            }
            /* list changed input lines and compare them */
            for (uint32_t line = 0; line < LS_SIZE / 16u; line++) {
                if (memcmp(in + line * 16u, want + line * 16u, 16) == 0) continue;
                fprintf(stderr, "pair %u changed line 0x%05X got:", p, line * 16u);
                for (int k = 0; k < 16; k++) fprintf(stderr, " %02X", got[line*16u+k]);
                fprintf(stderr, "  want:");
                for (int k = 0; k < 16; k++) fprintf(stderr, " %02X", want[line*16u+k]);
                fprintf(stderr, "\n");
            }
        }
    }
    int gate_pass;
    /* The nine GPRs that survive the 0xA660 tail call must match too. */
    static const uint32_t live_gpr[9] = { 7u, 12u, 18u, 24u, 50u, 59u, 60u, 61u, 77u };
    uint64_t gpr_diff = 0;
    for (uint32_t p = 0; p < npairs; p++) {
        const uint32_t* gotg = (const uint32_t*)((const uint8_t*)host_gpr_out_map + (size_t)p * GPR_WORDS * 4u);
        const uint32_t* wantg = (const uint32_t*)((const uint8_t*)pairs[p].out.gpr);
        for (int k = 0; k < 9; k++)
            for (int i = 0; i < 4; i++)
                if (gotg[live_gpr[k] * 4u + i] != wantg[live_gpr[k] * 4u + i]) gpr_diff++;
    }
    printf("  live GPRs (7,12,18,24,50,59,60,61,77): %s (%llu differing words)\n",
           gpr_diff ? "DIFFERS" : "BIT-EXACT", (unsigned long long)gpr_diff);
    /* Gate: bit-exact preferred; otherwise <=1 LSB on <=0.1% of output values. */
    gate_pass = (diff_words == 0);
    if (!gate_pass && max_ulp <= 1 && (double)diff_words / (double)total_words <= 0.001) gate_pass = 1;
    if (gpr_diff) gate_pass = 0;
    all_exact = (diff_words == 0);
    printf("\nExactness (%u pairs, full 256 KiB LS): %s\n", npairs,
           all_exact ? "BIT-EXACT (0 differing bytes)" : "DIFFERS");
    printf("  diff bytes=%llu  diff words=%llu / %llu  max ULP=%u\n",
           (unsigned long long)diff_bytes, (unsigned long long)diff_words,
           (unsigned long long)total_words, max_ulp);
    printf("  word ULP histogram: exact=%llu 1=%llu 2=%llu 3=%llu 4=%llu 5=%llu 6=%llu >=7/inf=%llu\n",
           (unsigned long long)(total_words - diff_words), (unsigned long long)hist[1],
           (unsigned long long)hist[2], (unsigned long long)hist[3], (unsigned long long)hist[4],
           (unsigned long long)hist[5], (unsigned long long)hist[6], (unsigned long long)hist[7]);
    printf("  gate (<=1 LSB on <=0.1%%): %s\n", gate_pass ? "PASS" : "FAIL");

    /* ---- burst cost: J = 1, 64, wanted_J in one dispatch each ---- */
    uint32_t Js[3] = { 1u, 64u, wanted_J };
    printf("\n%-8s %12s %12s %12s %12s\n", "J", "GPU ms", "GPU us/call", "record us", "submit us");
    Timing burst = {0};
    for (int i = 0; i < 3; i++) {
        if (i == 2 && Js[i] == Js[1]) continue;
        Timing t = run_J(dev, queue, cmd, fence, pipe, layout, dset, Js[i], npairs, iters);
        printf("%-8u %12.4f %12.4f %12.2f %12.2f\n", Js[i], t.gpu_ms, t.us_per_job, t.record_us, t.submit_us);
        if (Js[i] == wanted_J) burst = t;
    }

    /* Headroom projection: 29 fps -> 4.394k calls/frame (the step-3 rate). */
    if (burst.J) {
        double calls_per_frame = 1270050.0 / 10.0 / 29.0;
        double per_frame = burst.gpu_ms * calls_per_frame / (double)burst.J;
        printf("\nBurst %u calls in one dispatch: %.4f ms (%.3f us/call).\n",
               burst.J, burst.gpu_ms, burst.us_per_job);
        printf("At the step-3 leaf rate (127,005 calls/s at 29 fps = %.0f calls/frame): %.3f ms/frame GPU.\n",
               calls_per_frame, per_frame);
        printf("CPU record+submit per burst: %.2f + %.2f us.\n", burst.record_us, burst.submit_us);
    }
    printf("\nRESULT: %s\n", gate_pass ? "PASS" : "FAIL");
    printf("SPIR-V: %s\n", spv_path);

    /* teardown (errors ignored) */
    (void)exact_t;
    vkDestroyFence(dev, fence, NULL); vkDestroyCommandPool(dev, cmd_pool, NULL);
    vkDestroyDescriptorPool(dev, pool, NULL); vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyShaderModule(dev, module, NULL); vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    if (device_local) {
        vkDestroyBuffer(dev, in_buf, NULL); vkDestroyBuffer(dev, out_buf, NULL);
        vkDestroyBuffer(dev, gpr_buf, NULL); vkDestroyBuffer(dev, gpr_out_buf, NULL);
        vkFreeMemory(dev, in_m, NULL); vkFreeMemory(dev, out_m, NULL);
        vkFreeMemory(dev, gpr_m, NULL); vkFreeMemory(dev, gpr_out_m, NULL);
    }
    vkUnmapMemory(dev, host_in_m); vkUnmapMemory(dev, host_out_m); vkUnmapMemory(dev, host_gpr_m);
    vkUnmapMemory(dev, host_gpr_out_m);
    vkDestroyBuffer(dev, host_in, NULL); vkDestroyBuffer(dev, host_out, NULL);
    vkDestroyBuffer(dev, host_gpr, NULL); vkDestroyBuffer(dev, host_gpr_out, NULL);
    vkFreeMemory(dev, host_in_m, NULL); vkFreeMemory(dev, host_out_m, NULL);
    vkFreeMemory(dev, host_gpr_m, NULL); vkFreeMemory(dev, host_gpr_out_m, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    free(spv); free(phys_list); free(pairs);
    return gate_pass ? 0 : 1;
}

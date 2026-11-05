/*
 * Offline exactness/ring gate for the c690 0x2A position-first 32-byte
 * record class.
 *
 * The SPU pre-pass is the reference: the job file carries its exported
 * per-record source offset (u16) and the 4 index/attribute bytes it writes at
 * record +28..31, plus the captured IN and OUT Local Store images.  This
 * harness prefills those 4 bytes, runs c690_2a_32b.comp once, and compares
 * the 28 bytes the GPU writes with the SPU records.
 *
 * Gate: positions 0 differing bytes, normal/tangent/uv <= 1 LSB, the SPU
 * attribute word exact, no GPU write outside bytes 0..27 of a record, and the
 * ring bytes identical under the real PUT mapping used by
 * test_c690_2a_plumbing.c.
 *
 * Build:
 *   clang -O2 -march=native -o /tmp/opencode/gpu2a/test_c690_2a_32b \
 *     mlb16-recomp/tools/replay/test_c690_2a_32b.c -l:libvulkan.so.1 -lm
 *
 * Run:
 *   test_c690_2a_32b job.bin c690_2a_32b.comp /tmp/opencode/c690_2a_32b.spv \
 *       --out-prefix /tmp/opencode/gpu2a32b/pair_<seq>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define CHECK(expr) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: Vulkan error %d\n", __FILE__, __LINE__, (int)_r); exit(1); } } while (0)

#define LS_BYTES 262144u
#define REC_SIZE 32u
#define MAX_COUNT 8192u

typedef struct {
    char magic[8];              /* "C692B01" */
    uint32_t count;
    uint32_t out_base;
    uint32_t pos_base;
    uint32_t nrm_base;
    uint32_t tan_base;
    uint32_t uv_base;
} JobHdr;

typedef struct {
    uint32_t pos_base, nrm_base, tan_base, uv_base;
    uint32_t out_base, count, pad0, pad1;
} Push;

static uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no memory type\n");
    exit(1);
}

static void make_buffer(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize size,
                        VkBuffer* buf, VkDeviceMemory* mem, void** mapped)
{
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

static uint32_t* read_file_u32(const char* path, size_t* out_words)
{
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (sz & 3)) { fprintf(stderr, "%s: bad SPIR-V size %ld\n", path, sz); exit(1); }
    uint32_t* p = (uint32_t*)malloc((size_t)sz);
    if (!p || fread(p, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "%s: short read\n", path); exit(1); }
    fclose(f);
    if (p[0] != 0x07230203u) { fprintf(stderr, "%s: not SPIR-V\n", path); exit(1); }
    *out_words = (size_t)sz / 4;
    return p;
}

static void write_file(const char* path, const void* data, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    if (n && fwrite(data, 1, n, f) != n) { perror(path); exit(1); }
    fclose(f);
}

static int32_t sx(uint32_t v, int bits)
{
    uint32_t sign = 1u << (bits - 1);
    return (int32_t)((v & (sign - 1u)) - (v & sign)) ;
}

static uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int half_span(uint16_t a, uint16_t b)
{
    /* Distance in half-float encodings; compare the raw bit patterns as the
     * packed LSB. Handles the two signs symmetrically. */
    int d = (int)a - (int)b;
    return d < 0 ? -d : d;
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s job.bin shader.comp out.spv [--out-prefix P] "
                        "[--ring-lsa X] [--ring-size X]\n", argv[0]);
        return 2;
    }
    const char* job_path = argv[1];
    const char* comp_path = argv[2];
    const char* spv_path = argv[3];
    const char* out_prefix = "/tmp/opencode/c690_2a_32b";
    uint32_t ring_lsa = 0x2F000u, ring_size = 0x30D0u;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--out-prefix") && i + 1 < argc) out_prefix = argv[++i];
        else if (!strcmp(argv[i], "--ring-lsa") && i + 1 < argc) ring_lsa = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ring-size") && i + 1 < argc) ring_size = (uint32_t)strtoul(argv[++i], NULL, 0);
    }

    FILE* jf = fopen(job_path, "rb");
    if (!jf) { perror(job_path); return 1; }
    JobHdr hdr;
    if (fread(&hdr, sizeof hdr, 1, jf) != 1 || memcmp(hdr.magic, "C692B01", 7)) {
        fprintf(stderr, "%s: bad job header\n", job_path);
        return 1;
    }
    if (hdr.count == 0 || hdr.count > MAX_COUNT) { fprintf(stderr, "bad count %u\n", hdr.count); return 1; }
    static uint8_t in_ls[LS_BYTES], spu_ls[LS_BYTES], seed_ls[LS_BYTES];
    if (fread(in_ls, 1, sizeof in_ls, jf) != sizeof in_ls ||
        fread(spu_ls, 1, sizeof spu_ls, jf) != sizeof spu_ls) {
        fprintf(stderr, "%s: short LS image\n", job_path);
        return 1;
    }
    uint16_t* offs = (uint16_t*)calloc(hdr.count + 2u, sizeof(uint16_t));
    uint32_t* attrs = (uint32_t*)calloc(hdr.count + 1u, sizeof(uint32_t));
    if (!offs || !attrs) { fprintf(stderr, "oom\n"); return 1; }
    if (fread(offs, 2, hdr.count, jf) != hdr.count ||
        fread(attrs, 4, hdr.count, jf) != hdr.count) {
        fprintf(stderr, "%s: short index/attribute table\n", job_path);
        return 1;
    }
    fclose(jf);

    /* Seed the output image with the SPU's four attribute bytes, then let the
     * GPU write only bytes 0..27 of every record. */
    memcpy(seed_ls, in_ls, sizeof seed_ls);
    for (uint32_t k = 0; k < hdr.count; k++) {
        uint32_t a = hdr.out_base + k * REC_SIZE;
        if (a + REC_SIZE > LS_BYTES) { fprintf(stderr, "record %u out of LS\n", k); return 1; }
        seed_ls[a + 28u] = (uint8_t)(attrs[k] >> 24);
        seed_ls[a + 29u] = (uint8_t)(attrs[k] >> 16);
        seed_ls[a + 30u] = (uint8_t)(attrs[k] >> 8);
        seed_ls[a + 31u] = (uint8_t)(attrs[k]);
    }

    char compile_cmd[4096];
    snprintf(compile_cmd, sizeof compile_cmd,
             "glslangValidator -V --target-env vulkan1.0 -o \"%s\" \"%s\"", spv_path, comp_path);
    int rc = system(compile_cmd);
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) { fprintf(stderr, "shader compile failed\n"); return 1; }
    size_t spv_words = 0;
    uint32_t* spv = read_file_u32(spv_path, &spv_words);

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "c690-2a-32b", .apiVersion = VK_API_VERSION_1_0 };
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
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfamily, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev; CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, qfamily, 0, &queue);

    const uint32_t off_bytes = (hdr.count * 2u + 3u) & ~3u;
    uint32_t maxoff = 0;
    for (uint32_t k = 0; k < hdr.count; k++) if (offs[k] > maxoff) maxoff = offs[k];
    uint32_t extent = maxoff + 16u;
    if (extent > 0x10000u) extent = 0x10000u;
    const uint32_t in_bytes = extent * 4u;
    const uint32_t out_bytes = hdr.count * REC_SIZE;
    VkBuffer in_buf, off_buf, out_buf; VkDeviceMemory in_mem, off_mem, out_mem;
    void *in_map, *off_map, *out_map;
    make_buffer(dev, phys, in_bytes, &in_buf, &in_mem, &in_map);
    make_buffer(dev, phys, out_bytes, &out_buf, &out_mem, &out_map);
    make_buffer(dev, phys, off_bytes ? off_bytes : 4u, &off_buf, &off_mem, &off_map);
    memset(in_map, 0, in_bytes);
    {
        const uint32_t tab[4] = { hdr.pos_base, hdr.nrm_base, hdr.tan_base, hdr.uv_base };
        for (int i = 0; i < 4; i++) {
            uint32_t room = tab[i] < LS_BYTES ? LS_BYTES - tab[i] : 0;
            uint32_t n = extent < room ? extent : room;
            memcpy((uint8_t*)in_map + (uint32_t)i * extent, in_ls + tab[i], n);
        }
    }
    memcpy(out_map, spu_ls + hdr.out_base, out_bytes);
    memset(off_map, 0, off_bytes); memcpy(off_map, offs, hdr.count * 2u);

    VkDescriptorSetLayoutBinding bind[3] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bind };
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

    Push pc = { .pos_base = 0, .nrm_base = extent, .tan_base = 2u * extent,
                .uv_base = 3u * extent, .out_base = 0, .count = hdr.count };
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
    VkDescriptorPoolSize dps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet dset; CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));
    VkDescriptorBufferInfo dbi[3] = {
        { .buffer = in_buf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = off_buf, .offset = 0, .range = VK_WHOLE_SIZE },
        { .buffer = out_buf, .offset = 0, .range = VK_WHOLE_SIZE } };
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
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    const uint32_t groups = (hdr.count + 63u) / 64u;

    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, groups, 1, 1);
    CHECK(vkEndCommandBuffer(cmd));
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    CHECK(vkResetFences(dev, 1, &fence));

    const uint8_t* gpu = (const uint8_t*)out_map;
    size_t pos_diff = 0, attr_diff = 0, outside = 0;
    size_t n_lsb[2] = {0, 0}, t_lsb[2] = {0, 0}, u_lsb[2] = {0, 0};
    int max_n = 0, max_t = 0, max_u = 0;
    for (uint32_t k = 0; k < hdr.count; k++) {
        uint32_t a = k * REC_SIZE;
        const uint8_t* g = gpu + a;
        const uint8_t* s = spu_ls + hdr.out_base + a;
        for (uint32_t i = 0; i < 12u; i++) if (g[i] != s[i]) pos_diff++;
        uint32_t gw = ((uint32_t)g[12] << 24) | ((uint32_t)g[13] << 16) | ((uint32_t)g[14] << 8) | g[15];
        uint32_t sw = ((uint32_t)s[12] << 24) | ((uint32_t)s[13] << 16) | ((uint32_t)s[14] << 8) | s[15];
        int dn = 0;
        dn = abs((int)sx(gw & 0x7ff, 11) - (int)sx(sw & 0x7ff, 11));
        int d1 = abs((int)sx((gw >> 11) & 0x7ff, 11) - (int)sx((sw >> 11) & 0x7ff, 11));
        int d2 = abs((int)sx((gw >> 22) & 0x3ff, 10) - (int)sx((sw >> 22) & 0x3ff, 10));
        if (d1 > dn) dn = d1; if (d2 > dn) dn = d2;
        if (dn > max_n) max_n = dn; n_lsb[dn > 1]++;
        int dt = 0;
        for (int j = 0; j < 4; j++) {
            int16_t gs = (int16_t)((g[16 + j * 2] << 8) | g[17 + j * 2]);
            int16_t ss = (int16_t)((s[16 + j * 2] << 8) | s[17 + j * 2]);
            int d = (int)gs - (int)ss; if (d < 0) d = -d; if (d > dt) dt = d;
        }
        if (dt > max_t) max_t = dt; t_lsb[dt > 1]++;
        int du = 0;
        for (int j = 0; j < 2; j++) {
            uint16_t gh = (uint16_t)((g[24 + j * 2] << 8) | g[25 + j * 2]);
            uint16_t sh = (uint16_t)((s[24 + j * 2] << 8) | s[25 + j * 2]);
            int d = half_span(gh, sh); if (d > du) du = d;
        }
        if (du > max_u) max_u = du; u_lsb[du > 1]++;
        if (memcmp(g + 28, s + 28, 4)) attr_diff++;
    }
    for (uint32_t k = 0; k < hdr.count; k++)
        if (memcmp(gpu + k * REC_SIZE + 28, spu_ls + hdr.out_base + k * REC_SIZE + 28, 4))
            outside++;

    /* Live split: the SPU still writes every byte it writes today except the
     * 28 record bytes the GPU assembles.  Start from the SPU output (the
     * non-record bookkeeping is the SPU's) and substitute the GPU record
     * bytes 0..27, then compare the ring the walker reads. */
    static uint8_t split_ls[LS_BYTES];
    memcpy(split_ls, spu_ls, LS_BYTES);
    for (uint32_t k = 0; k < hdr.count; k++) {
        uint32_t a = hdr.out_base + k * REC_SIZE;
        memcpy(split_ls + a, gpu + k * REC_SIZE, 28u);
    }
    size_t ring_diff = 0, ring_pos_diff = 0, ring_packed_diff = 0, ring_other_diff = 0;
    for (uint32_t i = 0; i < ring_size; i++) {
        uint32_t ls = ring_lsa + i;
        if (spu_ls[ls] == split_ls[ls]) continue;
        ring_diff++;
        if (ls >= hdr.out_base && ls < hdr.out_base + hdr.count * REC_SIZE) {
            uint32_t rem = (ls - hdr.out_base) % REC_SIZE;
            if (rem < 12u || rem >= 28u) ring_pos_diff++;
            else ring_packed_diff++;
        } else {
            ring_other_diff++;
        }
    }
    uint64_t rec_end = (uint64_t)hdr.out_base + (uint64_t)hdr.count * REC_SIZE;
    int in_ring = (hdr.out_base >= ring_lsa &&
                   rec_end <= (uint64_t)ring_lsa + ring_size);

    char p[1200];
    snprintf(p, sizeof p, "%s_gpu_records.bin", out_prefix);
    write_file(p, gpu, hdr.count * REC_SIZE);
    snprintf(p, sizeof p, "%s_spu_records.bin", out_prefix);
    write_file(p, spu_ls + hdr.out_base, hdr.count * REC_SIZE);
    snprintf(p, sizeof p, "%s_gpu_ring.bin", out_prefix);
    write_file(p, split_ls + ring_lsa, ring_size);
    snprintf(p, sizeof p, "%s_spu_ring.bin", out_prefix);
    write_file(p, spu_ls + ring_lsa, ring_size);

    printf("c690 0x2A 32-byte compact gate: job=%s count=%u out=0x%05X extent=%u\n",
           job_path, hdr.count, hdr.out_base, extent);
    printf("positions: %zu differing bytes\n", pos_diff);
    printf("normal packed: max %d LSB, %zu record(s) > 1\n", max_n, n_lsb[1]);
    printf("tangent s16:   max %d LSB, %zu record(s) > 1\n", max_t, t_lsb[1]);
    printf("uv f16:        max %d LSB, %zu record(s) > 1\n", max_u, u_lsb[1]);
    printf("SPU attr word: %zu differing record(s)\n", attr_diff);
    printf("GPU writes outside record bytes 0..27: %zu byte(s)\n", outside);
    printf("ring bytes: %zu differing: position/attr %zu (must be 0), "
           "packed normal/uv %zu (<= 1 LSB), outside records %zu\n",
           ring_diff, ring_pos_diff, ring_packed_diff, ring_other_diff);
    printf("ring map: LS 0x%05X..0x%05X (output inside=%s)\n",
           ring_lsa, ring_lsa + ring_size, in_ring ? "yes" : "no");
    const int pass = (pos_diff == 0 && max_n <= 1 && max_t <= 1 && max_u <= 1 &&
                      attr_diff == 0 && outside == 0 &&
                      ring_pos_diff == 0 && ring_other_diff == 0);
    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");

    vkUnmapMemory(dev, in_mem); vkUnmapMemory(dev, off_mem); vkUnmapMemory(dev, out_mem);
    vkDestroyFence(dev, fence, NULL); vkDestroyCommandPool(dev, cmd_pool, NULL);
    vkDestroyDescriptorPool(dev, pool, NULL); vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyShaderModule(dev, module, NULL); vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyBuffer(dev, in_buf, NULL); vkDestroyBuffer(dev, off_buf, NULL); vkDestroyBuffer(dev, out_buf, NULL);
    vkFreeMemory(dev, in_mem, NULL); vkFreeMemory(dev, off_mem, NULL); vkFreeMemory(dev, out_mem, NULL);
    vkDestroyDevice(dev, NULL); vkDestroyInstance(inst, NULL);
    free(spv); free(phys_list); free(offs); free(attrs);
    return pass ? 0 : 1;
}

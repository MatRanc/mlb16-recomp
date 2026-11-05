// c690 0x2A vertex assembler: generic record writer shared by the per-class
// kernels.  The SPU index pre-pass is kept on the SPU; it exports, per record,
// the source entry index (a 16-byte-aligned entry in every attribute array)
// and the bytes it writes itself.  This kernel gathers that entry and writes
// only the class's fields; every other byte of the output image is seeded from
// the SPU image and left untouched.
//
// A source entry is 16 bytes, big-endian per 32-bit lane.  The output is the
// raw Local Store byte image, so values are byte-swapped on read and write.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std430, binding = 0) readonly buffer InBuffer  { uint inw[];  };
layout(std430, binding = 1) readonly buffer IdxBuffer { uint idxs[]; };
layout(std430, binding = 2) buffer OutBuffer { uint outw[]; };

layout(push_constant) uniform PushConstants {
    uint out_base;
    uint count;
    uint rec_size;
    uint b[7];
} pc;

uint bswap32(uint v)
{
    return (v >> 24u) |
           ((v >> 8u)  & 0x0000FF00u) |
           ((v << 8u)  & 0x00FF0000u) |
           (v << 24u);
}

float load_f32(uint slot, uint entry, uint off)
{
    return uintBitsToFloat(bswap32(inw[(pc.b[slot] + entry * 16u + off) >> 2u]));
}

void store_word(uint byte_off, uint v)
{
    outw[byte_off >> 2u] = bswap32(v);
}

void store_u16(uint byte_off, uint v)
{
    uint wi = byte_off >> 2u;
    uint byte = byte_off & 3u;
    uint spu = bswap32(outw[wi]);
    uint sh = (3u - byte) * 8u;          /* shift of the first byte */
    spu = (spu & ~(0xFFFFu << (sh - 8u))) | ((v & 0xFFFFu) << (sh - 8u));
    outw[wi] = bswap32(spu);
}

int enc_fixed(float v, float scale, int lo, int hi)
{
    return clamp(int(v * scale) - 1, lo, hi);
}

uint f16_bits(float v)
{
    /* The lifted pack truncates the f16 mantissa and flushes denormals to a
     * signed zero (the SPU path has no denormal f16 values on this corpus). */
    uint f = floatBitsToUint(v);
    uint sign = (f >> 16u) & 0x8000u;
    int e = int((f >> 23u) & 0xFFu) - 127;
    if (e < -14)
        return sign;
    if (e > 15)
        return sign | 0x7C00u;
    uint he = uint(e + 15) << 10u;
    uint hm = (f >> 13u) & 0x3FFu;
    return sign | he | hm;
}

uint pack_normal(uint slot, uint e, uint off)
{
    int x = enc_fixed(load_f32(slot, e, off + 0u), 1024.0, -1024, 1023);
    int y = enc_fixed(load_f32(slot, e, off + 4u), 1024.0, -1024, 1023);
    int z = enc_fixed(load_f32(slot, e, off + 8u),  512.0,  -512,  511);
    return (uint(x) & 0x7FFu) | ((uint(y) & 0x7FFu) << 11u) |
           ((uint(z) & 0x3FFu) << 22u);
}

uint pack_pos(uint slot, uint e, uint off, uint dst)
{
    float px = load_f32(slot, e, off + 0u);
    float py = load_f32(slot, e, off + 4u);
    float pz = load_f32(slot, e, off + 8u);
    store_word(dst + 0u, floatBitsToUint(px));
    store_word(dst + 4u, floatBitsToUint(py));
    store_word(dst + 8u, floatBitsToUint(pz));
    return dst + 12u;
}

uint pack_s16x4(uint slot, uint e, uint off, uint dst, float scale)
{
    int a = enc_fixed(load_f32(slot, e, off + 0u),  scale, -32768, 32767);
    int b = enc_fixed(load_f32(slot, e, off + 4u),  scale, -32768, 32767);
    int c = enc_fixed(load_f32(slot, e, off + 8u),  scale, -32768, 32767);
    int d = enc_fixed(load_f32(slot, e, off + 12u), scale, -32768, 32767);
    store_word(dst + 0u, (uint(a) & 0xFFFFu) << 16u | (uint(b) & 0xFFFFu));
    store_word(dst + 4u, (uint(c) & 0xFFFFu) << 16u | (uint(d) & 0xFFFFu));
    return dst + 8u;
}

uint pack_s16x2(uint slot, uint e, uint off, uint dst, float scale)
{
    int a = enc_fixed(load_f32(slot, e, off + 0u), scale, -32768, 32767);
    int b = enc_fixed(load_f32(slot, e, off + 4u), scale, -32768, 32767);
    store_word(dst, (uint(a) & 0xFFFFu) << 16u | (uint(b) & 0xFFFFu));
    return dst + 4u;
}

uint pack_s16x3(uint slot, uint e, uint off, uint dst, float scale)
{
    int a = enc_fixed(load_f32(slot, e, off + 0u), scale, -32768, 32767);
    int b = enc_fixed(load_f32(slot, e, off + 4u), scale, -32768, 32767);
    int c = enc_fixed(load_f32(slot, e, off + 8u), scale, -32768, 32767);
    store_u16(dst + 0u, uint(a) & 0xFFFFu);
    store_u16(dst + 2u, uint(b) & 0xFFFFu);
    store_u16(dst + 4u, uint(c) & 0xFFFFu);
    return dst + 6u;
}

uint pack_f16x2(uint slot, uint e, uint off, uint dst)
{
    uint u = f16_bits(load_f32(slot, e, off + 0u));
    uint v = f16_bits(load_f32(slot, e, off + 4u));
    store_word(dst, (u << 16u) | v);
    return dst + 4u;
}

void main()
{
    uint k = gl_GlobalInvocationID.x;
    if (k >= pc.count)
        return;
    uint e = idxs[k];
    uint o = pc.out_base + k * pc.rec_size;

#if defined(C690_CLASS_POSONLY)
    pack_pos(0u, e, 0u, o);
#elif defined(C690_CLASS_20B)
    pack_pos(0u, e, 0u, o);
    pack_f16x2(1u, e, 0u, o + 12u);
    pack_s16x2(2u, e, 0u, o + 16u, 8192.0);
#elif defined(C690_CLASS_28B_POS)
    pack_pos(0u, e, 0u, o);
    store_word(o + 12u, pack_normal(1u, e, 0u));
    pack_s16x4(2u, e, 0u, o + 16u, 32768.0);
    pack_f16x2(3u, e, 0u, o + 24u);
#elif defined(C690_CLASS_32B_POS)
    pack_pos(0u, e, 0u, o);
    store_word(o + 12u, pack_normal(1u, e, 0u));
    pack_s16x4(2u, e, 0u, o + 16u, 32768.0);
    pack_f16x2(3u, e, 0u, o + 24u);
#elif defined(C690_CLASS_32B_ATTR)
    store_word(o + 8u,  pack_normal(0u, e, 0u));
    store_word(o + 12u, pack_normal(1u, e, 0u));
    store_word(o + 16u, pack_normal(2u, e, 0u));
    pack_pos(3u, e, 0u, o + 20u);
#elif defined(C690_CLASS_28B_ATTR)
    store_word(o + 4u,  pack_normal(0u, e, 0u));
    store_word(o + 8u,  pack_normal(1u, e, 0u));
    store_word(o + 12u, pack_normal(2u, e, 0u));
    pack_pos(3u, e, 0u, o + 16u);
#elif defined(C690_CLASS_44B)
    pack_pos(0u, e, 0u, o);
    store_word(o + 12u, pack_normal(1u, e, 0u));
    pack_s16x2(2u, e, 0u, o + 16u, 1024.0);
    pack_s16x4(3u, e, 0u, o + 20u, 32768.0);
    pack_s16x2(4u, e, 0u, o + 28u, 16384.0);
    pack_s16x3(5u, e, 0u, o + 32u, 1024.0);
    pack_s16x3(6u, e, 0u, o + 38u, 1024.0);
#else
#error "no C690_CLASS_x defined"
#endif
}

#!/usr/bin/env python3
"""Offline exactness gate for the c690 0x2A per-class record kernels.

Parses a PS3RECOMP_C690_TAIL_CAPTURE spool, classifies every single-command
0x2A pair into one of the decoded record layouts, recovers the source entry
index the SPU pre-pass used (the record's position identifies it), finds the
stream base that reproduces every packed field, runs the class shader once and
diffs its output against the captured SPU Local Store.

Classes (record size / fields):
  pos    12  position only (the 48-byte family is four positions in a row)
  20b    20  pos + f16 uv + 2 x s16 (1/8192)
  28b_pos 28 pos + normal 11:11:10 + 4 x s16 tan + f16 uv
  32b_pos 32 the phase-2b position-first class (SPU attr +28..31)
  32b_attr 32 three 11:11:10 packs + pos at +20 (SPU prefix +0..7)
  28b_attr 28 three 11:11:10 packs + pos at +16 (SPU prefix +0..3)
  44b    44 pos + normal + 2 x s16 + 4 x s16 + 2 x s16 + 2 x 3 x s16

Policy: positions exact, packed values <= 1 LSB, bytes the class does not
write must equal the captured SPU image, and no write outside the records.

Usage:
  tools/replay/test_c690_2a_classes.py --spool DIR [--spool DIR] [--limit N]
      [--work DIR] [--no-vk]
"""
import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

MAGIC = b'C69STG01'
T_IN, T_OUT = 9, 10
META_WORDS = 24
BITMAP_BYTES = 2048
LS_SIZE = 256 * 1024
GPR_SIZE = 128 * 16
HDR = struct.Struct('<IIQII')


def be32(buf, off):
    return struct.unpack_from('>I', buf, off)[0]


def f32(buf, off):
    return struct.unpack_from('>f', buf, off)[0]


def parse_file(path):
    blob = Path(path).read_bytes()
    if blob[:8] != MAGIC:
        raise ValueError(f'{path}: bad magic')
    off = 12
    pairs = {}
    while off + HDR.size <= len(blob):
        rtype, size, ea, lsa, aux = HDR.unpack_from(blob, off)
        off += HDR.size
        payload = blob[off:off + size]
        off += size
        if len(payload) != size:
            break
        meta = struct.unpack_from('<%dI' % META_WORDS, payload, 0)
        base = META_WORDS * 4
        ls = payload[base + BITMAP_BYTES:base + BITMAP_BYTES + LS_SIZE]
        gpr = payload[base + BITMAP_BYTES + LS_SIZE:
                      base + BITMAP_BYTES + LS_SIZE + GPR_SIZE]
        ent = {'type': rtype, 'op': meta[4], 'seq': meta[7],
               'cmd': meta[12:20], 'ls': ls, 'gpr': gpr}
        pairs.setdefault(meta[7], {})['in' if rtype == T_IN else 'out'] = ent
    return [p for p in pairs.values() if 'in' in p and 'out' in p
            and p['in']['op'] == 0x2A]


# ---- encodings (mirror c690_2a_records.inc) --------------------------------

def enc_fixed(v, scale, lo, hi):
    return max(lo, min(hi, int(v * scale) - 1))


def f16_bits(v):
    """Truncate the mantissa and flush denormals to signed zero, like the SPU."""
    f = struct.unpack('>I', struct.pack('>f', v))[0]
    sign = (f >> 16) & 0x8000
    e = ((f >> 23) & 0xFF) - 127
    if e < -14:
        return sign
    if e > 15:
        return sign | 0x7C00
    he = (e + 15) << 10
    hm = (f >> 13) & 0x3FF
    return sign | he | hm


def pack_normal(ls, a):
    x = enc_fixed(f32(ls, a + 0), 1024.0, -1024, 1023)
    y = enc_fixed(f32(ls, a + 4), 1024.0, -1024, 1023)
    z = enc_fixed(f32(ls, a + 8), 512.0, -512, 511)
    return (x & 0x7FF) | ((y & 0x7FF) << 11) | ((z & 0x3FF) << 22)


def pack_s16s(ls, a, n, scale):
    return [enc_fixed(f32(ls, a + 4 * i), scale, -32768, 32767)
            for i in range(n)]


def pack_f16s(ls, a, n):
    return [f16_bits(f32(ls, a + 4 * i)) for i in range(n)]


def sx(v, bits):
    sign = 1 << (bits - 1)
    return (v & (sign - 1)) - (v & sign)


FIELD_SPECS = {
    'pos':    {'bytes': 12},
    'normal': {'bytes': 4},
    's16x4':  {'bytes': 8},
    's16x2':  {'bytes': 4},
    's16x3':  {'bytes': 6},
    'f16x2':  {'bytes': 4},
}

# name, shader, record size, fields (name, dst, enc, param)
CLASSES = [
    ('32b_pos',  'c690_2a_32b_pos.comp',  32,
     [('pos', 0, 'pos', 0), ('normal', 12, 'normal', 0),
      ('s16x4', 16, 's16x4', 32768.0), ('f16x2', 24, 'f16x2', 0)]),
    ('32b_attr', 'c690_2a_32b_attr.comp', 32,
     [('normal0', 8, 'normal', 0), ('normal1', 12, 'normal', 0),
      ('normal2', 16, 'normal', 0), ('pos', 20, 'pos', 0)]),
    ('28b_attr', 'c690_2a_28b_attr.comp', 28,
     [('normal0', 4, 'normal', 0), ('normal1', 8, 'normal', 0),
      ('normal2', 12, 'normal', 0), ('pos', 16, 'pos', 0)]),
    ('28b_pos',  'c690_2a_28b_pos.comp',  28,
     [('pos', 0, 'pos', 0), ('normal', 12, 'normal', 0),
      ('s16x4', 16, 's16x4', 32768.0), ('f16x2', 24, 'f16x2', 0)]),
    ('20b',      'c690_2a_20b.comp',      20,
     [('pos', 0, 'pos', 0), ('f16x2', 12, 'f16x2', 0),
      ('s16x2', 16, 's16x2', 8192.0)]),
    ('44b',      'c690_2a_44b.comp',      44,
     [('pos', 0, 'pos', 0), ('normal', 12, 'normal', 0),
      ('s16x2a', 16, 's16x2', 1024.0), ('s16x4', 20, 's16x4', 32768.0),
      ('s16x2b', 28, 's16x2', 16384.0), ('s16x3a', 32, 's16x3', 1024.0),
      ('s16x3b', 38, 's16x3', 1024.0)]),
    ('pos',      'c690_2a_pos.comp',      12,
     [('pos', 0, 'pos', 0)]),
]

SPU_WRITTEN = {
    '32b_pos': [(28, 4)],
    '32b_attr': [(0, 8)],
    '28b_attr': [(0, 4)],
}


def u16s(buf, off, n):
    return [struct.unpack_from('>H', buf, off + 2 * i)[0] for i in range(n)]


def field_components(enc, raw, param):
    """Decode a field's raw bytes (big-endian as stored) into comparable ints."""
    if enc == 'pos':
        return list(raw)
    if enc == 'normal':
        w = int.from_bytes(raw, 'big')
        return [sx(w & 0x7FF, 11), sx((w >> 11) & 0x7FF, 11),
                sx((w >> 22) & 0x3FF, 10)]
    if enc == 's16x4':
        w0 = int.from_bytes(raw[0:4], 'big')
        w1 = int.from_bytes(raw[4:8], 'big')
        return [sx((w0 >> 16) & 0xFFFF, 16), sx(w0 & 0xFFFF, 16),
                sx((w1 >> 16) & 0xFFFF, 16), sx(w1 & 0xFFFF, 16)]
    if enc == 's16x2':
        w = int.from_bytes(raw, 'big')
        return [sx((w >> 16) & 0xFFFF, 16), sx(w & 0xFFFF, 16)]
    if enc == 's16x3':
        return [sx(int.from_bytes(raw[0:2], 'big'), 16),
                sx(int.from_bytes(raw[2:4], 'big'), 16),
                sx(int.from_bytes(raw[4:6], 'big'), 16)]
    if enc == 'f16x2':
        w = int.from_bytes(raw, 'big')
        return [(w >> 16) & 0xFFFF, w & 0xFFFF]
    raise ValueError(enc)


def expected_component(enc, param, ls, a):
    if enc == 'pos':
        return [ls[a + i] for i in range(12)]
    if enc == 'normal':
        w = pack_normal(ls, a)
        return [sx(w & 0x7FF, 11), sx((w >> 11) & 0x7FF, 11),
                sx((w >> 22) & 0x3FF, 10)]
    if enc == 's16x4':
        return pack_s16s(ls, a, 4, param)
    if enc == 's16x2':
        return pack_s16s(ls, a, 2, param)
    if enc == 's16x3':
        return pack_s16s(ls, a, 3, param)
    if enc == 'f16x2':
        return pack_f16s(ls, a, 2)
    raise ValueError(enc)


def comp_diff(enc, a, b):
    if enc == 'pos':
        return sum(1 for x, y in zip(a, b) if x != y)
    return max((abs(x - y) for x, y in zip(a, b)), default=0)


def matches(enc, param, ls, a, ref_bytes, tol=1):
    exp = expected_component(enc, param, ls, a)
    got = field_components(enc, ref_bytes, param)
    if enc == 'pos':
        return exp == got
    return all(abs(x - y) <= tol for x, y in zip(exp, got))


def get_bases(ls, r3):
    return [be32(ls, (r3 + 0x260 + 4 * i) & 0x3FFFF) for i in range(8)]


def try_class(pair, cls):
    name, shader, size, fields = cls
    ls = pair['in']['ls']
    out = pair['out']['ls']
    gpr = pair['in']['gpr']
    r3 = struct.unpack_from('<I', gpr, 3 * 16)[0] & 0x3FFFF
    bases = get_bases(ls, r3)
    ob = be32(ls, (r3 + 0xD0) & 0x3FFFF)
    end = be32(out, (r3 + 0xD0) & 0x3FFFF)
    if not ob:
        return None
    delta = (end - ob) & 0x3FFFF
    if delta == 0 or delta % size:
        return None
    n = delta // size
    if n == 0 or n > 4096 or ob + n * size > LS_SIZE:
        return None

    pos_field = next(f for f in fields if f[2] == 'pos')
    p = pos_field[1]
    rec_pos = [out[ob + k * size + p:ob + k * size + p + 12] for k in range(n)]
    nonpos = [f for f in fields if f[0] != pos_field[0]]
    refs = {}
    for (fname, dst, enc, param) in nonpos:
        refs[fname] = [out[ob + k * size + dst:
                           ob + k * size + dst + FIELD_SPECS[enc]['bytes']]
                       for k in range(n)]

    covers = []
    for bi, b in enumerate(bases):
        if not b:
            continue
        m = {}
        for a in range(0, 0x4000, 16):
            m.setdefault(ls[b + a:b + a + 12], []).append(a // 16)
        got = [m.get(rp) for rp in rec_pos]
        if all(got):
            covers.append((bi, got))

    for (pos_base, cand) in covers:
        def any_score(bi, fname, enc, param):
            b = bases[bi]
            return sum(1 for k in range(n)
                       if any(matches(enc, param, ls, b + c * 16, refs[fname][k])
                              for c in cand[k]))

        def joint_sel(src):
            sel = []
            for k in range(n):
                best_c, best_score = cand[k][0], -1
                for c in cand[k]:
                    score = 0
                    for (fname, dst, enc, param) in nonpos:
                        b = bases[src[fname]]
                        if matches(enc, param, ls, b + c * 16, refs[fname][k]):
                            score += 1
                    if score > best_score:
                        best_score, best_c = score, c
                if best_score < len(nonpos):
                    return None
                sel.append(best_c)
            return sel

        ranked = {}
        ok = True
        for (fname, dst, enc, param) in nonpos:
            scores = []
            for bi, b in enumerate(bases):
                if not b:
                    continue
                scores.append((any_score(bi, fname, enc, param), -bi, bi))
            scores.sort(reverse=True)
            if not scores or scores[0][0] < n:
                ok = False
                break
            ranked[fname] = [x[2] for x in scores if x[0] >= n]
        if not ok:
            continue

        src = {f[0]: None for f in fields}
        src[pos_field[0]] = pos_base

        def search(i):
            if i == len(nonpos):
                return joint_sel(src)
            fname = nonpos[i][0]
            for bi in ranked[fname]:
                src[fname] = bi
                r = search(i + 1)
                if r is not None:
                    return r
            src[fname] = None
            return None

        sel = search(0)
        if sel is None:
            continue
        good = True
        for (fname, dst, enc, param) in nonpos:
            b = bases[src[fname]]
            if not all(matches(enc, param, ls, b + sel[k] * 16, refs[fname][k])
                       for k in range(n)):
                good = False
                break
        if good:
            return {'cls': cls, 'ob': ob, 'n': n, 'idxs': sel, 'src': src,
                    'bases': bases, 'size': size, 'fields': fields}
    return None


def build_job(path, cfg, in_ls, seed):
    fields = cfg['fields']
    bases = [0] * 7
    # field slots are the field ordinal (shader uses slot 0..6)
    for slot, f in enumerate(fields):
        bases[slot] = cfg['bases'][cfg['src'][f[0]]]
    with open(path, 'wb') as fh:
        fh.write(struct.pack('<8s3I7I', b'C69CLS01', cfg['n'], cfg['ob'],
                             cfg['size'], *bases))
        fh.write(struct.pack('<%dI' % cfg['n'], *cfg['idxs']))
        fh.write(in_ls)
        fh.write(seed)


def build_runner(repo, work):
    exe = work / 'test_c690_2a_classes'
    cmd = ['clang', '-O2', '-o', str(exe),
           str(repo / 'tools/replay/test_c690_2a_classes.c'),
           '-l:libvulkan.so.1', '-lm']
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError('runner build failed:\n' + res.stderr)
    return exe


def main():
    repo = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser()
    ap.add_argument('--spool', action='append', default=[])
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--work', default='/tmp/opencode/c690_2a_classes')
    ap.add_argument('--no-vk', action='store_true')
    args = ap.parse_args()
    if not args.spool:
        ap.error('at least one --spool is required')
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)

    files = []
    for d in args.spool:
        files.extend(sorted(Path(d).glob('tail_*.bin')))
    pairs = []
    for f in files:
        pairs.extend(parse_file(f))
    print('0x2A pairs: %d' % len(pairs))
    if args.limit:
        pairs = pairs[:args.limit]

    exe = None if args.no_vk else build_runner(repo, work)
    tag = 0
    covered = 0
    total_records = 0
    stats = {'pos_bytes': 0, 'max_normal': 0, 'max_s16': 0, 'max_f16': 0,
             'outside': 0, 'seed_diff': 0, 'pass': 0, 'fail': 0}
    per_class = {}
    unclassified = {'ob0': 0, 'nopos': 0, 'layout': 0}
    rows = []

    for pair in pairs:
        if not be32(pair['in']['ls'], 0x1F690):
            pass
        r3 = struct.unpack_from('<I', pair['in']['gpr'], 3 * 16)[0] & 0x3FFFF
        ob = be32(pair['in']['ls'], (r3 + 0xD0) & 0x3FFFF)
        if not ob:
            unclassified['ob0'] += 1
            continue
        cfg = None
        for cls in CLASSES:
            cfg = try_class(pair, cls)
            if cfg:
                break
        if cfg is None:
            # distinguish a position-less output from a known-layout miss
            out = pair['out']['ls']
            end = be32(out, (r3 + 0xD0) & 0x3FFFF)
            delta = (end - ob) & 0x3FFFF
            prefixes = {pair['in']['ls'][a:a + 12]
                        for a in range(0, LS_SIZE - 16, 16)}
            has_pos = any(out[o:o + 12] in prefixes
                          for o in range(ob, min(ob + delta, LS_SIZE) - 12, 4))
            unclassified['nopos' if not has_pos else 'layout'] += 1
            continue
        name, shader, size, fields = cfg['cls']
        in_ls = pair['in']['ls']
        seed = bytearray(in_ls)
        for (off, ln) in SPU_WRITTEN.get(name, []):
            for k in range(cfg['n']):
                src = cfg['ob'] + k * size + off
                seed[src:src + ln] = pair['out']['ls'][src:src + ln]
        job = work / ('pair_%06d.job' % tag)
        build_job(job, cfg, in_ls, bytes(seed))
        tag += 1
        total_records += cfg['n']
        pc = per_class.setdefault(name, {'pairs': 0, 'pass': 0, 'records': 0})
        pc['pairs'] += 1
        pc['records'] += cfg['n']
        if exe is None:
            continue
        spv = work / ('pair_%06d.spv' % (tag - 1))
        gpu_path = work / ('pair_%06d.gpu' % (tag - 1))
        cmd = [str(exe), str(job), str(repo / 'tools/replay' / shader), str(spv),
               '--out', str(gpu_path)]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        if res.returncode != 0:
            print('runner failed on %s: %s' % (name, res.stderr.strip()[:200]))
            stats['fail'] += 1
            continue
        gpu = gpu_path.read_bytes()
        ref = pair['out']['ls']

        covered_bytes = set()
        for (fname, dst, enc, param) in fields:
            ln = FIELD_SPECS[enc]['bytes']
            for i in range(ln):
                covered_bytes.add(dst + i)
        pos_diff = 0
        max_lsb = 0
        outside = 0
        seed_diff = 0
        for k in range(cfg['n']):
            a = cfg['ob'] + k * size
            for (fname, dst, enc, param) in fields:
                ln = FIELD_SPECS[enc]['bytes']
                g = gpu[a + dst:a + dst + ln]
                s = ref[a + dst:a + dst + ln]
                if enc == 'pos':
                    pos_diff += sum(1 for x, y in zip(g, s) if x != y)
                else:
                    d = comp_diff(enc, field_components(enc, g, param),
                                  field_components(enc, s, param))
                    if d > max_lsb:
                        max_lsb = d
            for off in range(size):
                if off not in covered_bytes and gpu[a + off] != ref[a + off]:
                    outside += 1
        # no writes outside the record range
        start = cfg['ob']
        stop = cfg['ob'] + cfg['n'] * size
        outside += sum(1 for i in range(0, start) if gpu[i] != seed[i])
        outside += sum(1 for i in range(stop, LS_SIZE) if gpu[i] != seed[i])
        stats['pos_bytes'] += pos_diff
        stats['max_normal'] = max(stats['max_normal'], max_lsb)
        stats['outside'] += outside
        ok = (pos_diff == 0 and max_lsb <= 1 and outside == 0)
        stats['pass' if ok else 'fail'] += 1
        covered += 1 if ok else 0
        pc['pass'] += 1 if ok else 0
        rows.append((pair['in']['seq'], name, size, cfg['n'], pos_diff, max_lsb,
                     outside, 'PASS' if ok else 'FAIL'))

    print('\n%-9s %-9s %-4s %-6s %-5s %-5s %-6s %s' %
          ('seq', 'class', 'rec', 'n', 'pos', 'lsb', 'out', 'res'))
    for r in rows[:400]:
        print('%-9d %-9s %-4d %-6d %-5d %-5d %-6d %s' % r)
    print('\nper class:')
    for name, pc in sorted(per_class.items()):
        print('  %-9s pairs=%d pass=%d records=%d' %
              (name, pc['pairs'], pc['pass'], pc['records']))
    classified = sum(pc['pairs'] for pc in per_class.values())
    print('\nclassified pairs=%d (ob0=%d nopos=%d other-layout=%d)'
          % (classified, unclassified['ob0'], unclassified['nopos'],
             unclassified['layout']))
    print('records=%d positions=%d differing bytes; max packed LSB=%d; '
          'outside/seed=%d; pass=%d/%d'
          % (total_records, stats['pos_bytes'], stats['max_normal'],
             stats['outside'], stats['pass'], stats['pass'] + stats['fail']))
    tot = len(pairs)
    print('coverage=%d/%d 0x2A pairs (%.1f %%); pass rate %.1f %%'
          % (covered, tot, 100.0 * covered / tot,
             100.0 * stats['pass'] / max(1, stats['pass'] + stats['fail'])))
    ok = (stats['fail'] == 0 and stats['pos_bytes'] == 0 and
          stats['max_normal'] <= 1 and stats['outside'] == 0)
    print('RESULT: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

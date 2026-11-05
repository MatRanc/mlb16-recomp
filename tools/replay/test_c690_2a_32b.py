#!/usr/bin/env python3
"""Offline gate for the c690 0x2A position-first 32-byte record class.

Parses a PS3RECOMP_C690_TAIL_CAPTURE spool, finds the 32-byte pairs (the same
stride classifier as test_c690_2a_plumbing.py), exports the SPU pre-pass
index list and attribute word per record, builds one job per pair, runs the
Vulkan kernel (test_c690_2a_32b.c / c690_2a_32b.comp) and aggregates the
exactness and ring-byte result.

The index contract recovered from the meter corpora:
  * format A: cmd[0] is a u16 per record, a byte offset into all four planar
    arrays; record k gathers at array[cmd0[k]].
  * format B: record k gathers at array[k * 16] (the index list is implicit).
The 4 bytes at record +28..31 are SPU-written; the job carries the captured
SPU word and the kernel leaves it untouched.

Usage:
  tools/replay/test_c690_2a_32b.py --spool DIR [--spool DIR] [--limit N]
      [--work DIR] [--no-vk]

Defaults assume the sibling layout: mlb16-recomp/, ps3recomp-play/,
recomp-work/.
"""
import argparse
import os
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
REC_SIZE = 32
JOB_MAGIC = b'C692B01'

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_c690_2a_plumbing as plumb  # noqa: E402


def be32(buf, off):
    return struct.unpack_from('>I', buf, off)[0]


def load_pairs(spool_dir):
    pairs = []
    for name in sorted(Path(spool_dir).glob('tail_*.bin')):
        pairs.extend(plumb.parse_spool(name).values())
    return [p for p in pairs if p['in']['op'] == 0x2A]


def classify_pair(p):
    """Return (format, out_base, delta, count, tab, offsets) or None."""
    ls = p['in']['ls']
    ref = p['out']['ls']
    gpr = p['in']['gpr']
    r3 = struct.unpack_from('<I', gpr, 3 * 16)[0] & (LS_SIZE - 1)
    tab = [be32(ls, (r3 + 0x260 + i * 4) & (LS_SIZE - 1)) for i in range(4)]
    ob = be32(ls, (r3 + 0xD0) & (LS_SIZE - 1))
    end = be32(ref, (r3 + 0xD0) & (LS_SIZE - 1))
    delta = (end - ob) & (LS_SIZE - 1)
    if not ob or delta == 0 or delta % REC_SIZE:
        return None
    n = delta // REC_SIZE
    if n == 0 or n > 4096:
        return None
    prefixes = {ls[a:a + 12] for a in range(0, LS_SIZE - 16, 16)}
    ok = sum(1 for k in range(n)
             if ref[ob + k * REC_SIZE:ob + k * REC_SIZE + 12] in prefixes)
    if ok / n < 0.999:
        return None

    cmd = p['in'].get('cmd')
    if cmd is None:
        return None

    def matches(offs):
        return all(ls[tab[0] + o:tab[0] + o + 12] ==
                   ref[ob + k * REC_SIZE:ob + k * REC_SIZE + 12]
                   for k, o in enumerate(offs))

    offs = [struct.unpack_from('>H', ref, cmd[0] + k * 2)[0] for k in range(n)]
    if matches(offs):
        return ('A', ob, delta, n, tab, offs)
    offs = [k * 16 for k in range(n)]
    if matches(offs):
        return ('B', ob, delta, n, tab, offs)
    return None


def write_job(path, in_ls, out_ls, out_base, tab, offs, attrs):
    with open(path, 'wb') as f:
        f.write(struct.pack('<8s6I', JOB_MAGIC, len(offs), out_base,
                            tab[0], tab[1], tab[2], tab[3]))
        f.write(in_ls)
        f.write(out_ls)
        f.write(struct.pack('<%dH' % len(offs), *offs))
        f.write(struct.pack('<%dI' % len(attrs), *attrs))


def build_harness(repo, work):
    exe = work / 'test_c690_2a_32b'
    cmd = ['clang', '-O2', '-march=native', '-o', str(exe),
           str(repo / 'tools/replay/test_c690_2a_32b.c'), '-l:libvulkan.so.1', '-lm']
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError('harness build failed:\n' + res.stderr)
    return exe


NUM = re.compile(r'([-0-9]+)')
def parse_report(text):
    def grab(pat):
        m = re.search(pat, text)
        return int(m.group(1)) if m else None
    return {
        'pos_diff': grab(r'positions:\s+(\d+) differing'),
        'max_n': grab(r'normal packed: max (\d+) LSB'),
        'max_t': grab(r'tangent s16:\s+max (\d+) LSB'),
        'max_u': grab(r'uv f16:\s+max (\d+) LSB'),
        'attr_diff': grab(r'SPU attr word:\s+(\d+) differing'),
        'outside': grab(r'outside record bytes 0\.\.27:\s+(\d+)'),
        'ring_diff': grab(r'ring bytes:\s+(\d+) differing'),
        'ring_pos': grab(r'position/attr (\d+) \(must be 0\)'),
        'ring_packed': grab(r'packed normal/uv (\d+)'),
        'ring_other': grab(r'outside records (\d+)'),
        'pass': 'RESULT: PASS' in text,
    }


def main():
    repo = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser()
    ap.add_argument('--spool', action='append', default=[])
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--work', default=str(Path('/tmp/opencode') / 'gpu2a_32b'))
    ap.add_argument('--seq', type=int, default=0,
                    help='only this captured 0x2A seq (0 = all accepted)')
    ap.add_argument('--no-vk', action='store_true', help='only classify/export jobs')
    args = ap.parse_args()

    if not args.spool:
        ap.error('at least one --spool is required')
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)

    pairs = []
    for d in args.spool:
        pairs.extend(load_pairs(d))
    pairs.sort(key=lambda p: p['in']['seq'])
    print('0x2A pairs: %d' % len(pairs))

    accepted = []
    stats = {'A': 0, 'B': 0, 'none': 0}
    for p in pairs:
        c = classify_pair(p)
        if c is None:
            stats['none'] += 1
            continue
        stats[c[0]] += 1
        accepted.append((p, c))
    print('32-byte class: A(offset list)=%d B(identity)=%d, unclassified 32B=%d'
          % (stats['A'], stats['B'], stats['none']))
    if args.seq:
        accepted = [a for a in accepted if a[0]['in']['seq'] == args.seq]
        if not accepted:
            print('no accepted 32-byte pair with seq %d' % args.seq)
            return 1
    if args.limit:
        accepted = accepted[:args.limit]

    exe = None
    shader = repo / 'tools/replay/c690_2a_32b.comp'
    if not args.no_vk:
        exe = build_harness(repo, work)

    total_records = 0
    agg = {'pairs': 0, 'pos_diff': 0, 'max_n': 0, 'max_t': 0, 'max_u': 0,
           'attr_diff': 0, 'outside': 0, 'ring_diff': 0, 'ring_pos': 0,
           'ring_packed': 0, 'ring_other': 0, 'pass': 0}
    rows = []
    for idx, (p, c) in enumerate(accepted):
        fmt, ob, delta, n, tab, offs = c
        ref = p['out']['ls']
        attrs = [be32(ref, ob + k * REC_SIZE + 28) for k in range(n)]
        job = work / ('pair_%06d.job' % p['in']['seq'])
        write_job(job, p['in']['ls'], ref, ob, tab, offs, attrs)
        total_records += n
        if exe is None:
            continue
        spv = work / 'c690_2a_32b.spv'
        prefix = work / ('pair_%06d' % p['in']['seq'])
        cmd = [str(exe), str(job), str(shader), str(spv),
               '--out-prefix', str(prefix)]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        r = parse_report(res.stdout + res.stderr)
        r.update({'seq': p['in']['seq'], 'fmt': fmt, 'n': n,
                  'out': ob, 'desc': be32(p['in']['ls'], 0x1F690)})
        rows.append(r)
        agg['pairs'] += 1
        for k in ('pos_diff', 'max_n', 'max_t', 'max_u', 'attr_diff',
                  'outside', 'ring_diff', 'ring_pos', 'ring_packed',
                  'ring_other'):
            if r.get(k) is not None and r[k] > agg[k]:
                agg[k] = r[k]
        agg['pass'] += 1 if r['pass'] else 0

    if exe is None:
        print('jobs exported to %s; --no-vk set' % work)
        return 0

    print('\n%-9s %-3s %-5s %-6s %-5s %-5s %-5s %-6s %-7s %s' %
          ('seq', 'fmt', 'n', 'pos', 'maxN', 'maxT', 'maxU', 'attr', 'ring', 'res'))
    for r in rows:
        print('%-9d %-3s %-5d %-6s %-5s %-5s %-5s %-6s %-7s %s' %
              (r['seq'], r['fmt'], r['n'], r['pos_diff'], r['max_n'],
               r['max_t'], r['max_u'], r['attr_diff'], r['ring_diff'],
               'PASS' if r['pass'] else 'FAIL'))
    print('\npairs=%d records=%d' % (agg['pairs'], total_records))
    print('worst: positions=%d differing bytes, normal=%d LSB, tangent=%d LSB, '
          'uv=%d LSB, attr=%d, outside=%d, ring=%d differing '
          '(position/attr %d, packed %d, other %d); pass=%d/%d'
          % (agg['pos_diff'], agg['max_n'], agg['max_t'], agg['max_u'],
             agg['attr_diff'], agg['outside'], agg['ring_diff'],
             agg['ring_pos'], agg['ring_packed'], agg['ring_other'],
             agg['pass'], agg['pairs']))
    ok = (agg['pairs'] > 0 and agg['pass'] == agg['pairs'] and
          agg['pos_diff'] == 0 and agg['max_n'] <= 1 and agg['max_t'] <= 1 and
          agg['max_u'] <= 1 and agg['attr_diff'] == 0 and agg['outside'] == 0 and
          agg['ring_pos'] == 0 and agg['ring_other'] == 0)
    print('RESULT: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

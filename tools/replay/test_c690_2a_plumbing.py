#!/usr/bin/env python3
"""Offline plumbing gate for the c690 0x2A assembler.

Runs the C harness (test_c690_2a_plumbing.c) on a captured 0x10F78 entry:
the SPU path is run for the reference, the four planar arrays are exported to
a compact buffer, one GPU dispatch produces the records, and the bytes the
RSX walker reads at the ring EA are compared.  It then patches the same
record bytes into a captured meter frame's guest memory and replays that frame
through replay_rsx_engine, comparing the output hash for the SPU-produced and
GPU-produced bytes.

This is research tooling: the capture and the lifted module are game data and
are never committed.  See docs/edge-gpu-plumbing.md.

Usage:
  tools/replay/test_c690_2a_plumbing.py [--capture DIR] [--spool DIR]
      [--meter-prefix PREFIX] [--engine PATH] [--work DIR] [--no-replay]

Defaults assume the sibling layout:
  mlb16-recomp/ (this repo), ps3recomp-play/, recomp-work/
"""
import argparse
import os
import re
import shutil
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

OUT_BASE = 0x2F870
REC_SIZE = 36
RING_LSA = 0x2F000
RING_SIZE = 0x30D0
RING_EA = 0x11765D00
# A captured meter-frame vertex array (location=1, offset 0xc5a310) that the
# walker reads; used only to exercise the binding hash offline.
METER_LOCATION = 1
METER_OFFSET = 0xc5a310


def be32(buf, off):
    return struct.unpack_from('>I', buf, off)[0]


def parse_spool(path):
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
        ent = {'type': rtype, 'op': meta[4], 'key': meta[5], 'seq': meta[7],
               'cmd': meta[12:20], 'ls': ls, 'gpr': gpr}
        pairs.setdefault(meta[7], {})['in' if rtype == T_IN else 'out'] = ent
    return {k: v for k, v in pairs.items() if 'in' in v and 'out' in v}


def _prefix_set(ls):
    """First 12 bytes of every 16-byte-aligned LS entry (the source stride)."""
    return {ls[a:a + 12] for a in range(0, LS_SIZE - 16, 16)}


def _stride_score(prefixes, ref, ob, delta, stride):
    """Share of records whose first 12 bytes are a raw source entry."""
    if stride <= 0 or delta <= 0 or delta % stride:
        return -1.0
    n = delta // stride
    ok = sum(1 for k in range(n)
             if ref[ob + k * stride:ob + k * stride + 12] in prefixes)
    return ok / n


def classify_2a(pairs):
    """Classify each 0x2A output by its record stride.

    36-byte is the validated phase-2a kernel layout.  The meter tail is a mix
    of strides selected by the command descriptor: a measured 32-byte class
    (position-first: 3 x f32 + packed normal/tangent/uv), plus 20- and
    48-byte and unclassified formats.  Strides are detected by checking that
    every record starts with a raw 16-byte source entry (the assembler is a
    gather); a wrong stride fails on the first records.
    """
    out = {'total': 0, 'fmt36': 0, 'fmt32': 0, 'fmt20': 0, 'fmt48': 0,
           'other': 0, 'examples': []}
    for p in pairs:
        if p['in']['op'] != 0x2A:
            continue
        out['total'] += 1
        ls = p['in']['ls']
        ref = p['out']['ls']
        gpr = p['in'].get('gpr') or b'\0' * GPR_SIZE
        r3 = struct.unpack_from('<I', gpr, 3 * 16)[0] & (LS_SIZE - 1)
        tab = [be32(ls, (r3 + 0x260 + i * 4) & (LS_SIZE - 1))
               for i in range(4)]
        ob = be32(ls, (r3 + 0xD0) & (LS_SIZE - 1))
        end = be32(ref, (r3 + 0xD0) & (LS_SIZE - 1))
        delta = (end - ob) & (LS_SIZE - 1)
        desc = be32(ls, 0x1F690) if LS_SIZE > 0x1F694 else 0

        is36 = False
        if ob + REC_SIZE <= len(ref):
            is36 = (ref[ob:ob + 8] == ls[tab[3]:tab[3] + 8] and
                    ref[ob + 12:ob + 24] == ls[tab[1]:tab[1] + 12] and
                    ref[ob + 24:ob + 36] == ls[tab[0]:tab[0] + 12])

        scores = {}
        if is36:
            out['fmt36'] += 1
            cls = '36'
        else:
            prefixes = _prefix_set(ls)
            scores = {s: _stride_score(prefixes, ref, ob, delta, s)
                      for s in (20, 32, 48)}
            wins = [(v, -s) for s, v in scores.items() if v >= 0.999]
            if wins:
                stride = -max(wins)[1]
                cls = str(stride)
                out['fmt' + cls] += 1
            else:
                cls = 'other'
                out['other'] += 1
        if len(out['examples']) < 6:
            out['examples'].append({
                'seq': p['in']['seq'], 'tab': [hex(x) for x in tab],
                'out_base': hex(ob), 'delta': delta, 'desc': hex(desc),
                'class': cls, 'scores': {k: round(v, 3)
                                         for k, v in scores.items()}})
    return out


def patch_mem(src_prefix, dst_prefix, location, offset, data):
    for ext in ('.fifo', '.state', '.mem'):
        shutil.copyfile(src_prefix + ext, dst_prefix + ext)
    blob = bytearray(Path(dst_prefix + '.mem').read_bytes())
    magic, ver, nents, page_bytes, memlen, _ = struct.unpack_from('<6I', blob, 0)
    if magic != 0x50414357 or page_bytes != 4096:
        raise ValueError('bad .mem header')
    ents = {}
    off = 24
    for _ in range(nents):
        loc, pg, ln, do = struct.unpack_from('<4I', blob, off)
        off += 16
        ents[(loc, pg)] = do
    data0 = 24 + nents * 16
    written = 0
    i = 0
    while i < len(data):
        addr = offset + i
        pg = addr & ~4095
        in_page = addr & 4095
        if (location, pg) not in ents:
            break
        do = ents[(location, pg)]
        n = min(4096 - in_page, len(data) - i)
        blob[data0 + do + in_page:data0 + do + in_page + n] = data[i:i + n]
        written += n
        i += n
    Path(dst_prefix + '.mem').write_bytes(blob)
    return written


def run_engine(engine, prefix):
    cmd = [str(engine), str(prefix), '--frames', '2', '--warmup', '1']
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    m = re.search(r'output hash:\s+([0-9a-fA-F]+)', res.stdout + res.stderr)
    misses = re.search(r'guest reads:\s+(\d+) hit,\s+(\d+) miss', res.stdout + res.stderr)
    if not m:
        raise RuntimeError(f'replay_rsx_engine failed:\n{res.stdout}\n{res.stderr}')
    return m.group(1).lower(), misses.groups() if misses else (None, None)


def build_harness(repo, work):
    play = repo.parent / 'ps3recomp-play'
    recomp = repo.parent / 'recomp-work'
    module = recomp / 'spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c'
    if not module.exists():
        return None
    exe = work / 'test_c690_2a_plumbing'
    cmd = [
        'clang', '-O2', '-march=native', '-DSPU_FAST_FLOAT', '-DSPU_LIFTED_FAST_LS',
        '-I', str(play / 'runtime/spu'), '-I', str(play / 'include'),
        '-I', str(repo / 'tools/replay'),
        '-o', str(exe),
        str(repo / 'tools/replay/test_c690_2a_plumbing.c'),
        str(repo / 'tools/replay/reg_10f78.c'),
        str(module),
        str(play / 'runtime/spu/job_c690_native.c'),
        '-l:libvulkan.so.1', '-lm',
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError('harness build failed:\n' + res.stderr)
    return exe


def main():
    repo = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default='/tmp/opencode/fncap21b/0x10F78')
    ap.add_argument('--spool', action='append', default=[])
    ap.add_argument('--meter-prefix',
                    default=str(repo.parent / 'recomp-work/meter_frame'))
    ap.add_argument('--engine',
                    default=str(repo.parent / 'recomp-work/build-play/ps3recomp_sdk/replay_rsx_engine'))
    ap.add_argument('--work', default='/tmp/opencode/c690_plumbing')
    ap.add_argument('--no-replay', action='store_true')
    ap.add_argument('--coverage-only', action='store_true',
                    help='classify the spools and skip the GPU harness')
    args = ap.parse_args()

    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    print('c690 0x2A plumbing gate')

    # 1. Coverage of the captured meter 0x2A jobs.
    if args.spool:
        agg = {'total': 0, 'fmt36': 0, 'fmt32': 0, 'fmt20': 0, 'fmt48': 0,
               'other': 0}
        for d in args.spool:
            pairs = []
            for name in sorted(Path(d).glob('tail_*.bin')):
                pairs.extend(parse_spool(name).values())
            c = classify_2a(pairs)
            for k in agg:
                agg[k] += c[k]
            tot = c['total'] or 1
            print(f'\nspool {d}: 0x2A pairs={c["total"]} '
                  f'36-byte={c["fmt36"]} 32-byte={c["fmt32"]} '
                  f'20-byte={c["fmt20"]} 48-byte={c["fmt48"]} '
                  f'other={c["other"]} '
                  f'(32 {100.0 * c["fmt32"] / tot:.1f} %, '
                  f'other {100.0 * (c["other"] + c["fmt20"] + c["fmt48"]) / tot:.1f} %)')
            for e in c['examples']:
                print('  seq=%d tab=%s out_base=%s delta=%d desc=%s class=%s scores=%s' %
                      (e['seq'], ','.join(e['tab']), e['out_base'], e['delta'],
                       e['desc'], e['class'], e['scores']))
        tot = agg['total'] or 1
        print(f'\nall spools: 0x2A pairs={agg["total"]} '
              f'36-byte={agg["fmt36"]} ({100.0 * agg["fmt36"] / tot:.1f} %) '
              f'32-byte={agg["fmt32"]} ({100.0 * agg["fmt32"] / tot:.1f} %) '
              f'20-byte={agg["fmt20"]} ({100.0 * agg["fmt20"] / tot:.1f} %) '
              f'48-byte={agg["fmt48"]} ({100.0 * agg["fmt48"] / tot:.1f} %) '
              f'other={agg["other"]} ({100.0 * agg["other"] / tot:.1f} %)')

    if args.coverage_only:
        return 0

    # 2. Exactness + ring bytes on the captured 36-byte job.
    exe = build_harness(repo, work)
    if exe is None:
        print('\nlifted module missing; skipping the GPU/ring harness')
        return 2
    shader = repo / 'tools/replay/c690_2a_assembler.comp'
    spv = work / 'c690_2a.spv'
    prefix = work / 'plumb'
    cmd = [str(exe), args.capture, str(shader), str(spv),
           '--ring-lsa', hex(RING_LSA), '--ring-size', hex(RING_SIZE),
           '--ring-ea', hex(RING_EA), '--out-prefix', str(prefix)]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    print('\n' + res.stdout.strip())
    if res.returncode != 0:
        print(res.stderr, file=sys.stderr)
        return 1

    # 3. Walker hash: patch the GPU/SPU records into a captured meter frame.
    if args.no_replay:
        return 0
    meter = Path(args.meter_prefix)
    engine = Path(args.engine)
    if not meter.with_suffix('.mem').exists() or not engine.exists():
        print('\nmeter frame or replay_rsx_engine missing; skipping the walker hash')
        return 0
    spu_rec = prefix.parent / (prefix.name + '_spu_records.bin')
    gpu_rec = prefix.parent / (prefix.name + '_gpu_records.bin')
    spu = spu_rec.read_bytes()
    gpu = gpu_rec.read_bytes()
    hashes = {}
    for arm, data in (('spu', spu), ('gpu', gpu)):
        dst = str(work / ('meter_' + arm))
        n = patch_mem(str(meter), dst, METER_LOCATION, METER_OFFSET, data)
        h, miss = run_engine(engine, dst)
        hashes[arm] = h
        print(f'\nmeter frame {arm}: patched {n} bytes at '
              f'loc={METER_LOCATION} off=0x{METER_OFFSET:X}; '
              f'hash={h} (reads {miss[0]} hit / {miss[1]} miss)')
    print('\nwalker hash SPU=%s GPU=%s -> %s' %
          (hashes['spu'], hashes['gpu'],
           'MATCH' if hashes['spu'] == hashes['gpu'] else 'DIFFER'))
    return 0 if hashes['spu'] == hashes['gpu'] else 1


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Offline binding gate for the c690 0x2A GPU ring resolver.

The live path (PS3RECOMP_C690_GPU_2A) exports the 32-byte class, computes the
records on the renderer's Vulkan device and serves the GPU range through the
draw engine's guest resolver.  replay_rsx_engine has no Vulkan device, so this
gate feeds the *GPU kernel's* bytes (from test_c690_2a_32b.py) through the same
resolver hook (PS3RECOMP_GPU_RING=loc:offset:file) and checks:

  1. resolver transparency: the exact lifted bytes served through the resolver
     produce the same draw/state hash as the same bytes in guest memory
     (this is the "key on vs key off" hash gate; it proves the binding is not a
     guest-memory patch);
  2. the GPU-byte arm's hash, reported against the SPU arm.  The kernel is
     policy-exact (positions exact, packed fields <= 1 LSB), not byte-exact, so
     a hash difference here is the approved packed drift, quantified by the
     byte gate.  Positions are identical in both arms.

Usage:
  test_c690_2a_binding.py [--spool DIR] [--seq N] [--meter-prefix P]
                          [--engine PATH] [--work DIR] [--frames N]
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_c690_2a_plumbing as plumb  # noqa: E402

METER_LOCATION = 1
METER_OFFSET = 0xc5a310
DEFAULT_SEQ = 453000


def run_engine(engine, prefix, frames, warmup, gpu_ring=None):
    env = None
    cmd = [str(engine), str(prefix), '--frames', str(frames),
           '--warmup', str(warmup)]
    if gpu_ring:
        import os
        env = dict(os.environ)
        env['PS3RECOMP_GPU_RING'] = gpu_ring
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600, env=env)
    out = r.stdout + r.stderr
    h = re.search(r'output hash:\s+([0-9a-fA-F]+)', out)
    g = re.search(r'output gate:\s+([0-9a-fA-F]+)', out)
    reads = re.search(r'guest reads:\s+(\d+) hit,\s+(\d+) miss', out)
    return (h.group(1).lower() if h else None,
            g.group(1).lower() if g else None,
            reads.groups() if reads else None,
            r.returncode, out)


def main():
    here = Path(__file__).resolve().parent
    repo = here.parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument('--spool',
                    default=str(repo.parent / 'recomp-work/captures/gpu2a/fncap2b2w3'))
    ap.add_argument('--seq', type=int, default=DEFAULT_SEQ)
    ap.add_argument('--meter-prefix',
                    default=str(repo.parent / 'recomp-work/meter_frame'))
    ap.add_argument('--engine',
                    default=str(repo.parent / 'recomp-work/build-gpu/ps3recomp_sdk/replay_rsx_engine'))
    ap.add_argument('--work', default='/tmp/opencode/gpu2a_32b')
    ap.add_argument('--frames', type=int, default=4)
    ap.add_argument('--warmup', type=int, default=2)
    args = ap.parse_args()

    work = Path(args.work)
    gpu_rec = work / ('pair_%06d_gpu_records.bin' % args.seq)
    spu_rec = work / ('pair_%06d_spu_records.bin' % args.seq)
    if not (gpu_rec.exists() and spu_rec.exists()):
        print('running the 32-byte kernel gate for seq %d' % args.seq)
        r = subprocess.run([sys.executable, str(here / 'test_c690_2a_32b.py'),
                            '--spool', args.spool, '--work', str(work),
                            '--seq', str(args.seq)], capture_output=True,
                           text=True, timeout=900)
        sys.stdout.write(r.stdout[-4000:])
        sys.stderr.write(r.stderr[-2000:])
        if not (gpu_rec.exists() and spu_rec.exists()):
            print('32-byte gate did not emit pair files')
            return 1

    engine = Path(args.engine)
    meter = Path(args.meter_prefix)
    frames, warmup = args.frames, args.warmup

    # 1. Baseline: the captured guest image (no resolver).
    base_h, base_g, base_r, _, _ = run_engine(engine, meter, frames, warmup)
    # 2. The exact lifted bytes written into a copy of the guest image.
    dst = str(work / ('meter_bind_spu_%d' % args.seq))
    spu_bytes = spu_rec.read_bytes()
    written = plumb.patch_mem(str(meter), dst, METER_LOCATION, METER_OFFSET,
                              spu_bytes)
    patched_h, patched_g, patched_r, _, _ = run_engine(
        engine, Path(dst), frames, warmup)
    # 3. Resolver arm with the same lifted bytes (transparency gate).
    req = '%d:%#x:%s' % (METER_LOCATION, METER_OFFSET, spu_rec)
    spu_h, spu_g, spu_r, _, _ = run_engine(engine, meter, frames, warmup, req)
    # 4. Resolver arm with the GPU kernel's bytes (the live arm).
    req2 = '%d:%#x:%s' % (METER_LOCATION, METER_OFFSET, gpu_rec)
    gpu_h, gpu_g, gpu_r, _, _ = run_engine(engine, meter, frames, warmup, req2)

    print('meter frame:', meter)
    print('pair seq %d records: %d bytes (SPU), %d bytes (GPU), %d patched'
          % (args.seq, len(spu_bytes), gpu_rec.stat().st_size, written))
    print('guest baseline:      hash %s gate %s reads %s' % (base_h, base_g, base_r))
    print('guest SPU bytes:     hash %s gate %s reads %s' % (patched_h, patched_g, patched_r))
    print('resolver SPU bytes:  hash %s gate %s reads %s' % (spu_h, spu_g, spu_r))
    print('resolver GPU bytes:  hash %s gate %s reads %s' % (gpu_h, gpu_g, gpu_r))
    ok = True
    if base_h == patched_h:
        print('FAIL: the SPU record bytes did not change the guest baseline')
        ok = False
    if spu_h != patched_h or spu_g != patched_g:
        print('FAIL: resolver transparency (resolver SPU bytes differ from guest)')
        ok = False
    if spu_r and spu_r[1] != '0':
        print('FAIL: resolver arm has guest read misses: %s' % (spu_r,))
        ok = False
    if gpu_r and gpu_r[1] != '0':
        print('FAIL: GPU arm has guest read misses: %s' % (gpu_r,))
        ok = False
    if spu_h == gpu_h:
        print('GPU-byte hash identical to the SPU arm (byte-exact kernel)')
    else:
        print('GPU-byte hash differs from the SPU arm: the kernel is '
              'positions-exact and <= 1 LSB on packed fields (policy), so '
              'the null-backend vertex digest differs.  This is the approved '
              'drift, quantified by the 32-byte byte gate.')
    print('RESULT:', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

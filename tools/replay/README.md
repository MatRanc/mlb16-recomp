# Offline per-function replay harness

Run a lifted SPU c690 function on **real captured game state**, without booting the
game or waiting through the ~3-minute match load. This is the loop for developing
native kernels (AVX2/AVX-512) and for checking a kernel bit-exactly against the lift.

Currently wired for `0x6D68` (attribute unpack/dequantize), a true leaf: no DMA,
no indirect branch, no `SPU_DIRECT`.

## 1. Capture the input state (in the game)

The runtime has a generic per-function capture (`ps3recomp`, off by default):

```sh
cd ~/Documents/mlb16-online/recomp-work
mkdir -p /tmp/opencode/fncap
EXTRA="SPU_FN_CAPTURE=0x6D68:/tmp/opencode/fncap" ab-tools/measure_fast.sh fncap.log
```

This dumps, the first time `0x6D68` is dispatched:

- `ls.bin`  — 256 KB local store
- `gpr.bin` — 128 GPRs × 16 bytes
- `pc.txt`  — `lsa=0x06D68 pc=0x06D68 image=6`

The capture fires from the function body, so it works for `SPU_DIRECT`-called leaves.

## 2. Build and run the replay

```sh
gcc -std=gnu11 -O2 -march=native -DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS \
    -I~/Documents/mlb16-online/ps3recomp-native/include \
    -I~/Documents/mlb16-online/ps3recomp-native/runtime/spu \
    -o /tmp/opencode/fncap/test_job_c690_replay \
    ~/Documents/mlb16-online/mlb16-recomp/tools/replay/test_job_c690_replay.c -lm
/tmp/opencode/fncap/test_job_c690_replay /tmp/opencode/fncap
```

`argv[1]` is the capture directory (default `/tmp/opencode/fncap`). The test prints
a deterministic FNV-1a checksum of the resulting LS and GPRs, the inferred loop
count, and a per-call timing (GPRs reset per call; the LS is reset once and written
in place at the same addresses).

Captured pitch-screen sample (2026-11-01):

```
Inferred loop count: 24
FNV-1a64 LS+GPR: 0xfc2e9569dc2feb11
Final pc=0x108E8
lifted fn only: ~563 ns/call
```

## 3. Adding a native kernel

Replace the `#include "ref_spu_func_6d68.c"` reference with a call to the native
kernel, then run both on the same captured state and diff the LS+GPR checksums
(the `test_job_c690_native.c` pattern in `ps3recomp-native/runtime/spu/tests/`).

`ref_spu_func_6d68.c` is a verbatim copy of the lifted body; regenerate it from
`recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c` after any re-lift.

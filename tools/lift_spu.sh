#!/bin/sh
# Lifts MLB 16's SPU code: the 5 SPU ELF programs in the EBOOT, the WWS job manager's policy module and the job
# modules linked into the EBOOT. Output (generated from the game, keep out of git) goes to <work>/spu_lifted, which
# ps3recomp's template picks up with -DSPU_DIR=<work>/spu_lifted.
# Usage: tools/lift_spu.sh /path/to/ps3recomp /path/to/EBOOT.elf work
set -e
SDK=$1 ELF=$2 OUT=$3
PY="$SDK/.venv/bin/python"
[ -x "$PY" ] || PY=python3
mkdir -p "$OUT/spu_images" "$OUT/spu_lifted"
"$PY" "$SDK/tools/extract_spu_images.py" "$ELF" -o "$OUT/spu_images"
# The WWS job manager's policy module isn't an ELF: SPURS gets 12,032 raw bytes at 0x105bb80 (file offset
# 0x104bb80) and loads them at local store 0xA00. Wrapped at that base, its text segment fingerprints the same as
# what the runtime looks up (0x75BFE68AE96D7577).
"$PY" - "$ELF" "$OUT/wwsjob_pm.bin" <<'PYEOF'
import sys
d = open(sys.argv[1], 'rb').read()
open(sys.argv[2], 'wb').write(d[0x104bb80:0x104bb80 + 12032])
PYEOF
"$PY" "$SDK/tools/wrap_spu_elf.py" --base 0xA00 --entry 0xA00 --out "$OUT/spu_images/wwsjob_pm.elf" "$OUT/wwsjob_pm.bin"
"$PY" "$SDK/tools/build_spu_workloads.py" --images "$OUT/spu_images" --lifted "$OUT/spu_lifted" \
    --out "$OUT/spu_lifted/spu_workloads.c" --title mlb16 --constructor
# Entry points the job modules reach only through function pointers. Without them those calls fall to the SPU
# interpreter, which made loading into a match crawl. Caught with a gdb breakpoint on spu_interp_run_until
# (docs/progress.md); lift_jobmods only splits a function that actually contains the address.
LBP_JOBMOD_EXTRA_FUNCS=0x8ec4,0x918c,0x91a8,0x9200,0x9df8,0x9e34,0xb238,0xbcb4,0xbcdc,0xbd20,0xbdb4,0xbdd8,0xbe20,0xbe5c,0xc70c,0xc74c,0xc764,0xc78c,0xc7a0,0xc7d4,0xc7e8,0xc98c,0xd2b8,0xd4a8,0xd7a0,0xd810,0xd878,0xd9d0 \
"$PY" "$SDK/tools/lift_jobmods.py" --engine-elf "$ELF" --out "$OUT/spu_lifted" \
    --register "$OUT/spu_lifted/jobmods_register.c" --symbol-prefix mlb16
grep -q 0x75BFE68AE96D7577 "$OUT/spu_lifted/spu_workloads.c" && echo "job manager registered"

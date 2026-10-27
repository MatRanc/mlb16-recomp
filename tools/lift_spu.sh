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
"$PY" "$SDK/tools/lift_jobmods.py" --engine-elf "$ELF" --out "$OUT/spu_lifted" \
    --register "$OUT/spu_lifted/jobmods_register.c" --symbol-prefix mlb16
grep -q 0x75BFE68AE96D7577 "$OUT/spu_lifted/spu_workloads.c" && echo "job manager registered"

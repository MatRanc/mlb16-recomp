#!/bin/sh
# Runs ps3recomp's front end on your own decrypted MLB 16 EBOOT and reports what it finds.
# Usage: tools/probe.sh /path/to/ps3recomp /path/to/EBOOT.elf [workdir]
# Output goes to workdir (default ./work), which is generated from the game: keep it out of git.
set -e
SDK=$1 ELF=$2 OUT=${3:-work}
# real code ends here; everything after is read-only data (checked in Ghidra: SECTION5 at 0xf2c634 is r--)
CODE_END=0xf2c634
PY="$SDK/.venv/bin/python"
[ -x "$PY" ] || PY=python3
mkdir -p "$OUT/recompiled" "$OUT/spu"
"$PY" "$SDK/tools/ppu_loader.py" "$ELF" -o "$OUT"
"$PY" "$SDK/tools/ppu_lifter.py" "$ELF" --functions "$OUT/EBOOT.functions.json" \
    --hle-stubs "$OUT/EBOOT.imports.json" --code-end $CODE_END --output "$OUT/recompiled/"
"$PY" "$SDK/tools/gen_hle_nids.py" --all --out "$OUT/recompiled/ppu_hle_nids.cpp"
"$PY" "$SDK/tools/extract_spu_images.py" "$ELF" -o "$OUT/spu"
echo "undecoded words in lifted PPU code: $(cat "$OUT"/recompiled/ppu_recomp_*.cpp | grep -c 'TODO: .word')"

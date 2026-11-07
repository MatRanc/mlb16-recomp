#!/bin/bash
# Re-apply every generated-code patch after a fresh PPU/SPU lift.
#
# The lifted trees are game-derived and not in git. The stable build needs:
#   1. tools/c690_ref/patch_cull_hooks.py       (ps3recomp port/stable)
#   2. tools/c690_ref/patch_native_screen.py    (ps3recomp port/stable, after 1)
#   3. tools/patch_bink_ready.py                (this repo, on the PPU lift)
#
# Optional, off by default, because each needs a different ps3recomp branch and
# the key behind it defaults off. The play build uses --wws-native --ftz --approx:
#   --wws-native  tools/c690_ref/patch_wws_pm_native.py  (port/stable or later;
#                 the runtime header runtime/spu/wws_native_pm.h must exist)
#   --ftz         tools/spu_ftz_twin.py                  (port/stable or later; needs the
#                 variant registry in runtime/spu/spu_workload.c)
#   --approx      tools/spu_ftz_twin.py --approx         (port/stable or later)
#   --native-21   tools/c690_ref/patch_native_21.py      (exp/e1/n21 or later)
#   --native-2a   tools/c690_ref/patch_native_2a.py      (exp/e1/n2a or later)
#   --gpu2a       tools/c690_ref/patch_gpu2a.py          (exp/gpu or later)
#   --capture     pass --capture to patch_cull_hooks.py (offline capture build)
#
# Usage: tools/apply_patches.sh [options]
#   --work DIR        recomp-work (default: repository sibling)
#   --ps3recomp DIR   ps3recomp checkout (default: sibling ps3recomp-play)
#   --dry-run         print the commands, change nothing
#   -h, --help        this text
#
# Idempotent: every patch script detects an already-patched tree. Run it after
# tools/probe.sh and tools/lift_spu.sh, and again after any relift.
set -euo pipefail

S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$S/.." && pwd)

WORK=${MLB16_WORK:-$(cd "$REPO/.." && pwd)/recomp-work}
PS3RECOMP=${MLB16_PS3RECOMP:-${PS3RECOMP_DIR:-$(cd "$WORK/.." && pwd)/ps3recomp-play}}
RECOMPILED=${RECOMPILED:-$WORK/recompiled}
SPU_DIR=${SPU_DIR:-$WORK/spu_lifted}
C690="$SPU_DIR/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c"
WWS_PM="$SPU_DIR/wwsjob_pm/spu_recomp.c"
DRY=0
CAPTURE=0
WWS_NATIVE=0
APPROX=0
FTZ=0
NATIVE_21=0
NATIVE_2A=0
GPU2A=0

while [ $# -gt 0 ]; do
    case "$1" in
        --work) WORK=$2; shift 2 ;;
        --ps3recomp) PS3RECOMP=$2; shift 2 ;;
        --dry-run) DRY=1; shift ;;
        --capture) CAPTURE=1; shift ;;
        --wws-native) WWS_NATIVE=1; shift ;;
        --approx) APPROX=1; shift ;;
        --ftz) FTZ=1; shift ;;
        --native-21) NATIVE_21=1; shift ;;
        --native-2a) NATIVE_2A=1; shift ;;
        --gpu2a) GPU2A=1; shift ;;
        -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0 ;;
        *) echo "apply_patches.sh: unknown argument '$1' (try --help)" >&2; exit 2 ;;
    esac
done

RECOMPILED=${RECOMPILED:-$WORK/recompiled}
SPU_DIR=${SPU_DIR:-$WORK/spu_lifted}
C690="$SPU_DIR/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c"
WWS_PM="$SPU_DIR/wwsjob_pm/spu_recomp.c"
PY=${PYTHON:-python3}

need_file() {
    [ -f "$1" ] || { echo "apply_patches.sh: missing $1$2" >&2; exit 1; }
}

run() {
    if [ "$DRY" = 1 ]; then
        printf 'DRY:'; printf ' %q' "$@"; printf '\n'
    else
        printf '+'; printf ' %q' "$@"; printf '\n'
        "$@"
    fi
}

need_file "$RECOMPILED/ppu_recomp.h" " (run tools/probe.sh first)"
need_file "$C690" " (run tools/lift_spu.sh first)"
need_file "$S/patch_bink_ready.py"

CULL="$PS3RECOMP/tools/c690_ref/patch_cull_hooks.py"
SCREEN="$PS3RECOMP/tools/c690_ref/patch_native_screen.py"
need_file "$CULL" " (ps3recomp checkout on port/stable or exp/live-ab)"
need_file "$SCREEN" " (ps3recomp checkout on port/stable or exp/live-ab)"

if [ -f "$WWS_PM" ] && grep -q wws_native_pm "$WWS_PM" 2>/dev/null &&
   [ ! -f "$PS3RECOMP/runtime/spu/wws_native_pm.h" ]; then
    echo "apply_patches.sh: WARNING: $WWS_PM includes wws_native_pm.h," >&2
    echo "  but $PS3RECOMP has no such header. Build only on a branch that has" >&2
    echo "  it (exp/wws-native or later), or relift into a fresh spu_lifted/." >&2
fi

cull_args=("$CULL")
[ "$CAPTURE" = 1 ] && cull_args+=(--capture)
cull_args+=("$C690")
run "$PY" "${cull_args[@]}"
run "$PY" "$SCREEN" "$C690"
run "$PY" "$S/patch_bink_ready.py" "$RECOMPILED"

if [ "$WWS_NATIVE" = 1 ]; then
    WWS="$PS3RECOMP/tools/c690_ref/patch_wws_pm_native.py"
    need_file "$WWS" " (ps3recomp exp/wws-native or later)"
    need_file "$PS3RECOMP/runtime/spu/wws_native_pm.h" " (the WWS native PM header)"
    need_file "$WWS_PM"
    run "$PY" "$WWS" "$WWS_PM"
fi

if [ "$NATIVE_21" = 1 ]; then
    P="$PS3RECOMP/tools/c690_ref/patch_native_21.py"
    need_file "$P" " (ps3recomp exp/e1/n21 or later)"
    run "$PY" "$P" "$C690"
fi
if [ "$NATIVE_2A" = 1 ]; then
    P="$PS3RECOMP/tools/c690_ref/patch_native_2a.py"
    need_file "$P" " (ps3recomp exp/e1/n2a or later)"
    run "$PY" "$P" "$C690"
fi
if [ "$GPU2A" = 1 ]; then
    P="$PS3RECOMP/tools/c690_ref/patch_gpu2a.py"
    need_file "$P" " (ps3recomp exp/gpu or later)"
    run "$PY" "$P" "$C690"
fi

if [ "$FTZ" = 1 ]; then
    P="$PS3RECOMP/tools/spu_ftz_twin.py"
    need_file "$P" " (ps3recomp exp/spu-v4 or later)"
    grep -q spu_workload_register_img_ftz "$PS3RECOMP/runtime/spu/spu_workload.c" 2>/dev/null || {
        echo "apply_patches.sh: $PS3RECOMP/runtime/spu/spu_workload.c has no FTZ variant registry" >&2
        echo "  --ftz needs ps3recomp exp/spu-v4 (or later)." >&2
        exit 1
    }
    run "$PY" "$P" --spu-dir "$SPU_DIR"
fi

if [ "$APPROX" = 1 ]; then
    P="$PS3RECOMP/tools/spu_ftz_twin.py"
    need_file "$P" " (ps3recomp port/stable or later)"
    run "$PY" "$P" --spu-dir "$SPU_DIR" --approx
fi

echo "apply_patches.sh: done"

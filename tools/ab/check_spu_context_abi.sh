#!/usr/bin/env bash
# Guard against spu_context layout drift between the runtime and the lifted
# SPU modules.
#
# The lifted modules generated under recomp-work/spu_lifted include
#   ../../runtime/spu/spu_context.h
# i.e. recomp-work/runtime, which is a symlink (normally to
# ps3recomp-native/runtime). The runtime library is built from the port tree's
# own runtime/spu/spu_context.h. The two headers are allowed to differ only by
# fields APPENDED at the end of spu_context. A field inserted in the middle
# moves dcache_id/host_depth/steps/... that the lifted dispatch reads at the
# old offsets and the c690 job dies at 0x1095C after three jobs
# (see docs/freeze-20261103.md).
#
# Compile one offset probe against each header and compare every shared field.
# Exit 0 when the shared prefix agrees, 1 otherwise. SPU_ABI_CHECK=0 skips.
set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$S/../.." && pwd)
W=${MLB16_WORK:-$(cd "$REPO/.." && pwd)/recomp-work}
SDK=${SDK:-${MLB16_PS3RECOMP:-${PS3RECOMP_DIR:-$(cd "$W/.." && pwd)/ps3recomp-play}}}
LIFT_RT=${LIFT_RT:-$W/runtime/spu}
CC=${CC:-cc}

[ "${SPU_ABI_CHECK:-1}" = 0 ] && { echo "spu-context ABI check: disabled"; exit 0; }
# Launchers pass the build's CMakeCache: the runtime header is the one the
# build used (PS3RECOMP_DIR), and the check only applies when that build uses
# the shared lifted modules in recomp-work/spu_lifted.
if [ -n "${BUILD_CACHE:-}" ] && [ -f "$BUILD_CACHE" ]; then
    sdk_cache=$(sed -n 's/^PS3RECOMP_DIR:[^=]*=//p' "$BUILD_CACHE" | head -1)
    [ -n "$sdk_cache" ] && SDK=$sdk_cache
    spu_dir=$(sed -n 's/^SPU_DIR:[^=]*=//p' "$BUILD_CACHE" | head -1)
    if [ "$spu_dir" != "$W/spu_lifted" ]; then
        echo "spu-context ABI check: skip (build uses SPU_DIR=${spu_dir:-none}, not the shared lifted modules)"
        exit 0
    fi
fi
[ -f "$SDK/runtime/spu/spu_context.h" ] || { echo "spu-context ABI check: no $SDK/runtime/spu/spu_context.h (skip)"; exit 0; }
[ -f "$LIFT_RT/spu_context.h" ] || { echo "spu-context ABI check: no $LIFT_RT/spu_context.h (skip)"; exit 0; }

tmp=$(mktemp -d "${TMPDIR:-/tmp}/spuabi.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/probe.c" <<'PROBE'
#include <stdio.h>
#include <stddef.h>
#include "spu_context.h"
#define P(f) printf("%-28s %zu\n", #f, offsetof(spu_context, f))
int main(void) {
    P(gpr); P(ls); P(pc); P(image_id); P(policy_mode); P(resident_ovl);
    P(resident_code); P(dcache_id); P(dcache_residency); P(resident_task);
    P(host_depth); P(steps); P(module_img_a00);
    P(lockstep_quantum_ctr); P(lockstep_release_tb); P(dec_start_tb);
    P(ch_wait_cv); P(ch_wait_lock); P(int_enable); P(irq_frame);
    P(list_stall_mask); P(list_stall_stat); P(list_stall_elem_lsa);
    P(list_stall_ea_base); P(list_stall_cmd);
    P(irq_saved); P(irq_resume_pc); P(irq_gpr);
    P(drain_ret_pc); P(stop_request); P(coh_listed);
    return 0;
}
PROBE

base="-D_GNU_SOURCE -O2 -msse4.1 -mssse3 -std=gnu17 -w"
for side in runtime lifted; do
    if [ "$side" = runtime ]; then inc="$SDK/runtime/spu $SDK/include"; else inc="$LIFT_RT $SDK/include"; fi
    if ! $CC $base -I$SDK/include -I$SDK/runtime/platform -I$SDK/libs/system \
             -I$SDK/libs/video -I$SDK/libs/spurs -I$SDK/libs/misc \
             $(for d in $inc; do printf ' -I%s' "$d"; done) \
             "$tmp/probe.c" -o "$tmp/$side" 2>"$tmp/$side.err"; then
        echo "spu-context ABI check: cannot compile probe against the $side header" >&2
        sed -n '1,6p' "$tmp/$side.err" >&2
        exit 1
    fi
    "$tmp/$side" > "$tmp/$side.txt"
done

if diff -u "$tmp/runtime.txt" "$tmp/lifted.txt" > "$tmp/diff.txt"; then
    echo "spu-context ABI check: shared field offsets agree (runtime vs lifted header)"
    exit 0
fi
echo "spu-context ABI check: FAIL - the lifted SPU modules and the runtime disagree on spu_context:" >&2
cat "$tmp/diff.txt" >&2
echo "Only APPEND new fields at the end of spu_context; see docs/freeze-20261103.md." >&2
exit 1

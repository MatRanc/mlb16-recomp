#!/bin/bash
# Windowed multi-scene play session for the play/multiscene build.
#
#   ./play_windowed.sh          MangoHud overlay + CSV, readback WCB=2 (ondemand), cube RTs on
#   ./play_windowed.sh nowcb    readback off (wrong visuals; for comparison)
#   ./play_windowed.sh wcb      PS3RECOMP_RSX_WCB=3 (async readback) instead
#   ./play_windowed.sh nohud    no MangoHud (use if it breaks the window)
#   ./play_windowed.sh wcb nohud
#
# Runs in the foreground. Closing the window (or Ctrl-C) ends the run and the
# screenshot helper. Logs and thumbnails are written under recomp-work/:
#   play_<timestamp>.log           game log with [flip], [walker-prof],
#                                  [c690-stats], [RSX vulkan] window lines
#   play_<timestamp>/              MangoHud CSV, thumb_<epoch-ms>.png, meta.txt
#
# The thumbnail source is the game's own frame dump (PS3RECOMP_VK_DUMP), so it
# works on native Wayland as well as Xwayland. If no dump appears, it falls
# back to an X11 root-window grab when DISPLAY is set.
#
# Defaults: VK_DUMMY_BLACK=1 (no pink missing textures; measure_fast uses it)
# and PS3RECOMP_VK_PRESENT_MODE=fifo (avoids the native-Wayland MAILBOX present
# stall). Both are plain environment variables, so export them before the
# script to override.

set -o pipefail

S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$S/ab/common.sh"
BUILD=$MLB16_BUILD
ELF=$MLB16_EBOOT
EXE="$W/$BUILD/$MLB16_BIN"
DUMP_EVERY=${PLAY_DUMP_EVERY:-30}

HUD=1
WCB=2
for arg in "$@"; do
    case "$arg" in
        nohud) HUD=0 ;;
        wcb)   WCB=3 ;;
        nowcb) WCB=0 ;;
        -h|--help)
            awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
            exit 0 ;;
        *) echo "play_windowed.sh: unknown argument '$arg' (use wcb, nohud)" >&2 ;;
    esac
done

if [ ! -x "$EXE" ]; then
    echo "play_windowed.sh: $EXE not found; build $BUILD first" >&2
    exit 1
fi
if [ ! -e "$ELF" ]; then
    echo "play_windowed.sh: $ELF not found" >&2
    exit 1
fi
# Guard: the lifted SPU modules include a second spu_context.h through
# recomp-work/runtime. A field inserted in the middle there silently breaks the
# c690 jobs a couple of minutes into the boot (see docs/notes/freeze-20261103.md).
# SPU_ABI_CHECK=0 overrides.
if [ -x "$S/check_spu_context_abi.sh" ]; then
    if ! BUILD_CACHE="$W/$BUILD/CMakeCache.txt" "$S/check_spu_context_abi.sh"; then
        echo "play_windowed.sh: spu_context ABI mismatch; refusing to start (SPU_ABI_CHECK=0 to override)" >&2
        exit 1
    fi
fi

TS=$(date +%Y%m%d_%H%M%S)
START_EPOCH=$(date +%s)
LOG="$W/play_$TS.log"
OUTDIR="$W/play_$TS"
mkdir -p "$OUTDIR"

{
    echo "[play] start epoch=$START_EPOCH iso=$(date --iso-8601=seconds) build=$BUILD wcb=$WCB hud=$HUD"
    echo "[play] log=$LOG"
    echo "[play] out=$OUTDIR"
} > "$LOG"
{
    echo "PLAY_START_EPOCH=$START_EPOCH"
    echo "PLAY_TS=$TS"
    echo "BUILD=$BUILD"
    echo "WCB=$WCB"
    echo "HUD=$HUD"
    echo "ARGS=$*"
} > "$OUTDIR/meta.txt"

# The user's normal run.sh environment (no scripted pad) plus the multi-scene
# diagnostics.
export __GL_SYNC_TO_VBLANK=0
export GCM_HANDLER_THREAD=1 GCM_FLIPCB_ONTICK=1 GCM_FLIPCOUNT=1
export PS3RECOMP_VK_GUEST_PROGRAMS=1 PS3RECOMP_VK_WINDOW=1 PS3RECOMP_VK_FULLSCREEN=0
export SPURS_READY_LEVEL=1 SPURS_NO_IDLE_PROBE=1
# Keep a still-ready policy module in the same worker for up to 8 jobs before
# the coordinator round trip (docs/notes/c690-job-gaps-ab.md). SPU_CODE_DMA_MEMO is
# left off: it was measured alone and did not move the frame metrics.
export SPURS_PM_FAST_RESCHED="${SPURS_PM_FAST_RESCHED:-64}"
# native c690 dispatch for MeshJobs0/1 (wids 1,2); docs/notes/c690-native-dispatch.md
export SPURS_PM_NATIVE_DISPATCH="${SPURS_PM_NATIVE_DISPATCH:-6}"
# menu flip fix: tick completes from our own request state (docs/notes/menu-34ms.md, menu-flip-fix.md)
export PS3RECOMP_FLIP_TICK_OWN_STATE="${PS3RECOMP_FLIP_TICK_OWN_STATE:-1}"
# character shaders: translate TXD/DDX/DDY so jersey torsos draw (docs/notes/jersey-transparent.md)
export PS3RECOMP_FP_TXD="${PS3RECOMP_FP_TXD:-1}"
# the user's Better depth of field blur, applied at ELF load (docs/notes/dof-patch.md)
export PS3RECOMP_DOF_PATCH="${PS3RECOMP_DOF_PATCH:-1}"
# write the frame dump off the walker thread (0.5-0.7 s hitches otherwise; docs/notes/present-stalls.md)
export PS3RECOMP_VK_DUMP_ASYNC="${PS3RECOMP_VK_DUMP_ASYNC:-1}"
# shader disk cache survives rebuilds (was 0 hits after every rebuild; docs/notes/boot-gaps.md)
export PS3RECOMP_SHADER_CACHE_STABLE_KEY="${PS3RECOMP_SHADER_CACHE_STABLE_KEY:-1}"
export VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1
export PS3_TITLE=BCUS01089 PS3_VFS_ROOT="$W/vfs" PS3_GAMEDATA_ROOT="$W"
export PS3RECOMP_C690_STATS="${PS3RECOMP_C690_STATS:-0}"  # diagnostic only; on costs ~2.3 real fps at the meter (2026-11-06), set 1 when needed
export PS3RECOMP_WALKER_PROF=1
export PS3RECOMP_VK_PRESENT_STATS=1
export PS3RECOMP_PUMP_MS=8
# Thread affinity: default off. PS3RECOMP_PIN=1 pins the walker to cpu 0, ppu
# main to cpu 1 and the five c690 lanes to the remaining cores; the two-launch
# A/B on 2026-11-04 measured 35.31 fps at PIN=0 vs 30.98 at PIN=1 at the pitch
# meter (docs/notes/pin-and-burst-path.md). Keep the default explicit while still
# allowing an override.
export PS3RECOMP_PIN="${PS3RECOMP_PIN:-0}"
# On-disk cache of translated guest shaders (docs/notes/shader-disk-cache.md): a warm
# run rebuilds first-use pipelines without glslang. Keyed by the guest program
# identity plus the translator version; recomp-work/shader_cache/.
export PS3RECOMP_SHADER_DISK_CACHE="${PS3RECOMP_SHADER_DISK_CACHE:-1}"
# Hitch recorder: keep only frames longer than 250 ms, with the work inside
# them, next to the MangoHud CSV. tools/hitch_table.py turns it into a table.
# Overhead at the meter was below run-to-run noise; the rings stay bounded.
export PS3RECOMP_TIMELINE="${PS3RECOMP_TIMELINE:-$OUTDIR/hitches.pstl}"
export PS3RECOMP_TIMELINE_HITCH_US="${PS3RECOMP_TIMELINE_HITCH_US:-250000}"
export PS3RECOMP_TIMELINE_RING_MB="${PS3RECOMP_TIMELINE_RING_MB:-16}"
export PS3RECOMP_TIMELINE_FLUSH_MB="${PS3RECOMP_TIMELINE_FLUSH_MB:-2}"
# Cheap wins (mlb16-recomp/docs/notes/cheap-wins.md), all individually switchable:
#   SPU_MFC_LOOKUP_CACHE  per-host-thread MFC engine memo (no registry SRW lock)
#   GCM_NOP_SKIP          skip NV4097 NO_OPERATION (0x0100) runs in the walker
#   GCM_REF_IMMEDIATE     publish SET_REFERENCE at execution, not at drain tail
#   PS3RECOMP_RSX_TEX_PIN never evict a texture bound this frame
#   PS3RECOMP_TEX_DIRTY  validate textures by guest-page write generation
# Set any to 0 in the environment to turn it off.
export SPU_MFC_LOOKUP_CACHE="${SPU_MFC_LOOKUP_CACHE:-1}"
export GCM_NOP_SKIP="${GCM_NOP_SKIP:-1}"
export GCM_REF_IMMEDIATE="${GCM_REF_IMMEDIATE:-1}"
export PS3RECOMP_RSX_TEX_PIN="${PS3RECOMP_RSX_TEX_PIN:-1}"
# Validate cached textures from a per-4K-page write generation instead of
# hashing their bytes (docs/notes/texture-dirty.md): -3.4 ms walker drain, tex
# 10.3 -> 5.9 ms/frame at the pitch meter.
export PS3RECOMP_TEX_DIRTY="${PS3RECOMP_TEX_DIRTY:-1}"
# Pipeline answer memo (docs/notes/walker-quick-wins.md): -0.47 ms of walker `pipe`
# at the pitch meter, 85 % hit live, replay gates unchanged. The fetch plan
# memo (PS3RECOMP_FETCH_MEMO / PS3RECOMP_WALKER_FAST) is deliberately NOT on:
# live its reference sets change every frame, it hits ~5 %, and it costs
# ~2.4 ms/frame (see the same doc).
# Library fixes batch 1 (mlb16-recomp/docs/notes/hle-fixes-batch1.md). All are
# default on here and individually switchable; 0 restores the old behaviour.
#   PS3_LWCOND_REAL            real lwcond waits/timeouts/signals, lwm destroy
#   PS3RECOMP_AUDIO_PACE       POSIX audio at the real 5333 us block period
#   PS3RECOMP_FS_WRITE_CHECK   cellFsWrite errors on a short/failed write
#   PS3RECOMP_SPURS_TEARDOWN   Shutdown/Wait drain a workload before Remove
#   PS3RECOMP_GCM_OFFSET_SYNC  guest io/ea offset tables follow map/unmap
export PS3_LWCOND_REAL="${PS3_LWCOND_REAL:-1}"
export PS3RECOMP_AUDIO_PACE="${PS3RECOMP_AUDIO_PACE:-1}"
export PS3RECOMP_FS_WRITE_CHECK="${PS3RECOMP_FS_WRITE_CHECK:-1}"
export PS3RECOMP_SPURS_TEARDOWN="${PS3RECOMP_SPURS_TEARDOWN:-1}"
export PS3RECOMP_GCM_OFFSET_SYNC="${PS3RECOMP_GCM_OFFSET_SYNC:-1}"
export PS3RECOMP_PIPE_MEMO="${PS3RECOMP_PIPE_MEMO:-1}"
# Remap-build recent-reference memo (docs/notes/walker-cuts-2.md): answer a nearby
# duplicate from a 1024-entry direct-mapped memo instead of probing the open
# addressing table. -0.57 ms/frame offline, replay gates unchanged. Default on.
export PS3RECOMP_REMAP_FAST="${PS3RECOMP_REMAP_FAST:-1}"
# Complete a flip whose commands the drain has already executed and whose image
# the present path has queued, from the top of the drain pass. Implemented and
# unit-tested. DEFAULT ON since 2026-11-06: scored on real frames (buffer changes) it is +3.95 fps;
# the old -8 fps were the title's 34 ms timeout re-flips (docs/critical-path.md).
export PS3RECOMP_TICK_IN_DRAIN="${PS3RECOMP_TICK_IN_DRAIN:-1}"  # 2026-11-06: +3.95 real fps at the meter (no timeout re-flips); MangoHud reads lower
# Complete a presented flip when the next frame's fence is published (the
# marker-side design for this title: no in-FIFO flip word exists). DEFAULT OFF:
# the next frame's fence lands after the guest's 2x17 ms deadline, so completing
# there is late, and completing earlier just moves the wait into the buffer ref
# (docs/notes/frame-pacing-floor.md).
export PS3RECOMP_FLIP_FENCE_DONE="${PS3RECOMP_FLIP_FENCE_DONE:-0}"
# Present the window only when the title produced a new frame. Without it the
# frame clock's outer pass re-presents on a flip request, so MangoHud's row
# rate (one per vkQueuePresentKHR) reads above the guest [flip] fps. The
# window pump and input delivery are independent of presents, so this does not
# affect responsiveness; the confirm run measured 42.57 rows/s vs 42.66 [flip]
# fps at the start screen (docs/notes/textures-dummy-census.md).
export PS3RECOMP_VK_PRESENT_NEW_ONLY=1
# Even out the window present cadence: the guest finishes frames in
# bursts (two flips ~1 ms apart, then a ~46 ms gap), so the display saw
# pairs instead of a steady ~34 ms. The pacer holds each present until
# its arrival-cadence slot, at most one frame interval (docs/notes/frame-pacing.md).
export PS3RECOMP_VK_PACE="${PS3RECOMP_VK_PACE:-1}"
# cellGcmSetPrepareFlip must return the display buffer id, as the real SDK
# and RPCS3 do. It used to return CELL_OK (0), so every flip named
# display buffer 0 while the title rendered alternately into 0 and 1;
# a shown frame then repeated one already replaced (the whole-frame
# backward step, docs/notes/backward-frames.md). Default on, 0 restores it.
export PS3RECOMP_PREPARE_FLIP_RET="${PS3RECOMP_PREPARE_FLIP_RET:-1}"
# Disabled texture units read magenta otherwise, which makes the picture pink.
# measure_fast uses the same black dummy.
export VK_DUMMY_BLACK=1
# Native Wayland + NVIDIA: MAILBOX can leave the present thread blocked in
# vkQueuePresentKHR for tens of seconds, dropping every frame no-slot while the
# guest keeps flipping. FIFO is the required present mode and avoids that path
# (Xwayland negotiates FIFO too). Override with PS3RECOMP_VK_PRESENT_MODE=mailbox.
export PS3RECOMP_VK_PRESENT_MODE="${PS3RECOMP_VK_PRESENT_MODE:-fifo}"
# FIFO alone still froze native Wayland (2026-11-02, shown 0 / no-slot); Xwayland does not. Override SDL_VIDEODRIVER=wayland.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
# Thumbnail source: the title's own presented frame, every DUMP_EVERY flips.
export PS3RECOMP_VK_DUMP="$OUTDIR/frame.ppm"
export PS3RECOMP_VK_DUMP_EVERY="$DUMP_EVERY"
# Real controller, not pad.txt.
unset PAD_FILE
unset PS3RECOMP_RSX_WCB PS3RECOMP_RSX_WCB_STATS
# Env PS3RECOMP_RSX_CUBE_RT=0 turns the GPU cube render-target path off.
export PS3RECOMP_RSX_CUBE_RT="${PS3RECOMP_RSX_CUBE_RT:-1}"
# Renderer cuts + early render-target copies (docs/critical-path.md 3b, 2026-11-05: crowded replay view
# 15 -> 31-34 fps, no visual change seen). Live-switchable; set any to 0 to A/B. REMAP_* measured no gain.
# 2026-11-06 merge (user-checked): crowd sub-draw state, swizzled-RT wipe fix, SPU FTZ (bit-exact on the corpus).
export PS3RECOMP_CROWD_FIX="${PS3RECOMP_CROWD_FIX:-1}"
export PS3RECOMP_WIPE_FIX="${PS3RECOMP_WIPE_FIX:-0}"  # 2026-11-06: replaced by ENG_SWZ_RT (the heuristic missed the mound-visit entry)
export PS3RECOMP_SPU_FTZ="${PS3RECOMP_SPU_FTZ:-1}"
export PS3RECOMP_SPU_FLOAT_APPROX="${PS3RECOMP_SPU_FLOAT_APPROX:-1}"  # 2026-11-06: +1.63 real fps at the meter, user visual check OK
export PS3RECOMP_WALKER_WAKE="${PS3RECOMP_WALKER_WAKE:-1}"  # 2026-11-06: +0.82 real fps at the meter
export PS3RECOMP_REMAP_FLAT="${PS3RECOMP_REMAP_FLAT:-1}"  # 2026-11-06: walker -2.8 ms, +0.5 real fps (n.s.), user visual check OK
export PS3RECOMP_ENG_PROG_MEMO="${PS3RECOMP_ENG_PROG_MEMO:-1}"  # 2026-11-06: exact (replay bit-identical), +0.22 real fps at the meter
export PS3RECOMP_ENG_SWZ_RT="${PS3RECOMP_ENG_SWZ_RT:-1}"  # 2026-11-06: swizzled render targets stored in Morton order; fixes the tiled wipes, fps neutral
export PS3RECOMP_VK_DEPTH_BIAS="${PS3RECOMP_VK_DEPTH_BIAS:-1}"  # 2026-11-06: guest polygon offset; fixes the uniform stipple (shadow acne), fps neutral
export PS3RECOMP_VK_PASS_KEEP="${PS3RECOMP_VK_PASS_KEEP:-1}"
export PS3RECOMP_VK_VTX_PRE="${PS3RECOMP_VK_VTX_PRE:-1}"
export PS3RECOMP_VK_DRAW_STATE="${PS3RECOMP_VK_DRAW_STATE:-1}"
export PS3RECOMP_RSX_WCB_PREFETCH="${PS3RECOMP_RSX_WCB_PREFETCH:-1}"
export PS3RECOMP_RSX_WCB_STREAM="${PS3RECOMP_RSX_WCB_STREAM:-1}"
if [ "$WCB" != 0 ]; then
    export PS3RECOMP_RSX_WCB=$WCB
    export PS3RECOMP_RSX_WCB_STATS=60
fi

CMD=("$EXE" "$ELF")
if [ "$HUD" = 1 ]; then
    if command -v mangohud >/dev/null 2>&1; then
        export MANGOHUD_CONFIG="fps,frametime,frame_timing,fps_metrics=avg,0.01,0.001,cpu_stats,gpu_stats,core_load,core_load_change,gpu_load_change,output_folder=$OUTDIR,autostart_log=1,log_duration=0,log_interval=0"
        CMD=(mangohud "$EXE" "$ELF")
        echo "[play] MangoHud overlay on; CSV -> $OUTDIR"
    else
        echo "[play] mangohud not found; running without the overlay (nohud)" >&2
        HUD=0
    fi
fi
if [ "$HUD" = 0 ]; then
    echo "[play] MangoHud off"
fi

# ---- thumbnail helper ------------------------------------------------------
# Child of this script, not a daemon: it ends when the game pipeline ends or
# the trap fires. Prefer the game's PPM frame dump; fall back to X11 root.
shot_once() {
    local epoch tmp
    epoch=$(date +%s%3N)
    tmp="$OUTDIR/.thumb_source"
    if [ -s "$OUTDIR/frame.ppm" ] && cp "$OUTDIR/frame.ppm" "$tmp" 2>/dev/null; then
        if convert "$tmp" -resize 320x180 "$OUTDIR/thumb_${epoch}.png" 2>/dev/null; then
            rm -f "$tmp"
            return 0
        fi
    fi
    rm -f "$tmp"
    if [ -n "$DISPLAY" ] && command -v import >/dev/null 2>&1; then
        if import -window root "$OUTDIR/.root.png" 2>/dev/null &&
           convert "$OUTDIR/.root.png" -resize 320x180 "$OUTDIR/thumb_${epoch}.png" 2>/dev/null; then
            rm -f "$OUTDIR/.root.png"
            return 0
        fi
        rm -f "$OUTDIR/.root.png"
    fi
    return 1
}

(
    while sleep 2; do
        shot_once || true
    done
) &
SHOT_PID=$!

cleanup() {
    kill "$SHOT_PID" 2>/dev/null
    wait "$SHOT_PID" 2>/dev/null
    rm -f "$OUTDIR/.thumb_source" "$OUTDIR/.root.png"
    echo "[play] stopped; log $LOG; data $OUTDIR"
}
trap cleanup EXIT INT TERM

echo "[play] log $LOG"
echo "[play] data $OUTDIR"
echo "[play] fps (from [flip] lines), one line every 2 s:"

# Timestamp every line into the log; show a compact live fps on the console.
{
    "${CMD[@]}" 2>&1 \
    | awk '{ print strftime("%H:%M:%S"), $0; fflush() }' \
    | tee -a "$LOG" \
    | awk '
        match($0, /\[flip\] [0-9]+ at [0-9]+ ms/) {
            s = substr($0, RSTART, RLENGTH)
            split(s, p, " ")
            n = p[2] + 0; t = p[4] + 0
            if (first == 0) { n0 = n; t0 = t; first = 1 }
            if (t - t0 >= 2000) {
                printf("[play] %s fps=%.1f (flips %d in %.1fs)\n", strftime("%H:%M:%S"), (n - n0) * 1000.0 / (t - t0), n - n0, (t - t0) / 1000.0)
                fflush()
                n0 = n; t0 = t
            }
            next
        }
        /error|failed|fatal|abort|assert|\[RSX vulkan\] window:/ { print; fflush() }
        END {
            if (first == 1 && t > t0)
                printf("[play] final fps=%.1f (flips %d in %.1fs)\n", (n - n0) * 1000.0 / (t - t0), n - n0, (t - t0) / 1000.0)
        }'
}
RC=${PIPESTATUS[0]}
echo "[play] game exited rc=$RC"
exit "$RC"

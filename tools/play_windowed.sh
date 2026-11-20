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
USE_MANGOHUD=${USE_MANGOHUD:-0}
WCB=2
for arg in "$@"; do
    case "$arg" in
        nohud)    HUD=0 ;;
        mangohud) USE_MANGOHUD=1 ;;
        wcb)      WCB=3 ;;
        nowcb)    WCB=0 ;;
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

# Game settings shared with the measurement scripts.
. "$S/game_env.sh"
# This script's own outputs and input.
# Hitch recorder: keep only frames longer than 250 ms, with the work inside
# them, next to the MangoHud CSV. tools/hitch_table.py turns it into a table.
# Overhead at the meter was below run-to-run noise; the rings stay bounded.
export PS3RECOMP_TIMELINE="${PS3RECOMP_TIMELINE:-$OUTDIR/hitches.pstl}"
export PS3RECOMP_TIMELINE_HITCH_US="${PS3RECOMP_TIMELINE_HITCH_US:-250000}"
export PS3RECOMP_TIMELINE_RING_MB="${PS3RECOMP_TIMELINE_RING_MB:-16}"
export PS3RECOMP_TIMELINE_FLUSH_MB="${PS3RECOMP_TIMELINE_FLUSH_MB:-2}"
# FIFO alone still froze native Wayland (2026-11-02, shown 0 / no-slot); Xwayland does not. Override SDL_VIDEODRIVER=wayland.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
# Thumbnail source: the title's own presented frame, every DUMP_EVERY flips.
export PS3RECOMP_VK_DUMP="$OUTDIR/frame.ppm"
export PS3RECOMP_VK_DUMP_EVERY="$DUMP_EVERY"
# Real controller, not pad.txt.
unset PAD_FILE
unset PS3RECOMP_RSX_WCB PS3RECOMP_RSX_WCB_STATS
if [ "$WCB" != 0 ]; then
    export PS3RECOMP_RSX_WCB=$WCB
    export PS3RECOMP_RSX_WCB_STATS=60
fi

CMD=("$EXE" "$ELF")
if [ "$HUD" = 1 ]; then
    if [ "$USE_MANGOHUD" = 1 ]; then
        if command -v mangohud >/dev/null 2>&1; then
            export MANGOHUD_CONFIG="fps,frametime,frame_timing,fps_metrics=avg,0.01,0.001,cpu_stats,gpu_stats,core_load,core_load_change,gpu_load_change,output_folder=$OUTDIR,autostart_log=1,log_duration=0,log_interval=0"
            CMD=(mangohud "$EXE" "$ELF")
            echo "[play] External MangoHud overlay on; CSV -> $OUTDIR"
        else
            echo "[play] mangohud not found; falling back to built-in HUD" >&2
            USE_MANGOHUD=0
        fi
    fi
    if [ "$USE_MANGOHUD" = 0 ]; then
        export PS3RECOMP_HUD=1
        export PS3RECOMP_HUD_CSV="$OUTDIR"
        export PS3RECOMP_VK_PRESENT_STATS=1
        echo "[play] Native HUD overlay on (PS3RECOMP_HUD=1); CSV -> $OUTDIR"
    fi
fi
if [ "$HUD" = 0 ]; then
    export PS3RECOMP_HUD=0
    echo "[play] HUD off"
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

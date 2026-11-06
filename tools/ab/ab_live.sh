#!/bin/bash
# In-run A/B at a parked screen: one windowed game run, alternate two arms
# every SEG seconds through the PS3RECOMP_CTL control file, and report the
# per-segment fps and walker own/wait time.
#
#   TAG=aa_pin0 ARM_A="EXP_NO_DRAW=0" ARM_B="EXP_NO_DRAW=0" PIN=0 ./ab_live.sh
#   TAG=no_draw WCB=0 ARM_A="" ARM_B="EXP_NO_DRAW=1" ./ab_live.sh
#   TAG=start FAST=1 START_SCREEN=1 N=4 ARM_A="" ARM_B="EXP_FAST_WAKE=1" PIN=1 ./ab_live.sh
#
# Env:
#   ARM_A/ARM_B  newline-separated NAME=value lines (the control file)
#   BOOT_ARM     control file used from launch until the first switch
#                (default ARM_A). Needed when an arm must not be live during
#                boot: EXP_NO_DRAW=1 makes the boot screen black and the nav
#                cannot see it, so pass BOOT_ARM="EXP_NO_DRAW=0".
#   N            segments per arm (default 6)
#   SEG          seconds per segment (default 10)
#   DROP         seconds discarded after each switch (default 2)
#   WCB          PS3RECOMP_RSX_WCB value (default 2 = ondemand, the normal
#                visual configuration; arms that must not read back -- e.g.
#                EXP_NO_DRAW -- set WCB=0 explicitly)
#   PIN          1 => PS3RECOMP_PIN=1
#   START_SCREEN 1 => park at the press-start screen instead of navigating
#   BOOT         seconds before nav (default 40)
#   LIMIT        hard timeout for the game process (default 600)
#   VIDEODRIVER  SDL video driver (default x11: docs/window-freeze-fix.md's
#                fallback, because native-Wayland FIFO froze in the first run)
#   METER        1 (default) => after the pitch screen, press X once and
#                verify the pitch meter from the frame dump
#   METER_CAL    1 => if tools/ab/ref/pitch_meter.png is missing,
#                save calibration frames to the run folder and continue
#   METER_REF/METER_CROP/METER_THRESH/METER_WAIT see meter_lib.sh
#   AUTO_SETTLE  1 waits until the pitch walker mean is <= SETTLE_WALKER ms
#                for 8 s; 0 (default) waits a fixed SETTLE seconds after the
#                meter is up
#   SETTLE       fixed settle seconds when AUTO_SETTLE=0 (default 10)
#   SETTLE_WALKER  idle threshold in ms (default 10)
#   SCENE_SHOTS  1 (default) save a 640x360 thumbnail every 2 s into the run
#                folder and a contact sheet with the A/B bands
#   ARMS         multi-arm rotation instead of ARM_A/ARM_B:
#                'A=KEY=v;KEY=v|B=KEY=v|C=KEY=v' (labels are single letters).
#                N segments per arm, rotating A,B,C,A,...
#   REPORT       0 => skip ab_live_report.py (it only understands A/B marks)
#   LOAD_SAMPLE  1 => after nav reaches the match load, run
#                "top -H -b -d 1 -n LOAD_SAMPLE_SECS -p <game pid>" into
#                <out>/load_<label>_<epoch>.top. The pid is the real game
#                process (comm == MyGameRecomp), not the timeout/mangohud
#                wrapper.
#   LOAD_SAMPLE_SECS  seconds per sample (default 30)
#   LOAD_TOGGLE_ARM  if set, the load sample is taken in two halves of
#                LOAD_SAMPLE_SECS: the boot arm first, then this control-file
#                body, so one launch gives a baseline and a switched loading
#                CPU table. No mark is written for the toggle.
#
# Output: recomp-work/ab_live_<TAG>_<ts>.log, .../MangoHud CSV,
#         recomp-work/ab_live_<TAG>_<ts>_ft.png, ..._segments.txt,
#         .../scene_<epoch-ms>.png, ..._scene_sheet.png
# Exits non-zero on a preflight failure or if the screen was not reached.

set -u
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$S/common.sh"
BUILD=$MLB16_BUILD
EXE="$W/$BUILD/$MLB16_BIN"
ELF=$MLB16_EBOOT
REF_DIR=${REF_DIR:-$S/ref}

TAG=${TAG:-live}
ARM_A=${ARM_A-}
ARM_B=${ARM_B-}
ARMS=${ARMS:-}
BOOT_ARM=${BOOT_ARM-}
REPORT=${REPORT:-1}
DRY_RUN=${DRY_RUN:-0}
LOAD_SAMPLE=${LOAD_SAMPLE:-0}
LOAD_SAMPLE_SECS=${LOAD_SAMPLE_SECS:-30}
LOAD_TOGGLE_ARM=${LOAD_TOGGLE_ARM:-}
N=${N:-6}
SEG=${SEG:-10}
DROP=${DROP:-2}
WCB=${WCB:-2}
PIN=${PIN:-0}
START_SCREEN=${START_SCREEN:-0}
BOOT=${BOOT:-40}
LIMIT=${LIMIT:-600}
VIDEODRIVER=${VIDEODRIVER:-x11}
METER=${METER:-1}
METER_CAL=${METER_CAL:-0}
AUTO_SETTLE=${AUTO_SETTLE:-0}
SETTLE=${SETTLE:-10}
SETTLE_WALKER=${SETTLE_WALKER:-20}
SCENE_SHOTS=${SCENE_SHOTS:-1}
SKIP_CINE=${SKIP_CINE:-1}
CINE_GAP=${CINE_GAP:-1.6}
DUMMY_BLACK=${DUMMY_BLACK:-1}
IGNORE_MAGENTA=${IGNORE_MAGENTA:-0}
# Mean-abs-diff threshold for the pitch_wait screen. sqrt: the cube render
# targets change the players/crowd inside the match region; the calibration
# without them (25) sits below the ~30 diff with CUBE_RT=1, so the harness
# never confirms the pitch. 35 is the documented value (readback-cost.md,
# walker-cuts-2.md) and is now the default.
PITCH_THRESH=${PITCH_THRESH:-35.0}
CAPTURE_PREFIX=${CAPTURE_PREFIX:-}
# PS3RECOMP_C690_TAIL_CAPTURE's trigger. It must be its own file: the walker
# capture below consumes and deletes CAPTURE_PREFIX.trigger, so sharing it
# loses the tail corpus before the 200 ms poll can see it. ab_live removes a
# stale copy at launch and touches it only once the pitch meter is confirmed.
TAIL_TRIGGER=${PS3RECOMP_C690_TAIL_TRIGGER:-}
FINAL_SHOT=${FINAL_SHOT:-}
source "$S/meter_lib.sh"

TS=$(date +%Y%m%d_%H%M%S)
OUT=$W/ab_live_${TAG}_${TS}
LOG=$OUT.log
MARKS=$OUT.marks
CTL=$OUT.ctl
SEGMENTS=$OUT.segments.txt
mkdir -p "$OUT"

log() { echo "$(date +%H:%M:%S) [ab_live] $*" | tee -a "$LOG"; }

# ---- arms ------------------------------------------------------------------
# Default A/B; ARMS='A=..|B=..|C=..' rotates any number of single-letter arms.
ARM_LABELS=()
ARM_BODIES=()
if [ -n "$ARMS" ]; then
    IFS='|' read -r -a _arm_specs <<< "$ARMS"
    for _spec in "${_arm_specs[@]}"; do
        [ -n "$_spec" ] || continue
        ARM_LABELS+=("${_spec%%=*}")
        ARM_BODIES+=("${_spec#*=}")
    done
fi
if [ ${#ARM_LABELS[@]} -eq 0 ]; then
    ARM_LABELS=(A B)
    ARM_BODIES=("$ARM_A" "$ARM_B")
fi
NARM=${#ARM_LABELS[@]}
[ -n "$BOOT_ARM" ] || BOOT_ARM=${ARM_BODIES[0]}

# ---- preflight -------------------------------------------------------------
if pgrep -x MyGameRecomp >/dev/null; then
    log "the user's game is running; waiting (max 2 h)"
    for _ in $(seq 1 120); do
        sleep 60
        pgrep -x MyGameRecomp >/dev/null || break
    done
    if pgrep -x MyGameRecomp >/dev/null; then
        log "STOP: MyGameRecomp still running after 2 h"; exit 3
    fi
fi
gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
if [ "$gov" != performance ]; then
    log "STOP: CPU governor is '$gov', not performance"; exit 4
fi
if [ ! -x "$EXE" ]; then log "STOP: $EXE missing"; exit 5; fi
if [ ! -d "$W/vfs" ]; then log "STOP: $W/vfs missing"; exit 5; fi
if ! ls "$MLB16_ISO_MOUNT" >/dev/null 2>&1; then
    log "STOP: the game ISO is not mounted ($MLB16_ISO_MOUNT)"; exit 5
fi
# The lifted SPU modules are compiled with a second copy of spu_context.h
# (recomp-work/runtime -> ...); a field inserted in the middle shifts the
# dispatch state the lifted code reads. Fail before the boot, not after three
# c690 jobs. See docs/freeze-20261103.md.
if [ -x "$S/check_spu_context_abi.sh" ]; then
    if ! BUILD_CACHE="$W/$BUILD/CMakeCache.txt" "$S/check_spu_context_abi.sh" >>"$LOG" 2>&1; then
        log "STOP: spu_context ABI mismatch between the runtime and the lifted SPU modules"
        tail -25 "$LOG" | sed 's/^/  /'
        exit 7
    fi
fi

# ---- control file ----------------------------------------------------------
write_arm() {
    # Arms are written as NAME=value lines. Accept ';' as a separator too
    # (ARM_A/ARM_B/ARMS command lines are easier to quote that way); no arm
    # uses a semicolon inside a value.
    local body="$1" tmp="$CTL.tmp"
    printf '%s\n' "$body" | tr ';' '\n' > "$tmp"
    mv -f "$tmp" "$CTL"
}
last_flip_at() {
    local t=""
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        t=$(grep -a '\[flip\]' "$LOG" 2>/dev/null | tail -1 |
            sed -n 's/.* at \([0-9][0-9]*\) ms.*/\1/p')
        [ -n "$t" ] && break
        sleep 0.2
    done
    echo "${t:-0}"
}
switch_to() {   # $1 arm label, $2 body
    write_arm "$2"
    sleep 0.15
    local at; at=$(last_flip_at)
    local wall; wall=$(date +%s%3N)
    printf '%s %s %s\n' "$1" "$at" "$wall" >> "$MARKS"
    log "switch $1 at flip-ms $at (wall $wall)"
}

: > "$MARKS"
write_arm "$BOOT_ARM"       # boot with this; ARM_A takes over at the 1st switch

# The control file remembers a key once it has named it (ps3_live_ctl.c): an
# arm that leaves a key out does NOT revert it to the environment value, it
# keeps the last value the file set. Every arm must therefore name every key
# the other arm names, even just to hold it at the default. Warn loudly --
# the report's live-ctl self-check would fail, but only after the run.
keys_of() { printf '%s\n' "$1" | tr ';' '\n' |
            sed -n 's/^\([A-Za-z0-9_]*\)=.*/\1/p' | awk 'NF' | sort -u; }
allkeys=""
for _body in "${ARM_BODIES[@]}"; do
    allkeys+=$(keys_of "$_body")$'\n'
done
allkeys=$(printf '%s' "$allkeys" | awk 'NF' | sort -u)
for _i in "${!ARM_BODIES[@]}"; do
    _have=$(keys_of "${ARM_BODIES[$_i]}")
    _miss=$(comm -23 <(printf '%s\n' "$allkeys") \
                     <(printf '%s\n' "$_have" | awk 'NF'))
    if [ -n "$_miss" ]; then
        log "WARNING: arm ${ARM_LABELS[$_i]} does not name key(s) another arm names: $(printf '%s' "$_miss" | tr '\n' ' ')"
        log "WARNING: the control file does not revert them; those keys keep the last value set."
    fi
done

# ---- dry run ---------------------------------------------------------------
# Parse the arms, write and print each control file exactly as the run would,
# then stop before touching the game. Used to check separator/quoting bugs and
# the expected number of [pin] layout applies per switch without a launch.
if [ "$DRY_RUN" = 1 ]; then
    log "DRY_RUN: no launch; boot and switch control files only"
    log "DRY_RUN boot ${ARM_LABELS[0]:-BOOT}:"
    sed 's/^/DRYCTL /' "$CTL" | tee -a "$LOG"
    _total=$((NARM * N))
    for _i in $(seq 0 $((_total - 1))); do
        _ai=$((_i % NARM))
        write_arm "${ARM_BODIES[$_ai]}"
        log "DRY_RUN switch ${_i} ${ARM_LABELS[$_ai]}:"
        sed 's/^/DRYCTL /' "$CTL" | tee -a "$LOG"
    done
    log "DRY_RUN done; control file is the last arm"
    exit 0
fi

# ---- game environment ------------------------------------------------------
export __GL_SYNC_TO_VBLANK=0
export PAD_FILE=$W/pad.txt
export GCM_HANDLER_THREAD=1 GCM_FLIPCB_ONTICK=1 GCM_FLIPCOUNT=1
export PS3RECOMP_VK_GUEST_PROGRAMS=1 PS3RECOMP_VK_WINDOW=1 PS3RECOMP_VK_FULLSCREEN=0
export PS3RECOMP_VK_PRESENT_MODE=fifo PS3RECOMP_VK_PRESENT_STATS=1
# Present the window only when the guest produced a new frame. Without it the
# frame clock re-presents on SetPrepareFlip too, which doubled MangoHud's
# row rate over [flip] and put two presents ~1 ms apart (docs/frame-pacing.md).
export PS3RECOMP_VK_PRESENT_NEW_ONLY="${PS3RECOMP_VK_PRESENT_NEW_ONLY:-1}"
export SDL_VIDEODRIVER=$VIDEODRIVER
export PS3RECOMP_PUMP_MS=8 PS3RECOMP_WALKER_PROF=1
export PS3RECOMP_PIPE_TIMING=1
# Per-flip lifecycle log: frame time is computed from `fin` (the walker
# executed the flip), not from the 10-flip [flip] counter.
export PS3RECOMP_FLIP_LOG="${PS3RECOMP_FLIP_LOG:-1}"
export PS3RECOMP_C690_STATS="${PS3RECOMP_C690_STATS:-1}"
# Keep the standard play_windowed.sh configuration (docs/cheap-wins.md,
# docs/texture-dirty.md, docs/walker-quick-wins.md): otherwise an ab_live run
# would be measured on a different baseline than the play sessions.
export SPU_MFC_LOOKUP_CACHE="${SPU_MFC_LOOKUP_CACHE:-1}"
export GCM_NOP_SKIP="${GCM_NOP_SKIP:-1}"
export GCM_REF_IMMEDIATE="${GCM_REF_IMMEDIATE:-1}"
export PS3RECOMP_RSX_TEX_PIN="${PS3RECOMP_RSX_TEX_PIN:-1}"
export PS3RECOMP_TEX_DIRTY="${PS3RECOMP_TEX_DIRTY:-1}"
export PS3RECOMP_PIPE_MEMO="${PS3RECOMP_PIPE_MEMO:-1}"
export PS3RECOMP_REMAP_FAST="${PS3RECOMP_REMAP_FAST:-1}"
export PS3RECOMP_TICK_IN_DRAIN="${PS3RECOMP_TICK_IN_DRAIN:-0}"
export PS3RECOMP_FLIP_FENCE_DONE="${PS3RECOMP_FLIP_FENCE_DONE:-0}"
# play_windowed.sh's load-time and diagnostic defaults (the meter is unaffected
# by the shader disk cache; the hitch recorder only stores frames > 250 ms).
export PS3RECOMP_SHADER_DISK_CACHE="${PS3RECOMP_SHADER_DISK_CACHE:-1}"
export PS3RECOMP_TIMELINE="${PS3RECOMP_TIMELINE:-$OUT/hitches.pstl}"
export PS3RECOMP_TIMELINE_HITCH_US="${PS3RECOMP_TIMELINE_HITCH_US:-250000}"
export PS3RECOMP_TIMELINE_RING_MB="${PS3RECOMP_TIMELINE_RING_MB:-16}"
export PS3RECOMP_TIMELINE_FLUSH_MB="${PS3RECOMP_TIMELINE_FLUSH_MB:-2}"
export PS3RECOMP_PREPARE_FLIP_RET="${PS3RECOMP_PREPARE_FLIP_RET:-1}"
# Library fixes batch 1 (docs/hle-fixes-batch1.md). The A/B arms normally
# override these through the control file; these keep a plain run on the same
# tested configuration.
export PS3_LWCOND_REAL="${PS3_LWCOND_REAL:-1}"
export PS3RECOMP_AUDIO_PACE="${PS3RECOMP_AUDIO_PACE:-1}"
export PS3RECOMP_FS_WRITE_CHECK="${PS3RECOMP_FS_WRITE_CHECK:-1}"
export PS3RECOMP_SPURS_TEARDOWN="${PS3RECOMP_SPURS_TEARDOWN:-1}"
export PS3RECOMP_GCM_OFFSET_SYNC="${PS3RECOMP_GCM_OFFSET_SYNC:-1}"
export SPURS_PM_FAST_RESCHED="${SPURS_PM_FAST_RESCHED:-64}"
# native c690 dispatch for MeshJobs0/1 (wids 1,2); docs/c690-native-dispatch.md
export SPURS_PM_NATIVE_DISPATCH="${SPURS_PM_NATIVE_DISPATCH:-6}"
# menu flip fix: tick completes from our own request state (docs/menu-34ms.md, menu-flip-fix.md)
export PS3RECOMP_FLIP_TICK_OWN_STATE="${PS3RECOMP_FLIP_TICK_OWN_STATE:-1}"
# character shaders: translate TXD/DDX/DDY so jersey torsos draw (docs/jersey-transparent.md)
export PS3RECOMP_FP_TXD="${PS3RECOMP_FP_TXD:-1}"
# the user's Better depth of field blur, applied at ELF load (docs/dof-patch.md)
export PS3RECOMP_DOF_PATCH="${PS3RECOMP_DOF_PATCH:-1}"
# write the frame dump off the walker thread (0.5-0.7 s hitches otherwise; docs/present-stalls.md)
export PS3RECOMP_VK_DUMP_ASYNC="${PS3RECOMP_VK_DUMP_ASYNC:-1}"
# shader disk cache survives rebuilds (was 0 hits after every rebuild; docs/boot-gaps.md)
export PS3RECOMP_SHADER_CACHE_STABLE_KEY="${PS3RECOMP_SHADER_CACHE_STABLE_KEY:-1}"
export PS3RECOMP_RSX_CUBE_RT="${PS3RECOMP_RSX_CUBE_RT:-1}"
export PS3RECOMP_VK_PACE="${PS3RECOMP_VK_PACE:-1}"
export VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1
if [ "$DUMMY_BLACK" = 1 ]; then export VK_DUMMY_BLACK=1; else unset VK_DUMMY_BLACK; fi
[ "$IGNORE_MAGENTA" = 1 ] && export NAV_IGNORE_MAGENTA=1 PITCH_IGNORE_MAGENTA=1 IGNORE_MAGENTA=1
[ -n "$CAPTURE_PREFIX" ] && export PS3RECOMP_WALKER_CAPTURE=$CAPTURE_PREFIX
if [ -n "$TAIL_TRIGGER" ]; then
    if [ "$TAIL_TRIGGER" = "$CAPTURE_PREFIX.trigger" ]; then
        log "STOP: the c690 tail trigger must not be CAPTURE_PREFIX.trigger"
        exit 8
    fi
    export PS3RECOMP_C690_TAIL_TRIGGER=$TAIL_TRIGGER
    rm -f "$TAIL_TRIGGER"     # a stale file from an earlier run
fi
export SPURS_READY_LEVEL=1 SPURS_NO_IDLE_PROBE=1
export PS3_TITLE=BCUS01089 PS3_VFS_ROOT=$W/vfs PS3_GAMEDATA_ROOT=$W
export PS3RECOMP_VK_DUMP=$W/frame.ppm PS3RECOMP_VK_DUMP_EVERY=10
export PS3RECOMP_CTL=$CTL
[ "$PIN" = 1 ] && export PS3RECOMP_PIN=1
[ "$WCB" != 0 ] && export PS3RECOMP_RSX_WCB=$WCB
rm -f "$W/frame.ppm"

: > "$W/pad.txt"
: > "$LOG"
log "start tag=$TAG build=$BUILD N=$N seg=${SEG}s drop=${DROP}s wcb=$WCB pin=$PIN start_screen=$START_SCREEN videodriver=$VIDEODRIVER"
log "boot: $(printf '%s' "$BOOT_ARM" | tr '\n' ';')"
for _i in "${!ARM_LABELS[@]}"; do
    log "${ARM_LABELS[$_i]}: $(printf '%s' "${ARM_BODIES[$_i]}" | tr '\n' ';')"
done

HUD=0
if command -v mangohud >/dev/null 2>&1; then
    HUD=1
    export MANGOHUD_CONFIG="fps,frametime,frame_timing,output_folder=$OUT,autostart_log=1,log_duration=0,log_interval=0"
    CMD=(mangohud "$EXE" "$ELF")
else
    CMD=("$EXE" "$ELF")
fi
log "mangohud=$HUD"

# ---- scene thumbnails ------------------------------------------------------
SHEET_DONE=0
make_scene_sheet() {
    [ "$SCENE_SHOTS" = 1 ] || return 0
    [ "$SHEET_DONE" = 1 ] && return 0
    ls "$OUT"/scene_*.png >/dev/null 2>&1 || return 0
    python3 "$S/scene_sheet.py" "$OUT" "$MARKS" "$OUT/scene_sheet.png" \
        >> "$LOG" 2>&1 || true
    SHEET_DONE=1
}
SCENE_PID=""
scene_loop() {
    while sleep 2; do
        frame_shot "$OUT/scene_$(date +%s%3N).png" || true
    done
}
if [ "$SCENE_SHOTS" = 1 ]; then
    scene_loop &
    SCENE_PID=$!
    log "scene thumbnails every 2 s -> $OUT"
fi

# ---- per-thread CPU sampling during the match load -------------------------
# The game runs under `timeout ... mangohud ...`. pgrep -f matches every
# wrapper too, and the runtime renames the boot thread, so the process comm is
# "ppu main" and not the executable name. The real process is the one whose
# argv[0] is the game executable (mangohud `exec env ... "$@"` leaves that
# argv). Fall back to comm == the exe basename if argv is unreadable.
LOAD_PIDS=()
find_game_pid() {
    local p a0 c
    for p in $(pgrep -f 'MyGameRecomp' 2>/dev/null); do
        a0=$(tr '\0' '\n' < "/proc/$p/cmdline" 2>/dev/null | head -1)
        if [ "$a0" = "$EXE" ]; then echo "$p"; return 0; fi
        c=$(cat "/proc/$p/comm" 2>/dev/null || true)
        if [ "$c" = "$(basename "$EXE")" ]; then echo "$p"; return 0; fi
    done
    return 1
}
start_load_sample() {
    local label=$1 secs=$2 pid out
    [ "$LOAD_SAMPLE" = 1 ] || return 0
    pid=$(find_game_pid || true)
    if [ -z "${pid:-}" ]; then
        log "load-sample $label: no game process; pgrep -f list: $(pgrep -af 'MyGameRecomp' 2>/dev/null | tr '\n' ';')"
        return 0
    fi
    out="$OUT/load_${label}_$(date +%s).top"
    log "load-sample $label: top -H -b -d 1 -n $secs -p $pid -> $out"
    ( LC_ALL=C top -H -b -d 1 -n "$secs" -w 200 -p "$pid" > "$out" 2>&1
      log "load-sample $label done ($(grep -cE '^[[:space:]]*[0-9]+[[:space:]]' "$out" 2>/dev/null || echo 0) thread rows)" ) &
    LOAD_PIDS+=("$!")
}

# ---- launch ----------------------------------------------------------------
T0=$(date +%s)
setsid timeout -s KILL "$LIMIT" "${CMD[@]}" \
    > >(awk '{ print strftime("%H:%M:%S"), $0; fflush() }' | tee -a "$LOG") 2>&1 &
PGID=$!
cleanup() {
    if [ -n "$SCENE_PID" ]; then
        kill "$SCENE_PID" 2>/dev/null
        wait "$SCENE_PID" 2>/dev/null
        SCENE_PID=""
    fi
    for _p in "${LOAD_PIDS[@]:-}"; do
        [ -n "$_p" ] || continue
        kill "$_p" 2>/dev/null
        wait "$_p" 2>/dev/null
    done
    LOAD_PIDS=()
    kill -TERM -"$PGID" 2>/dev/null
    sleep 2
    kill -KILL -"$PGID" 2>/dev/null
    wait "$PGID" 2>/dev/null
    make_scene_sheet
}
trap cleanup EXIT INT TERM

# ---- reach the screen ------------------------------------------------------
if [ "$START_SCREEN" = 1 ]; then
    log "waiting ${BOOT}s for boot, then the press-start screen"
    sleep "$BOOT"
    ok=0
    for _ in $(seq 1 60); do
        sleep 1
        recent=$(grep -a '\[flip\]' "$LOG" 2>/dev/null | tail -20)
        nflips=$(printf '%s\n' "$recent" | grep -c '\[flip\]')
        draws=$(grep -a '\[walker-prof\]' "$LOG" 2>/dev/null | tail -1 |
                sed -n 's/.*draws=\([0-9.][0-9.]*\).*/\1/p')
        # ~3 draws/frame and >40 fps is the title/press-start screen.
        if [ "${nflips:-0}" -ge 5 ] && [ -n "${draws:-}" ] &&
           awk -v d="$draws" 'BEGIN{exit !(d <= 10)}'; then
            ok=1; break
        fi
    done
    [ "$ok" = 1 ] || { log "STOP: start screen not detected"; exit 6; }
    log "start screen reached"
else
    log "waiting ${BOOT}s for boot, then navigating to the pitch"
    sleep "$BOOT"
    if ! NAV_HOME=1 python3 "$S/nav3.py" >> "$LOG" 2>&1; then
        log "STOP: nav3 failed"; exit 6
    fi
    # nav3 returns when the options screen is confirmed: the match is now
    # loading. Sample per-thread CPU here, before the pitch/meter exists.
    if [ -n "$LOAD_TOGGLE_ARM" ]; then
        start_load_sample load_a "$LOAD_SAMPLE_SECS"
        sleep "$LOAD_SAMPLE_SECS"
        write_arm "$LOAD_TOGGLE_ARM"
        log "load-sample toggle -> $(printf '%s' "$LOAD_TOGGLE_ARM" | tr '\n' ';')"
        sleep 0.5
        start_load_sample load_b "$LOAD_SAMPLE_SECS"
    else
        start_load_sample load "$LOAD_SAMPLE_SECS"
    fi
    cine_rc=1
    meter_up=0
    if [ "$SKIP_CINE" = 1 ]; then
        log "skipping pregame cinematics with X (SKIP_CINE=1, gap ${CINE_GAP}s)"
        SKIP_CINE=1 CINE_GAP=$CINE_GAP python3 "$S/nav3.py" --to-pitch >> "$LOG" 2>&1
        cine_rc=$?
        if [ "$cine_rc" = 10 ]; then
            meter_up=1
            log "cinematic skip found the meter already up after $(( $(date +%s) - T0 )) s (no second X)"
        elif [ "$cine_rc" = 0 ]; then
            log "cinematics skipped after $(( $(date +%s) - T0 )) s; waiting for the pitch menu"
        else
            log "WARNING: --to-pitch did not confirm the pitch (rc=$cine_rc); polling"
        fi
    fi
    # The exact pitch-menu match is the safe trigger for the one X: the loose
    # pitch-camera match above can fire on a transition frame, and an X there
    # is consumed by the cinematic instead of selecting the pitch. Never
    # press while the meter is the only match.
    if [ "$meter_up" != 1 ]; then
        ok=0
        for _ in $(seq 1 "${PITCH_WAIT_S:-180}"); do
            sleep 1
            "$S/shot.sh" "ab_live_${TAG}_${TS}_nav" >/dev/null 2>&1 || true
            if python3 "$S/same.py" "$S/ab_live_${TAG}_${TS}_nav.png" \
                    "$REF_DIR/pitch_wait.png" 380,100,500,200 "$PITCH_THRESH" >/dev/null 2>&1; then
                ok=1; break
            fi
            # The cinematic skip may have opened the meter already: an X
            # pressed on the pitch camera selects the pitch. Never press a
            # second one; recognise the meter here instead of waiting for a
            # pitch_wait frame that will not come (warm-cache nav miss).
            if meter_matches "$S/ab_live_${TAG}_${TS}_nav.png"; then
                meter_up=1; ok=1
                log "pitch meter already up during the pitch poll; not pressing X"
                break
            fi
        done
        [ "$ok" = 1 ] || { log "STOP: pitch screen not reached"; exit 6; }
        log "pitch reached after $(( $(date +%s) - T0 )) s from launch"
    fi

    # The idle pitch screen cuts to b-roll after ~30 s. Press X exactly once:
    # it selects the pitch and shows the meter in its first state, where the
    # game waits indefinitely. Never press it again (that starts the throw).
    if [ "$METER" = 1 ]; then
        if [ "$meter_up" = 1 ]; then
            log "meter already up from the cinematic skip; not pressing X again"
        else
            sleep "${PRESS_SETTLE:-1}"
            log "pressing X once for the pitch meter"
            meter_press_once
        fi
        if [ -f "$meter_ref" ]; then
            if mshot=$(meter_wait_ready "$OUT/meter_$(date +%s%3N)" \
                                       "${METER_WAIT:-20}"); then
                log "pitch meter confirmed ($mshot) after $(( $(date +%s) - T0 )) s from launch"
            else
                log "STOP: pitch meter not detected after X"
                exit 6
            fi
        elif [ "$METER_CAL" = 1 ]; then
            log "WARNING: $meter_ref missing; calibration mode, saving frames"
            slept=0
            for d in 1 2 4 8 16 30; do
                sleep $((d - slept)); slept=$d
                frame_shot "$OUT/meter_cal_plus${d}s.png" || true
            done
            [ -f "$OUT/meter_cal_plus2s.png" ] && \
                cp "$OUT/meter_cal_plus2s.png" "$REF_DIR/pitch_meter_cal.png"
            log "calibration frames saved in $OUT"
        else
            log "STOP: $meter_ref missing (run once with METER_CAL=1)"
            exit 6
        fi
        if [ -n "$CAPTURE_PREFIX" ]; then
            rm -f "$CAPTURE_PREFIX.missing" "$CAPTURE_PREFIX.fifo" "$CAPTURE_PREFIX.state" "$CAPTURE_PREFIX.mem"
            touch "$CAPTURE_PREFIX.trigger"
            log "walker capture armed at the meter ($CAPTURE_PREFIX)"
            sleep 2
        fi
        # The tail capture has its own trigger, touched only after the meter
        # is confirmed. Never touch it earlier: it arms a 268 KiB-per-command
        # spool. It is not deleted here or by the walker capture.
        if [ -n "$TAIL_TRIGGER" ]; then
            touch "$TAIL_TRIGGER"
            log "c690 tail capture armed at the meter ($TAIL_TRIGGER)"
        fi
    fi
fi

# Settle, then alternate. The first switch starts the first segment.
# Default is a fixed wait after the meter is up (the meter scene is stable
# once its frames are warm). AUTO_SETTLE=1 restores the old walker-mean wait.
if [ "$AUTO_SETTLE" = 1 ] && [ "$START_SCREEN" != 1 ]; then
    log "auto-settle: waiting for walker mean <= ${SETTLE_WALKER} ms for 8 s"
    good=0
    for _ in $(seq 1 180); do
        sleep 1
        w=$(grep -a '\[walker-prof\]' "$LOG" 2>/dev/null | tail -20 |
            sed -n 's/.*walker=\([0-9.][0-9.]*\)ms.*/\1/p' |
            awk '{s+=$1; n++} END { if (n) printf "%.2f", s/n }')
        if [ -n "$w" ] && awk -v w="$w" -v t="$SETTLE_WALKER" \
                'BEGIN { exit !(w <= t) }'; then
            good=$((good + 1))
        else
            good=0
        fi
        if [ "$good" -ge 8 ]; then
            log "idle pitch reached (walker mean ${w} ms for 8 s)"
            break
        fi
    done
    [ "$good" -ge 8 ] || log "WARNING: idle pitch not reached; starting anyway"
else
    if [ "$METER" = 1 ] && [ "$START_SCREEN" != 1 ]; then
        log "settling ${SETTLE}s after the X press (meter scene)"
    else
        log "settling ${SETTLE}s before the first segment"
    fi
    sleep "$SETTLE"
fi

total=$((NARM * N))
for i in $(seq 0 $((total - 1))); do
    ai=$((i % NARM))
    arm=${ARM_LABELS[$ai]}; body=${ARM_BODIES[$ai]}
    switch_to "$arm" "$body"
    sleep "$((SEG - 2))"
    # live hang guard: the arm must keep flipping
    a=$(last_flip_at); sleep 2; b=$(last_flip_at)
    # A jump-park resync can drop one frame's commands; give it 3 s to
    # recover before calling the arm wedged.
    if [ "$a" = "$b" ]; then sleep 3; b=$(last_flip_at); fi
    if [ "$a" = "$b" ] || [ $((b - a)) -lt 10 ]; then
        log "STOP: no flips during segment $i ($arm): at $a -> $b"
        exit 7
    fi
done
log "segments done"

if [ -n "$FINAL_SHOT" ]; then
    sleep 1
    frame_shot "$FINAL_SHOT" && log "final meter frame -> $FINAL_SHOT"
fi

cleanup
trap - EXIT INT TERM
sleep 1

# ---- report ----------------------------------------------------------------
CSV=$(ls -t "$OUT"/*.csv 2>/dev/null | head -1)
CSV_EPOCH=""
if [ -n "$CSV" ]; then
    base=$(basename "$CSV" .csv)
    human=$(echo "$base" | sed -E 's/^.*_([0-9]{4})-([0-9]{2})-([0-9]{2})_([0-9]{2})-([0-9]{2})-([0-9]{2})$/\1-\2-\3 \4:\5:\6/')
    CSV_EPOCH=$(date -d "$human" +%s 2>/dev/null || true)
fi

if [ "$REPORT" = 1 ] && [ "$NARM" -le 2 ]; then
    python3 "$S/ab_live_report.py" --log "$LOG" --marks "$MARKS" \
        --seg "$SEG" --drop "$DROP" --out "$OUT" \
        ${CSV:+"--csv" "$CSV"} ${CSV_EPOCH:+"--csv-epoch" "$CSV_EPOCH"} \
        2>&1 | tee -a "$LOG"
else
    log "ab_live_report.py skipped (REPORT=$REPORT, arms=$NARM); use pin_ab.py"
fi

log "done; log=$LOG"
exit 0

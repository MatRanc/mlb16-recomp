#!/bin/bash
# Shared pitch-meter helpers for ab_live.sh / measure_fast.sh.
#
# The pitch screen is not stationary: after ~30 s idle it cuts to b-roll.
# Pressing X (CROSS) exactly once at the pitch selects the pitch and shows the
# meter in its first state, where the game waits indefinitely. Never press it a
# second time (that starts the throw).
#
# Env (read by the callers):
#   METER        1 (default) => end the nav on the pitch meter
#   METER_CAL    1 => when the reference is missing, save calibration frames
#                    and continue instead of stopping
#   METER_REF    reference PNG (default $REF_DIR/pitch_meter.png)
#   METER_CROP   "x0,y0,x1,y1" in 640x360 coords (default below)
#   METER_THRESH mean abs diff threshold (default 30.0)
#   METER_WAIT   seconds to wait for the meter after the press (default 20)

S=${S:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
REPO=$(cd "$S/../.." && pwd)
W=${W:-${MLB16_WORK:-$(cd "$REPO/.." && pwd)/recomp-work}}
REF_DIR=${REF_DIR:-$S/ref}
meter_ref=${METER_REF:-$REF_DIR/pitch_meter.png}
meter_crop=${METER_CROP:-370,95,395,120}
meter_thresh=${METER_THRESH:-30.0}

# Save the current guest frame (PS3RECOMP_VK_DUMP) as a 640x360 PNG.
frame_shot() {
    local dest=$1 tmp="$1.tmp.ppm" i
    for i in 1 2 3 4 5; do
        if cp "$W/frame.ppm" "$tmp" 2>/dev/null &&
           python3 -c "from PIL import Image; Image.open('$tmp').convert('RGB').resize((640,360)).save('$dest')" 2>/dev/null; then
            rm -f "$tmp"
            return 0
        fi
        sleep 0.3
    done
    rm -f "$tmp"
    return 1
}

meter_press_once() {
    printf '0x4000 6\n' > "$W/pad.txt"
}

# Match any saved meter reference (WCB on/off render the meter differently).
meter_matches() {
    local f=$1 r ok=1
    for r in "$REF_DIR"/pitch_meter*.png; do
        [ -f "$r" ] || continue
        if python3 "$S/same.py" "$f" "$r" "$meter_crop" "$meter_thresh" \
                >/dev/null 2>&1; then
            ok=0
            break
        fi
    done
    return $ok
}

# Wait until the dump matches the meter reference. $1 = output prefix.
# Echoes the matching PNG path on success.
meter_wait_ready() {
    local prefix=$1 timeout=${2:-${METER_WAIT:-20}} i dest
    for i in $(seq 1 "$timeout"); do
        sleep 1
        dest="${prefix}_${i}.png"
        frame_shot "$dest" || continue
        if meter_matches "$dest"; then
            echo "$dest"
            return 0
        fi
    done
    return 1
}

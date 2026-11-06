#!/bin/bash
# Fast pitch-screen benchmark: same env as measure6.sh, but a shorter boot wait,
# faster menu navigation, a 1 s pitch poll, and a 10 s measurement window with no
# per-thread sampling. ~2 min per run instead of ~4. $1 = log name.
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$S/common.sh"
EXE="$W/${MLB16_BUILD}/${MLB16_BIN}"
REF_DIR=${REF_DIR:-$S/ref}
SHOT_DIR=${SHOT_DIR:-$W}
cd "$W" || exit 1
LOG="${1:-measure_fast.log}"
METER=${METER:-1}
SETTLE=${SETTLE:-10}
source "$S/meter_lib.sh"

kill_game() { pkill -9 -f "$EXE" 2>/dev/null || true; }

if [ ! -f "$REF_DIR/pitch_wait.png" ]; then
    echo "measure_fast.sh: $REF_DIR/pitch_wait.png missing" >&2
    echo "  The navigation references are game screenshots and are not committed." >&2
    echo "  See SETUP.md (reference screenshots) or set REF_DIR to an existing ref/." >&2
    exit 2
fi

(EXTRA_ENV="SPURS_READY_LEVEL=1 SPURS_NO_IDLE_PROBE=1 VK_DUMMY_BLACK=1 VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1 GCM_FLIPCOUNT=1 ${PS3RECOMP_VK_PRESENT_THREAD:+PS3RECOMP_VK_PRESENT_THREAD=$PS3RECOMP_VK_PRESENT_THREAD} ${PS3RECOMP_VK_WINDOW:+PS3RECOMP_VK_WINDOW=$PS3RECOMP_VK_WINDOW} ${PS3RECOMP_RSX_WCB:+PS3RECOMP_RSX_WCB=$PS3RECOMP_RSX_WCB} $EXTRA" LIMIT=${LIMIT:-400} $S/start.sh "$LOG" &)
sleep ${BOOT:-40}
NAV_HOME=1 ${NAV_FAST:+NAV_FAST=$NAV_FAST} python3 $S/nav3.py 2>&1 | tee -a nav.log || { kill_game; echo "nav failed"; exit 1; }
if [ "${SKIP_CINE:-1}" = 1 ]; then
  SKIP_CINE=1 CINE_GAP=${CINE_GAP:-1.6} python3 $S/nav3.py --to-pitch 2>&1 | tee -a nav.log || true
fi
ok=0
for k in $(seq 1 180); do
  sleep 1
  SHOT_DIR="$SHOT_DIR" $S/shot.sh mf_$LOG
  res=$(python3 $S/same.py "$SHOT_DIR/mf_$LOG.png" "$REF_DIR/pitch_wait.png" 380,100,500,200 25.0 2>&1)
  rc=$?
  if [ $rc -eq 0 ]; then ok=1; break; fi
done
[ $ok = 1 ] || { kill_game; echo "pitch screen not reached"; exit 1; }
# The idle pitch cuts to b-roll after ~30 s; X once selects the pitch and
# parks on the meter (the first state, before the timing swing).
if [ "$METER" = 1 ]; then
  sleep 1
  echo "pressing X once for the pitch meter"
  meter_press_once
  if [ -f "$meter_ref" ]; then
    if mshot=$(meter_wait_ready "$W/mf_${LOG}_meter_$(date +%s%3N)" "${METER_WAIT:-20}"); then
      echo "pitch meter confirmed ($mshot)"
    else
      kill_game; echo "pitch meter not detected"; exit 1
    fi
  elif [ "$METER_CAL" = 1 ]; then
    echo "WARNING: $meter_ref missing; calibration mode, saving frames"
    slept=0
    for d in 1 2 4 8 16 30; do sleep $((d - slept)); slept=$d; frame_shot "$W/mf_${LOG}_cal_plus${d}s.png" || true; done
    [ -f "$W/mf_${LOG}_cal_plus2s.png" ] && cp "$W/mf_${LOG}_cal_plus2s.png" "$REF_DIR/pitch_meter_cal.png"
  else
    kill_game; echo "pitch_meter.png missing (METER_CAL=1 to calibrate)"; exit 1
  fi
fi
sleep "$SETTLE"
python3 - "$LOG" <<'PY'
import re, sys, time
log_path = sys.argv[1]
def flips():
    return [(int(m.group(1)), int(m.group(2))) for m in
            (re.search(r'\[flip\] (\d+) at (\d+) ms', l) for l in open(log_path, errors='ignore')) if m]
fl = flips()
if len(fl) < 2:
    print("meter fps: 0.00 (no flips)"); sys.exit(0)
a = fl[-1]
t0 = time.time(); time.sleep(10.0); t1 = time.time()
b = flips()[-1]
dt = (b[1] - a[1]) / 1000.0
print("meter fps: %.2f  (10 s wall %.2f s, %d flips)" %
      ((b[0]-a[0])/dt if dt > 0 else 0.0, t1-t0, b[0]-a[0]))
PY
SHOT_DIR="$SHOT_DIR" $S/shot.sh mf_${LOG}_end
kill_game
sleep 2

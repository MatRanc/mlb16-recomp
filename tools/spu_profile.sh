#!/bin/bash
# Repeatable SPU function profiler (Stage 0). Reaches the pitch screen, then
# takes N gdb stack samples of every thread and prints the per-function
# distribution. The psample per-thread attribution is unreliable; gdb is not.
# Usage: spu_profile.sh [logname] [samples]
S=/home/m/Documents/mlb16-online/recomp-work/ab-tools
W=/home/m/Documents/mlb16-online/recomp-work
cd "$W" || exit 1
LOG="${1:-spu_profile.log}"
NS="${2:-120}"
OUT="${LOG%.log}.prof"
: > "$OUT"

(EXTRA_ENV="SPURS_READY_LEVEL=1 SPURS_NO_IDLE_PROBE=1 VK_DUMMY_BLACK=1 VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1 GCM_FLIPCOUNT=1" \
 LIMIT=900 $S/start.sh "$LOG" &)

sleep 45
NAV_HOME=1 python3 $S/nav3.py 2>&1 | tee -a nav.log || { pkill -9 -f 'build-o2/MyGame[R]ecomp'; echo "nav failed" | tee -a "$OUT"; exit 1; }

ok=0
for k in $(seq 1 120); do
  sleep 2
  $S/shot.sh prof_$LOG
  python3 $S/same.py $S/prof_$LOG.png $S/ref/pitch_wait.png 380,100,500,200 25.0 >/dev/null 2>&1 && { ok=1; break; }
done
[ $ok = 1 ] || { pkill -9 -f 'build-o2/MyGame[R]ecomp'; echo "pitch screen not reached" | tee -a "$OUT"; exit 1; }

PID=$(pgrep -n -f 'build-o2/MyGame[R]ecomp')
echo "PID=$PID samples=$NS" | tee -a "$OUT"
a_flip=$(grep '\[flip\]' "$LOG" | tail -1)
t0=$(date +%s%N); sleep 10; t1=$(date +%s%N)
b_flip=$(grep '\[flip\]' "$LOG" | tail -1)
echo "flips: $a_flip -> $b_flip wall_ns=$((t1-t0))" | tee -a "$OUT"

for i in $(seq 1 "$NS"); do
  echo "### sample $i" >> "$OUT"
  gdb -p $PID -batch -nx -ex 'set pagination off' \
    -ex 'thread apply all bt 5' >> "$OUT" 2>&1
done

$S/shot.sh prof_${LOG}_end
pkill -9 -f 'build-o2/MyGame[R]ecomp'

python3 - "$OUT" <<'PY'
import re, sys, collections
text = open(sys.argv[1]).read()
samples = text.split('### sample ')[1:]
top = collections.Counter(); cat = collections.Counter()
for s in samples:
    for b in re.split(r'^Thread ', s, flags=re.M)[1:]:
        frames = re.findall(r'^#(\d+)\s+0x[0-9a-f]+ in (\S+)', b, flags=re.M)
        if not frames: continue
        n0 = frames[0][1]
        if 'jobmod_c690' in n0:
            m = re.search(r'spu_func_([0-9A-Fa-f]{8})$', n0); cat['c690 job fn'] += 1
            if m: top[m.group(1).upper()] += 1
        elif 'native_spu_func_00007C18' in n0: cat['native 7C18'] += 1; top['7C18(native)'] += 1
        elif 'spu_drain_call' in n0: cat['drain loop'] += 1
        elif 'spu_step' in n0 or 'spu_interp' in n0: cat['interpreter'] += 1
        elif any(x in n0 for x in ('futex','syscall','clock_nanosleep','pthread_cond')): cat['idle/wait'] += 1
        else: cat['other'] += 1
print("== #0 category ==")
for k,v in cat.most_common(): print(f"  {v:5d}  {k}")
print("== top c690 job functions ==")
for k,v in top.most_common(20): print(f"  {v:4d}  0x{k}")
PY

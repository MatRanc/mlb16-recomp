#!/bin/bash
# Quick pitch-meter check: boot, navigate, press X once, measure 2 x 10 s at the
# meter. Same game settings as play_windowed.sh and ab_live.sh (ab/game_env.sh),
# so its fps compares with both. One binary, one number: use ab_live.sh for A/B.
#
#   tools/ab/measure_fast.sh                 # build from common.sh (build-soa)
#   BUILD=build-depth tools/ab/measure_fast.sh
#   EXTRA="PS3RECOMP_C690_SOA=0" tools/ab/measure_fast.sh   # one-off switch
#
# Prints "meter fps: <mean>" and leaves the usual ab_live_fast_<ts>.* files.
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# EXTRA="NAME=value ..." goes into the environment at boot (any switch, live or not).
env ${EXTRA:-} TAG=${TAG:-fast} N=1 SCENE_SHOTS=${SCENE_SHOTS:-0} ARM_A="" ARM_B="" \
    "$S/ab_live.sh" > /dev/null 2>&1
rc=$?
. "$S/common.sh"
log=$(ls -t "$MLB16_WORK"/ab_live_${TAG:-fast}_*.log 2>/dev/null | head -1)
seg=${log%.log}_segments.txt
if [ $rc -ne 0 ] || [ ! -s "$seg" ]; then
    echo "measure_fast: run failed (rc=$rc); see ${log:-no log}" >&2
    grep -a "\[ab_live\] STOP" "$log" 2>/dev/null >&2
    exit 1
fi
awk '{s+=$4; n++} END {printf "meter fps: %.2f  (%d x 10 s segments)\n", s/n, n}' "$seg"
echo "log: $log"

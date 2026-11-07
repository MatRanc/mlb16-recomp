#!/bin/bash
# usage: run_arm.sh <tag> <THP_SHIM mode or none> [build]
J=/home/m/.claude/jobs/3a167ccc/tmp; TAG=$1; MODE=$2; B=${3:-build-depth}
cd /home/m/Documents/mlb16-online/recomp-work/ab-tools
export PS3RECOMP_CROWD_FIX=1 PS3RECOMP_ENG_PROG_MEMO=1 PS3RECOMP_REMAP_FLAT=1 PS3RECOMP_RSX_WCB_PREFETCH=1 PS3RECOMP_RSX_WCB_STREAM=1 \
 PS3RECOMP_SPU_FLOAT_APPROX=1 PS3RECOMP_SPU_FTZ=1 PS3RECOMP_TICK_IN_DRAIN=1 PS3RECOMP_VK_DRAW_STATE=1 PS3RECOMP_VK_PASS_KEEP=1 PS3RECOMP_VK_VTX_PRE=1 \
 PS3RECOMP_WALKER_WAKE=1 PS3RECOMP_WIPE_FIX=1 PS3RECOMP_PIN=0 PS3RECOMP_C690_STATS=0
if [ "$MODE" != none ]; then export THP_SHIM=$MODE LD_PRELOAD=$J/thp_shim.so GLIBC_TUNABLES=glibc.malloc.hugetlb=1; fi
( # counter watcher
  until [ -n "$(ls ../ab_live_${TAG}_*.log 2>/dev/null)" ] && grep -q 'pitch meter confirmed' ../ab_live_${TAG}_*.log; do sleep 2; done
  sleep 14
  P=''; for p in $(pgrep -f MyGameRecomp); do case "$(readlink /proc/$p/exe)" in *MyGameRecomp) P=$p; break;; esac; done
  echo "pid $P" > $J/ctr_$TAG.txt
  awk '/AnonHugePages/{s+=$2} END{print "AnonHugePages_kB",s}' /proc/$P/smaps >> $J/ctr_$TAG.txt
  perf stat --per-thread -p $P -x, -e cycles,instructions,ic_fetch_stall.ic_stall_any,bp_l1_tlb_miss_l2_miss,bp_l1_tlb_miss_l2_hit,ls_l1_d_tlb_miss.all -o $J/ctrA_$TAG.csv -- sleep 12
  perf stat --per-thread -p $P -x, -e cycles,ic_fw32,ic_fw32_miss,l2_cache_req_stat.ic_dc_miss_in_l2,ex_ret_brn_misp,ls_tablewalker.dside -o $J/ctrB_$TAG.csv -- sleep 12 
) &
W=$!
TAG=$TAG BUILD=$B N=2 SEG=15 DROP=2 METER=1 SKIP_CINE=1 WCB=2 PIN=0 ARM_A="PS3RECOMP_REMAP_FLAT=1" ARM_B="PS3RECOMP_REMAP_FLAT=1" REPORT=0 SCENE_SHOTS=0 ./ab_live.sh > $J/out_$TAG.txt 2>&1
kill $W 2>/dev/null
L=$(ls -t ../ab_live_${TAG}_*.log | head -1); python3 real_fps.py $L | tail -6 > $J/fps_$TAG.txt

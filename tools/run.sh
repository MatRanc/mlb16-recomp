#!/bin/sh
# Runs the built port. <work>/vfs is the guest file system root: PS3_GAME links to your disc's PS3_GAME folder (a
# mounted ISO works) and game/BCUS01089 is a copy of the v01.10 update from RPCS3's dev_hdd0. The game writes its
# data install (about 9.5 GB) to game/BCUS01089_R. Don't point this at RPCS3's own dev_hdd0.
# Usage: tools/run.sh /path/to/MyGameRecomp /path/to/EBOOT.elf work
# Scripted input: PAD_FILE=<file>, then write a button mask to it (0x4000 CROSS, 0x0008 START, 0x0040 DOWN).
EXE=$1 ELF=$2 W=$(cd "$3" && pwd)
# GCM_HANDLER_THREAD=1 needs our ps3recomp branch (gcm-handler-thread); GCM_FLIPCB_ONTICK=1 is upstream. Both are
# needed for the frame loop: see docs/progress.md (2026-10-27, night). SPURS_READY_LEVEL=1 and SPURS_NO_IDLE_PROBE=1
# (branch mlb16-work) make SPURS schedule job lists as the SDK describes; without them a match load deadlocks
# (2026-10-27, late night). The runtime logs a lot; drop the per-wait lines.
GCM_HANDLER_THREAD=1 GCM_FLIPCB_ONTICK=1 PS3RECOMP_VK_GUEST_PROGRAMS=1 PS3RECOMP_VK_WINDOW=${WINDOW:-1} \
SPURS_READY_LEVEL=1 SPURS_NO_IDLE_PROBE=1 \
PS3_TITLE=BCUS01089 PS3_VFS_ROOT="$W/vfs" PS3_GAMEDATA_ROOT="$W" "$EXE" "$ELF" 2>&1 |
    grep -av "timer_usleep\|^\s*+\|^\[WAIT\]\|LWM-"

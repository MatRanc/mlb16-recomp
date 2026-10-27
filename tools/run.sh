#!/bin/sh
# Runs the built port. <work>/vfs is the guest file system root: PS3_GAME links to your disc's PS3_GAME folder (a
# mounted ISO works) and game/BCUS01089 is a copy of the v01.10 update from RPCS3's dev_hdd0. The game writes its
# data install (about 9.5 GB) to game/BCUS01089_R. Don't point this at RPCS3's own dev_hdd0.
# Usage: tools/run.sh /path/to/MyGameRecomp /path/to/EBOOT.elf work
EXE=$1 ELF=$2 W=$(cd "$3" && pwd)
# The runtime logs a lot; drop the per-wait lines.
PS3_TITLE=BCUS01089 PS3_VFS_ROOT="$W/vfs" PS3_GAMEDATA_ROOT="$W" "$EXE" "$ELF" 2>&1 |
    grep -av "timer_usleep\|^\s*+\|^\[WAIT\]\|LWM-"

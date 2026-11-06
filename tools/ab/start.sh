#!/bin/bash
# Scripted run to the starting lineups. $1 = log file.
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$S/common.sh"
EXE="$W/${MLB16_BUILD}/${MLB16_BIN}"
ELF="$MLB16_EBOOT"
cd "$W" || exit 1; : > pad.txt
LOG="${1:-start.log}"

if [ -n "$PS3RECOMP_DUMP_FIFO" ]; then
    (
        sleep 40
        for i in $(seq 1 120); do
            if grep -aq "FIFO dump closed" "$LOG" 2>/dev/null; then
                sleep 2
                pkill -9 -f "$EXE" 2>/dev/null || true
                break
            fi
            printf '0x4000 6\n' > "$W/pad.txt"
            sleep 2
        done
    ) &
fi

export __GL_SYNC_TO_VBLANK=0
export PAD_FILE=$W/pad.txt GCM_HANDLER_THREAD=1 GCM_FLIPCB_ONTICK=1 PS3RECOMP_VK_GUEST_PROGRAMS=1 \
PS3RECOMP_VK_DUMP=$W/frame.ppm PS3RECOMP_VK_DUMP_EVERY=10 PS3_TITLE=BCUS01089 \
PS3_VFS_ROOT=$W/vfs PS3_GAMEDATA_ROOT=$W SPU_DMA_REPEAT_SECS=${REPEAT:-600} ${EXTRA_ENV} SPU_DMA_REPEAT_LOG=1 PS3_FSLOG=1

timeout -s KILL ${LIMIT:-900} "$EXE" "$ELF" 2>&1 | grep --line-buffered -av "^\[WAKE\]\|semaphore_\|lwmutex\|sys_lwcond" | awk '{print strftime("%H:%M:%S"), $0; fflush()}' > "$LOG" &
wait

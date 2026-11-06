#!/bin/bash
# Windowed play session: real textures, SDL controller input, no frame dump or
# benchmark instrumentation. Ctrl-C (or closing the window) ends it.
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$S/common.sh"
EXE="$W/${MLB16_BUILD}/${MLB16_BIN}"
cd "$W" || exit 1
: > pad.txt
export __GL_SYNC_TO_VBLANK=0
export GCM_HANDLER_THREAD=1 GCM_FLIPCB_ONTICK=1 PS3RECOMP_VK_GUEST_PROGRAMS=1
export PS3RECOMP_VK_WINDOW=1 PS3RECOMP_VK_FULLSCREEN=${FULLSCREEN:-0}
export PS3_TITLE=BCUS01089 PS3_VFS_ROOT=$W/vfs PS3_GAMEDATA_ROOT=$W
export SPURS_READY_LEVEL=1 SPURS_NO_IDLE_PROBE=1
export VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1
unset PAD_FILE PS3RECOMP_VK_DUMP VK_DUMMY_BLACK GCM_FLIPCOUNT
export PS3RECOMP_RSX_WCB=1   # render-target readback: crowd, faces, shadows
# HUD=1 uses MangoHud for the top-left fps/frametime graph (install: sudo dnf install -y mangohud).
# SDL_VIDEODRIVER=x11 forces XWayland if the Wayland present path stalls with the mouse still.
if [ "${HUD:-0}" = 1 ] && command -v mangohud >/dev/null 2>&1; then
    exec mangohud "$EXE" "$MLB16_EBOOT"
fi
exec "$EXE" "$MLB16_EBOOT"

# shellcheck shell=bash
# The game settings every MLB 16 script runs with: play_windowed.sh,
# ab/ab_live.sh and ab/measure_fast.sh source this file, so a measured number
# and a played session use the same switches. Each line keeps a value already
# in the environment, so any switch can be overridden for one run. Add a new
# default here, with a one-line why and its measurement, not in a script.
export __GL_SYNC_TO_VBLANK=0
export GCM_HANDLER_THREAD=1 GCM_FLIPCB_ONTICK=1 GCM_FLIPCOUNT=1
export PS3RECOMP_VK_GUEST_PROGRAMS=1 PS3RECOMP_VK_WINDOW=1 PS3RECOMP_VK_FULLSCREEN=0
export SPURS_READY_LEVEL=1 SPURS_NO_IDLE_PROBE=1
# Keep a still-ready policy module in the same worker for up to 8 jobs before
# the coordinator round trip (docs/notes/c690-job-gaps-ab.md). SPU_CODE_DMA_MEMO is
# left off: it was measured alone and did not move the frame metrics.
export SPURS_PM_FAST_RESCHED="${SPURS_PM_FAST_RESCHED:-64}"
# native c690 dispatch for MeshJobs0/1 (wids 1,2); docs/notes/c690-native-dispatch.md
export SPURS_PM_NATIVE_DISPATCH="${SPURS_PM_NATIVE_DISPATCH:-6}"
# menu flip fix: tick completes from our own request state (docs/notes/menu-34ms.md, menu-flip-fix.md)
export PS3RECOMP_FLIP_TICK_OWN_STATE="${PS3RECOMP_FLIP_TICK_OWN_STATE:-1}"
# character shaders: translate TXD/DDX/DDY so jersey torsos draw (docs/notes/jersey-transparent.md)
export PS3RECOMP_FP_TXD="${PS3RECOMP_FP_TXD:-1}"
# the user's Better depth of field blur, applied at ELF load (docs/notes/dof-patch.md)
export PS3RECOMP_DOF_PATCH="${PS3RECOMP_DOF_PATCH:-1}"
# write the frame dump off the walker thread (0.5-0.7 s hitches otherwise; docs/notes/present-stalls.md)
export PS3RECOMP_VK_DUMP_ASYNC="${PS3RECOMP_VK_DUMP_ASYNC:-1}"
# shader disk cache survives rebuilds (was 0 hits after every rebuild; docs/notes/boot-gaps.md)
export PS3RECOMP_SHADER_CACHE_STABLE_KEY="${PS3RECOMP_SHADER_CACHE_STABLE_KEY:-1}"
export VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1
export PS3_TITLE=BCUS01089 PS3_VFS_ROOT="$W/vfs" PS3_GAMEDATA_ROOT="$W"
export PS3RECOMP_C690_STATS="${PS3RECOMP_C690_STATS:-0}"  # diagnostic only; on costs ~2.3 real fps at the meter (2026-11-06), set 1 when needed
export PS3RECOMP_WALKER_PROF=1
export PS3RECOMP_VK_PRESENT_STATS=1
export PS3RECOMP_PUMP_MS=8
# Thread affinity: default off. PS3RECOMP_PIN=1 pins the walker to cpu 0, ppu
# main to cpu 1 and the five c690 lanes to the remaining cores; the two-launch
# A/B on 2026-11-04 measured 35.31 fps at PIN=0 vs 30.98 at PIN=1 at the pitch
# meter (docs/notes/pin-and-burst-path.md). Keep the default explicit while still
# allowing an override.
export PS3RECOMP_PIN="${PS3RECOMP_PIN:-0}"
# On-disk cache of translated guest shaders (docs/notes/shader-disk-cache.md): a warm
# run rebuilds first-use pipelines without glslang. Keyed by the guest program
# identity plus the translator version; recomp-work/shader_cache/.
export PS3RECOMP_SHADER_DISK_CACHE="${PS3RECOMP_SHADER_DISK_CACHE:-1}"
# Cheap wins (mlb16-recomp/docs/notes/cheap-wins.md), all individually switchable:
#   SPU_MFC_LOOKUP_CACHE  per-host-thread MFC engine memo (no registry SRW lock)
#   GCM_NOP_SKIP          skip NV4097 NO_OPERATION (0x0100) runs in the walker
#   GCM_REF_IMMEDIATE     publish SET_REFERENCE at execution, not at drain tail
#   PS3RECOMP_RSX_TEX_PIN never evict a texture bound this frame
#   PS3RECOMP_TEX_DIRTY  validate textures by guest-page write generation
# Set any to 0 in the environment to turn it off.
export SPU_MFC_LOOKUP_CACHE="${SPU_MFC_LOOKUP_CACHE:-1}"
export GCM_NOP_SKIP="${GCM_NOP_SKIP:-1}"
export GCM_REF_IMMEDIATE="${GCM_REF_IMMEDIATE:-1}"
export PS3RECOMP_RSX_TEX_PIN="${PS3RECOMP_RSX_TEX_PIN:-1}"
# Validate cached textures from a per-4K-page write generation instead of
# hashing their bytes (docs/notes/texture-dirty.md): -3.4 ms walker drain, tex
# 10.3 -> 5.9 ms/frame at the pitch meter.
export PS3RECOMP_TEX_DIRTY="${PS3RECOMP_TEX_DIRTY:-1}"
# Pipeline answer memo (docs/notes/walker-quick-wins.md): -0.47 ms of walker `pipe`
# at the pitch meter, 85 % hit live, replay gates unchanged. The fetch plan
# memo (PS3RECOMP_FETCH_MEMO / PS3RECOMP_WALKER_FAST) is deliberately NOT on:
# live its reference sets change every frame, it hits ~5 %, and it costs
# ~2.4 ms/frame (see the same doc).
# Library fixes batch 1 (mlb16-recomp/docs/notes/hle-fixes-batch1.md). All are
# default on here and individually switchable; 0 restores the old behaviour.
#   PS3_LWCOND_REAL            real lwcond waits/timeouts/signals, lwm destroy
#   PS3RECOMP_AUDIO_PACE       POSIX audio at the real 5333 us block period
#   PS3RECOMP_FS_WRITE_CHECK   cellFsWrite errors on a short/failed write
#   PS3RECOMP_SPURS_TEARDOWN   Shutdown/Wait drain a workload before Remove
#   PS3RECOMP_GCM_OFFSET_SYNC  guest io/ea offset tables follow map/unmap
export PS3_LWCOND_REAL="${PS3_LWCOND_REAL:-1}"
export PS3RECOMP_AUDIO_PACE="${PS3RECOMP_AUDIO_PACE:-1}"
export PS3RECOMP_FS_WRITE_CHECK="${PS3RECOMP_FS_WRITE_CHECK:-1}"
export PS3RECOMP_SPURS_TEARDOWN="${PS3RECOMP_SPURS_TEARDOWN:-1}"
export PS3RECOMP_GCM_OFFSET_SYNC="${PS3RECOMP_GCM_OFFSET_SYNC:-1}"
export PS3RECOMP_PIPE_MEMO="${PS3RECOMP_PIPE_MEMO:-1}"
# Remap-build recent-reference memo (docs/notes/walker-cuts-2.md): answer a nearby
# duplicate from a 1024-entry direct-mapped memo instead of probing the open
# addressing table. -0.57 ms/frame offline, replay gates unchanged. Default on.
export PS3RECOMP_REMAP_FAST="${PS3RECOMP_REMAP_FAST:-1}"
# Complete a flip whose commands the drain has already executed and whose image
# the present path has queued, from the top of the drain pass. Implemented and
# unit-tested. DEFAULT ON since 2026-11-06: scored on real frames (buffer changes) it is +3.95 fps;
# the old -8 fps were the title's 34 ms timeout re-flips (docs/critical-path.md).
# 2026-11-20: OFF. With the SoA kernels it gave the worst outliers at the meter
# (p99 54 ms, segments dipping to ~30 fps; ab_live_flipedge_20261120_025615).
export PS3RECOMP_TICK_IN_DRAIN="${PS3RECOMP_TICK_IN_DRAIN:-0}"
# Complete a flip when its present is queued (the title flips immediate, no
# vsync). 2026-11-20: meter 43.0 -> 43.8 fps, frames >=26 ms 32% -> 5%, sd 5.2
# -> 1.9 ms (the 34 ms deadline mode is gone); mound visit 28.7 -> 27.3 fps but
# sd 4.6 -> 2.4 ms (ab_live_mound_flipdone_20261120_030427). User: looks good.
export PS3RECOMP_FLIP_ON_DONE="${PS3RECOMP_FLIP_ON_DONE:-1}"
# Native c690 SoA kernels (item 4); the flip numbers above were measured with them.
export PS3RECOMP_C690_SOA="${PS3RECOMP_C690_SOA:-1}"
# Driver pipeline cache across sessions. Async pipeline compiles stay OFF: two
# attempts skipped draws the title's exposure/grading depends on and the whole
# match came out washed out (2026-11-20; parked on exp/pipe-async-safe).
export PS3RECOMP_VK_PIPELINE_CACHE="${PS3RECOMP_VK_PIPELINE_CACHE:-1}"
export PS3RECOMP_PIPE_ASYNC="${PS3RECOMP_PIPE_ASYNC:-0}"
# Complete a presented flip when the next frame's fence is published (the
# marker-side design for this title: no in-FIFO flip word exists). DEFAULT OFF:
# the next frame's fence lands after the guest's 2x17 ms deadline, so completing
# there is late, and completing earlier just moves the wait into the buffer ref
# (docs/notes/frame-pacing-floor.md).
export PS3RECOMP_FLIP_FENCE_DONE="${PS3RECOMP_FLIP_FENCE_DONE:-0}"
# Present the window only when the title produced a new frame. Without it the
# frame clock's outer pass re-presents on a flip request, so MangoHud's row
# rate (one per vkQueuePresentKHR) reads above the guest [flip] fps. The
# window pump and input delivery are independent of presents, so this does not
# affect responsiveness; the confirm run measured 42.57 rows/s vs 42.66 [flip]
# fps at the start screen (docs/notes/textures-dummy-census.md).
export PS3RECOMP_VK_PRESENT_NEW_ONLY=1
# Even out the window present cadence: the guest finishes frames in
# bursts (two flips ~1 ms apart, then a ~46 ms gap), so the display saw
# pairs instead of a steady ~34 ms. The pacer holds each present until
# its arrival-cadence slot, at most one frame interval (docs/notes/frame-pacing.md).
export PS3RECOMP_VK_PACE="${PS3RECOMP_VK_PACE:-1}"
# cellGcmSetPrepareFlip must return the display buffer id, as the real SDK
# and RPCS3 do. It used to return CELL_OK (0), so every flip named
# display buffer 0 while the title rendered alternately into 0 and 1;
# a shown frame then repeated one already replaced (the whole-frame
# backward step, docs/notes/backward-frames.md). Default on, 0 restores it.
export PS3RECOMP_PREPARE_FLIP_RET="${PS3RECOMP_PREPARE_FLIP_RET:-1}"
# Disabled texture units read magenta otherwise, which makes the picture pink.
# measure_fast uses the same black dummy.
export VK_DUMMY_BLACK=1
# Native Wayland + NVIDIA: MAILBOX can leave the present thread blocked in
# vkQueuePresentKHR for tens of seconds, dropping every frame no-slot while the
# guest keeps flipping. FIFO is the required present mode and avoids that path
# (Xwayland negotiates FIFO too). Override with PS3RECOMP_VK_PRESENT_MODE=mailbox.
export PS3RECOMP_VK_PRESENT_MODE="${PS3RECOMP_VK_PRESENT_MODE:-fifo}"
# Env PS3RECOMP_RSX_CUBE_RT=0 turns the GPU cube render-target path off.
export PS3RECOMP_RSX_CUBE_RT="${PS3RECOMP_RSX_CUBE_RT:-1}"
# Renderer cuts + early render-target copies (docs/critical-path.md 3b, 2026-11-05: crowded replay view
# 15 -> 31-34 fps, no visual change seen). Live-switchable; set any to 0 to A/B. REMAP_* measured no gain.
# 2026-11-06 merge (user-checked): crowd sub-draw state, swizzled-RT wipe fix, SPU FTZ (bit-exact on the corpus).
export PS3RECOMP_CROWD_FIX="${PS3RECOMP_CROWD_FIX:-1}"
export PS3RECOMP_WIPE_FIX="${PS3RECOMP_WIPE_FIX:-0}"  # 2026-11-06: replaced by ENG_SWZ_RT (the heuristic missed the mound-visit entry)
export PS3RECOMP_SPU_FTZ="${PS3RECOMP_SPU_FTZ:-1}"
export PS3RECOMP_SPU_FLOAT_APPROX="${PS3RECOMP_SPU_FLOAT_APPROX:-1}"  # 2026-11-06: +1.63 real fps at the meter, user visual check OK
export PS3RECOMP_WALKER_WAKE="${PS3RECOMP_WALKER_WAKE:-1}"  # 2026-11-06: +0.82 real fps at the meter
export PS3RECOMP_REMAP_FLAT="${PS3RECOMP_REMAP_FLAT:-1}"  # 2026-11-06: walker -2.8 ms, +0.5 real fps (n.s.), user visual check OK
export PS3RECOMP_ENG_PROG_MEMO="${PS3RECOMP_ENG_PROG_MEMO:-1}"  # 2026-11-06: exact (replay bit-identical), +0.22 real fps at the meter
export PS3RECOMP_ENG_SWZ_RT="${PS3RECOMP_ENG_SWZ_RT:-1}"  # 2026-11-06: swizzled render targets stored in Morton order; fixes the tiled wipes, fps neutral
export PS3RECOMP_VK_DEPTH_BIAS="${PS3RECOMP_VK_DEPTH_BIAS:-1}"  # 2026-11-06: guest polygon offset; fixes the uniform stipple (shadow acne), fps neutral
export PS3RECOMP_VK_PASS_KEEP="${PS3RECOMP_VK_PASS_KEEP:-1}"
export PS3RECOMP_VK_VTX_PRE="${PS3RECOMP_VK_VTX_PRE:-1}"
export PS3RECOMP_VK_DRAW_STATE="${PS3RECOMP_VK_DRAW_STATE:-1}"
export PS3RECOMP_RSX_WCB_PREFETCH="${PS3RECOMP_RSX_WCB_PREFETCH:-1}"
export PS3RECOMP_RSX_WCB_STREAM="${PS3RECOMP_RSX_WCB_STREAM:-1}"

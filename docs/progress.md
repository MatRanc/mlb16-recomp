# Progress log

## 2026-11-05: SPU lifter register pressure; the 30 % spill target is not reachable this way (exp/spu-regs)

- Roundtable item 4. `--local-regs` already does the per-function write/read bookkeeping (dirty-only write-back,
  callee-write-set reloads), so the remaining 26-37 % GPR/stack share is intra-function live-set pressure. A new
  `--precise-regs` (lazy prologue + live-out reload filter) is correct and cuts executed reloads ~50 % on the
  `test_spu_local_regs.py` torture programs, but clang at -O2 already deletes those loads/reloads: the c690 hot
  functions are unchanged (0 %). `--ib-goto-starts` adds the documented `0xC0F4` self-back-edge goto
  (`0xC018` 468 -> 554 x86, +7.1 % module text); on four captured 0x21 kernel states it halves host dispatches
  (146 -> 79, 153 -> 71, 190 -> 101, 250 -> 187) with identical final LS/GPR. That is a dispatch win, not a spill
  win. No title lift flag changed; no game launch. Details: [`spu-regs.md`](spu-regs.md).
- Correctness: 0xFDA0 stage corpus (4 pairs) LS+GPR bit-exact for baseline/precise/goto/both; `run_tests.sh` 11/11;
  `test_spu_local_regs.py` and `test_spu_ib_known_targets.py` pass; WWS native-PM claim differential passes (test
  harness now defines `g_spu_direct`/`g_spu_stack_reset_hint` so a PM lifted with `--ib-goto` links).

## 2026-11-05 (walker diet): keeping the render pass open and hoisting the vertex unpack cuts the draw phase ~4 ms

Branch `exp/walker-diet` (docs/walker-diet.md). Four default-0 live keys; offline Vulkan crowded replay walker
43-46 -> 38.3 ms, `draw` 13-14 -> 8.9, byte-identical frame, validation layers clean. The shadow constant texture is
correct but gains nothing after the depth-GPU fix. Existing REMAP_DIRECT+FAST add another ~1.8 ms of `fetch`.
Live perf shows the walker on-CPU only ~52% of the time, so CPU-side gains are bounded. USER owes one live flip test.

## 2026-11-05 (RSX sync): the guest's GPU-done wait, and the write-vs-pending-draw window

Branch `exp/rsx-sync` (off `exp/live-ab`) in `ps3recomp-sync`, build
`recomp-work/build-sync`; **zero game launches** (Ghidra + the 2026-11-05 logs).
The game's resource-release fence is `Gfx_WaitFrameRef` (`0x00063b98`): it
polls the GCM control `ref` word for the last frame's `SET_REFERENCE`
(NV406E `0x0050`) token, on lwcond `DAT_01245068`; `UI_Movie_Open` calls it on
the first-open path only, and the per-frame gate is N-1 (one render frame in
flight). Our walker executes the token after the draws and reads their texture
bytes synchronously (`eng_texture_upload_work`; the async resolver is a no-op),
so the label is not early. The remaining window is a guest write that lands
after the fence but before the pending draw's bind; `PS3RECOMP_RSX_SYNC_FIX=1`
holds such a write until one drain pass that started after it has finished.
Offline 34/34 fixtures (the known `test_gcm_tick_in_drain` flake rerun passes);
no live launch. Details, frame-lag numbers and the A/B command:
[`rsx-sync.md`](rsx-sync.md). User live check still owed.

## 2026-11-05 (depth GPU): the 222 depth-guest binds are one empty shadow map

Branch `exp/depth-gpu` (off `exp/crowd-prof`), build `build-depth`, no game launch. `dguest` was a single 2048x1024
Z24S8 zeta sampled as linear DEPTH24_D8 by 222 draws a frame; the crowded capture clears it and never draws into it, and
`eng_zdepth_snapshot` refused a clear-only zeta (needs `had_write`), so each frame paid a GPU wait + 8 MB readback +
store + re-upload. `PS3RECOMP_ENG_DEPTH_GPU=1` (live key, default 0) snapshots it on the GPU while its guest copy is
unstored; the store stays lazy. Offline real-Vulkan, 3 runs: walker 61.3 -> 42.2 ms, `tex` 23.6 -> 5.4, `dguest`
222 -> 0; final frame byte-identical. Details: [`depth-gpu.md`](depth-gpu.md). User live check still owed.

## 2026-11-05 (crowded walker profile): the live texture cost is the Vulkan depth guest fallback

Full details in [`docs/crowded-walker-profile.md`](crowded-walker-profile.md).
Branch `exp/crowd-prof` (off `exp/live-ab`) in `ps3recomp-prof`, build
`recomp-work/build-prof`; **zero game launches** (offline capture + the user's
11:06-11:11 log). Code pushed to `fork` only; measurement switches only, all
default off.

- **The replay tool can drive the real Vulkan backend.**
  `PS3RECOMP_REPLAY_VK=1` replays `captures/walker_replay_fixed` headless on
  the RTX 3060 Ti with the shipping guest-program path, `VK_DRAW_BATCH=1`,
  `VK_FRAME_PIPELINE=1`. The null path and its digest are unchanged
  (`0e8f7c71ca8be27c` / `a9a80aa0392953cd`).
- **Null vs Vulkan, 1,694 draws, 0 misses:** walker **34.0 -> 62.2 ms**,
  `tex` **5.0 -> 23.8**, `draw` **6.0 -> 13.8**. The premise's 2.4-3.0 ms
  `tex` came from the null backend + the old 1,261-draw capture; the Vulkan
  `tex` matches live (28.1 ms in the 11:06-11:11 crowded window).
- **Top lever: the depth-target guest fallback.** Live binds a depth target
  as a texture **228 times/frame** (`dguest`); offline Vulkan does 222, same
  path. Skipping the guest copy (`PS3RECOMP_ENG_DEPTH_NOGUEST=1`, measurement
  only) drops `tex` 24.2 -> 5.5 ms and walker 64.9 -> 43.7. Live pays the
  readback but not the induced re-upload, so **~13 ms/frame** conservatively
  (19 ms upper bound).
- **Next levers:** per-draw Vulkan recording **4-8 ms** (13.4 % of perf
  samples in `libnvidia-glcore` under `vk_eng_draw_from`; batching already
  removed the 100 ms per-draw submit), then live `parse` **up to ~14 ms**
  (16.9 live vs 2.8 offline; a 2-5 burner load sweep leaves `parse` at
  3.0-9.6 ms, so it is an in-drain stall / producer wait, not CPU contention).
- **Offline test that cannot be run:** TEX_DIRTY is deliberately disabled in
  replay (`s_guest_reader` guard, rsx_draw_engine.c:3182); its live A/B
  (`tex` 10.34 -> 5.89 ms) still stands. WCB=2 is not armed; its texture-bind
  cousin is the depth guest path measured above.

## 2026-11-05 (team-logo green flash): the Bink planes are drawn before the first decode

Full details in [`docs/bink-video.md`](bink-video.md) §7. Branch `exp/bink-green`
in `ps3recomp-play`, build `recomp-work/build-green`; **no game launch** (the
user plays and checks team select). Code pushed to `fork` only; the key is
**default off**.

- **The green is the game's own clear.** `UI_Movie_Open` (`0x0097fff0`) clears
  both Bink Y/Cr/Cb frame-buffer sets to zero (`0x004f7a80`, store
  `0x004f7bd8`), then registers them and kicks `BinkDoFrameAsync`. The logo
  sprite is gated only on `ctx+0x1d0 >= 4` update calls
  (`UI_Movie_GetTextureIfReady` `0x00981488`, `0x009814ac`), never on
  `BinkWait` or the decoded-frame counter `+0x110`. If the first decode has not
  landed by the fourth draw, the sprite samples the cleared planes:
  Y=0/Cb=0/Cr=0 -> RGB ~(0,135,0). Fast scrolling opens and shows the movie
  inside that window; slow scrolling gives the decode time.
- **Renderer divergence fixed behind `PS3RECOMP_BINK_GREEN_FIX`.** TEX_DIRTY
  does cover the decoder's SPU PUTs, but the cache revalidated at most once per
  presented frame, the texture memo skipped same-frame repeat binds, and the
  default SPU PUT charged the write generation before the memcpy. The key adds
  open-pages -> copy -> charge for SPU MFC PUTs, same-frame generation
  revalidation, and a memo bypass when the generation moved.
- **Offline:** three new `test_rsx_draw_engine` checks (same-frame re-upload;
  prepare alone does not invalidate; post-copy note does). `test_video_fixtures`
  34/34; `test_gcm_tick_in_drain` is a pre-existing ~50 % flake, reproduced on
  unmodified `exp/live-ab`. `build-green` builds with build-play's options.
- **Limit:** if the decode lands after the frame's draw, the guest's own
  four-tick gate still shows the clear; that needs the decode side, not the
  renderer. The user A/B (`BUILD=build-green PS3RECOMP_BINK_GREEN_FIX=1`, then
  without) and the `[bink-green]` lines decide which case it is.

## 2026-11-05 (c690 lane gaps): the ~20 % idle is the PM's head/tail, not runtime dispatch

Full details in [`docs/c690-lane-gaps.md`](c690-lane-gaps.md). Branch
`exp/live-ab` in `ps3recomp-play`, build `recomp-work/build-play`. Two
pitch-meter launches (the cap), X once: `ab_live_lanegapsA_20261105_080615`
and `ab_live_lanegapsB_20261105_081004`; 24 s timelines each.
`SPURS_READY_WAKE` stays **default off**. Code pushed to `fork` only.

- **The lanes are 4.05 of 5 busy inside a 29.16 ms burst; each lane is idle
  5.56 ms (19.1 %).** The gap splits per lane per frame as tail 2.519 (job end
  -> PM end), head 2.075 (PM start -> job), host 0.599 (PM end -> next PM),
  other-workload 0.258, lock-line 0.035. **Tail + head = 84 % of the gap**:
  the guest WWS policy/job code around each job, run through our lift.
- **Host dispatch is small and already tight.** Native/fast rerun check 0.311
  lane-ms/frame (p50 1.0 us); coordinator wait 0.215 (p50 20.1 us, mean
  106 us, tail = the PPU has not published the next list); signal -> run wake
  0.043 (p50 10.1 us); both-kick 0.029. Lock-line retries 0.035.
- **Fixed the only plausible local runtime class, measured neutral.**
  `SPURS_READY_WAKE=1` makes a PPU `cellSpursReadyCountStore`/`Swap`/`Cas` or
  `cellSpursWakeUp` kick the coordinator at once instead of leaving it to the
  1 ms timed pass. Offline: coordinator-kick unit test PASS, scheduler PASS,
  TSan clean in the new path (4 documented fixture reports), ABI PASS. Live
  A/B: fps **36.50 -> 35.88**, p50 **34.15 both**, p90 37.62 -> 38.18, p99
  40.42 -> 41.04, lanes 4.05 -> 4.06; coordinator wait 0.215 -> 0.201
  lane-ms/frame (p50 unchanged). No win; key left off.
- **The "close 6 ms of runtime" premise is false.** Even zeroing host + DMA
  copies (~0.34) + lock line leaves the burst unchanged by ~1 ms; 4.6 ms of
  the 5.56 ms lane idle is per-job lifted PM code. The remaining lever is
  `spu-codegen-audit.md`, not the scheduler.
- Deviations: the switch is env-only (this mission may only touch
  `runtime/spu` + `libs/spurs`; the live-key table is in `libs/video`), so
  the A/B used the two-launch fallback. TSan used clang because gcc's
  `libtsan.so.2.0.0` symlink is missing.

## 2026-11-05 (boot gaps / match load): C6 was already fixed by the DXT lift; the shader cache never hit

Full details in [`docs/boot-gaps.md`](boot-gaps.md). Branch `exp/live-ab` in
`ps3recomp-play`, build `recomp-work/build-play`. Two boot -> pitch-meter
launches (the cap), X once: `ab_live_bootgap_off_20261105_073251` and
`ab_live_bootgap_on_20261105_073812`; the second was after a rebuild.
`PS3RECOMP_SHADER_CACHE_STABLE_KEY` stays **default off**. Code pushed to
`fork` only.

- **The 237-654 ms C6 boot gaps are gone.** They were the interpreted DXT
  kernel on the four `Wws_Job/DXTJob` lanes; the lift (750cb5e9, default on)
  removed them (11 gaps / 7.6 s -> 1 / 0.31 s at the start screen). Post-lift
  boot runs have no asset-loading gap of that size.
- **The remaining boot gaps are Bink intro decode, not a runtime wait.**
  100-132 ms frames at 16-28 s: six `spurs pm` lanes idle (`spu.run` 2-3 ms
  per window), walker idle, display on its flip lwcond, main PPU polls
  `sys_timer_usleep(400)` at guest LR `0x00EE93E4` (Bink SDK) 200-273 times
  per frame; the Bink task runs 28-32 ms rounds on its own host thread.
- **The match-load cheap bug:** the shader disk cache never hit. Its HLSL
  hash is seeded with `kTranslatorBuild = __DATE__ " " __TIME__`
  (`rsx_shader_spirv.cpp:144`, `:266`), so every rebuild rejects every file.
  Both cold runs: **0 hits, 856 misses, 856 rejected**.
- **Fix (opt-in, default off):** `PS3RECOMP_SHADER_CACHE_STABLE_KEY=1`
  hashes without the build stamp and accepts an old-build file when the
  translator version, glslang, guest key and HLSL length agree. It still
  rejects a version bump, a different key/length, a bad checksum or trailing
  bytes. `test_shader_spirv` **67/67**.
- **Measured:** load frame **4,341 ms -> 1,239 ms** (-71 %); shader work in
  it **3,292 ms -> 36 ms**; `[pipe-timing]` **213 glslang / 643 mem / 0 disk
  -> 862 disk / 0 glslang / 0 mem**. Boot -> meter 167 s -> 152 s; the
  key-off run also has an unrelated 6.8 s `pres_to_show` compositor stall
  (`[fliplog] n=1862`), so the controlled number is the load frame.
- **No SPURS_MAXC run was needed.** The idle lanes in the old DXT gap are
  not the current limiter.

## 2026-11-05 (c690 extra host lanes): the guest's maxContention is the ceiling; no burst win

Full details in [`docs/c690-extra-lanes.md`](c690-extra-lanes.md). Branch
`exp/c690-lanes` in the new worktree `ps3recomp-lanes` (from `exp/live-ab`,
with the parked WIP `27f49913` cherry-picked and fixed), build
`recomp-work/build-lanes`. Code pushed to `fork` only;
`PS3RECOMP_C690_EXTRA_LANES` stays **default 0**. One pitch-meter launch (the
cap), arms 0 / 2 / 4 hidden host lanes, 4 x 12 s each, plus a 24 s timeline.

- **Why lane 6 wedges:** `WwsJobList_SpinUntilDone` waits for a completed
  prefix of the job list, the minimum of six 16-bit per-lane cursors
  (`+0x42..+0x4c`) and the list counter (`+0x4e`). MeshJobs0/1 publish
  `maxContention=5` and `priority[5]=0`; only lane 0's cursor starts live.
  `SPURS_LANE6` made a sixth stream active and the guest deadlocked (flip
  stop, PPU parked, FIFO bad word). The safe invariant is total mesh
  instances <= maxContention, one PM context per `(wid, spu_num)`.
- **Design:** dormant helper workers run the same lifted PM on a physical row
  whose same wid is not already running; one ready unit / signal consumed
  under the same lock line; guest `active`/`curcont`/`ready` untouched; live
  key 0-4, helpers retire when the key drops. WIP fixes: u16 `lane_wid`,
  normal-dispatch row ownership, per-slot live retirement, stop joins
  helpers.
- **Gates:** scheduler fixture (hidden-slot + N-worker 5-instances/unique-row
  cases) PASS; ASan/UBSan 0 errors/0 leaks; TSan only the fixture's own
  guest-store races; video fixtures 33/33; ABI PASS.
- **Live A/B:** fps 36.60 / 36.25 / 36.45, p50 **34.15 ms in all arms**, p90
  38.46 / 38.35 / 38.25, p99 42.24 / 42.10 / 41.88. Timeline last c690 job
  p50 32.59 / 32.29 / 32.99 ms, lanes 4.04 / 4.05 / 4.25. The helper runs
  (~2.5 k jobs/s) but steals the jobs w0 stops doing (w0 3,028 -> 1,945);
  aggregate is flat. Thumbnails show no Edge divergence; 12 segments / ~146 s,
  no hang.
- **Conclusion:** more host workers behind the guest's 5-lane contract cannot
  shrink the burst. The remaining levers are per-job compute/codegen or a
  host-side job stealer that fills intra-lane gaps; going >5 is the lane-6
  wedge.

## 2026-11-05 (pipeline async, verified): the key-on failure was an inverted publish; fixed, cold run reaches the meter with 0 failures

Full details in [`docs/pipeline-async.md`](pipeline-async.md) §7. Branch
`exp/live-ab` in `ps3recomp-play`, build `recomp-work/build-play` at
`ff6e5f02` + `f3311e68`; code pushed to `fork` only. `PS3RECOMP_PIPE_ASYNC`
stays **default off**. Two cold-cache pitch-meter launches (the cap); caches
moved to `recomp-work/captures/pipeverify_20261105_064907/` and restored,
never deleted.

- **Root cause of the failed key-on launch: an inverted publish, not the
  driver.** `vk_eng_pipe_modules` returns 0 on success, but `vk_pa_run`
  stored `ok ? 2 : 3`, so every successful module job was published as
  state 3 ("translation failed"). The draw thread never built a pipeline;
  the pre-fix cold run reproduced it exactly (18 module jobs / 36
  `[pipe-timing]`, **0 `[pipe-create]`**, 448,360 skipped draws, 448,309
  `reason=failed`, pitch never reached). The old "worker-side
  `vkCreateGraphicsPipelines` returned null" diagnosis is corrected.
- **Fix:** `rc == 0 ? 2 : 3`, plus a log on `vkCreateShaderModule` failure.
  The worker still only translates and creates modules; the `VkPipeline` is
  built on the draw's thread with its own `s_e2` and `s_g.pipe_layout`, the
  proven synchronous path.
- **Live cold key-on run** (`ab_live_pipeasync_on3_20261105_070746`): meter
  at **185 s** (key off 214-217 s); drains **218 / 3 / 0** (>50 / >200 /
  >1 s) vs 427 / 9 / 1; largest **560.2 ms** (texture/surface work, pipe
  36.4 ms) vs the 4,324.1 ms load. **0 `reason=failed`**; **10,662** skipped
  draws, all `reason=modules`; max pending **15 consecutive frames (3.39 s)**
  during the black match-load transition; no persistent holes. Meter fps
  **32.25** vs 32.80 (mean frametime 31.01 vs 30.49 ms, within noise).
- **Gates:** key-off replay meter `a6fcc849341eba9b` / gate
  `ff2a220c28750bc3`, walker `416abee2c5bff47d` / gate
  `0bdb6c112e64e643`; `test_vk_pipe_queue` PASS under clang TSan. Verified
  user command: `PS3RECOMP_PIPE_ASYNC=1 PS3RECOMP_SHADER_DISK_CACHE=1
  PS3RECOMP_VK_PIPELINE_CACHE=1 recomp-work/play_windowed.sh`.

## 2026-11-05 (pipeline async): translation is 93 % of the first-use stall; worker path added, key-on launch still to verify

Full details in [`docs/pipeline-async.md`](pipeline-async.md). Branch
`exp/live-ab` in `ps3recomp-play`, build `recomp-work/build-play`. Code pushed
to `fork` only; `PS3RECOMP_PIPE_ASYNC` stays **default 0**. Two cold-cache
pitch-meter launches (the cap); shader/pipeline caches moved to
`recomp-work/captures/pipeasync_20261105/` and restored, never deleted.

- **Where the cold drain goes (key off, meter at 217 s):** 918 translations
  = **5,600.8 ms** (6.1 ms mean), `vkCreateShaderModule` 25.3 ms,
  fp/vp decompile 53.7 ms, `vkCreateGraphicsPipelines` 448.8 ms (460 builds,
  0.98 ms mean, 6.7 ms max). 216 ran glslang, 702 were memory hits; 918 cold
  disk misses wrote 16 MiB. Drains before the meter: 427 > 50 ms, 9 > 200 ms,
  1 > 1 s (4,324.1 ms load, `pipe=3,440.9 pipecrt=178`, `pipe_decompile`
  22.1 ms). The async work is therefore the HLSL -> SPIR-V step, not the
  driver pipeline and not the decompilers.
- **Implemented** `PS3RECOMP_PIPE_ASYNC` (live key): `pipeline_create` copies
  the HLSL and returns a slot; a bounded 256-job worker
  (`libs/video/vk_pipe_queue.c`) translates both stages and creates the
  modules. Draws wait up to a shared **2 ms per frame** budget
  (`PS3RECOMP_PIPE_ASYNC_WAIT_US`) and are then skipped; every skip is
  logged. The `VkPipeline` is built on the draw's thread (max 6.7 ms, under
  one frame). The async key also turns on the existing persistent
  `VkPipelineCache` and the worker prefetches the shader-cache files at boot.
- **Gates:** key-off replay last-frame digests unchanged (meter
  `a6fcc849341eba9b`, walker `416abee2c5bff47d`); `test_vk_pipe_queue` under
  clang TSan clean; video fixtures 34/34 (`test_gcm_tick_in_drain` is
  pre-existing flaky).
- **The key-on launch failed, and is why the design changed.** The first
  version also built `VkPipeline`s on the worker; the cold key-on run logged
  459,673 skipped draws and **zero** `[pipe-create]` lines, never reached the
  pitch, and was killed at the 900 s limit. The final code keeps only
  translation/modules on the worker and uses the proven synchronous driver
  call on the draw thread. The key-on live numbers are still missing; the
  morning user run (command in the doc) is the first test. Watch for brief
  missing objects (skip side effect) and for `reason=failed` (translation
  failure, pipeline will never appear).

## 2026-11-05 (native 0x21): whole-command rewrite is bit-exact offline but at parity; the one live A/B attempt wedged (pre-fix)

Full details in [`docs/edge-native-21.md`](edge-native-21.md). Branch
`exp/native-21` in `ps3recomp-n21`, build `recomp-work/build-n21`. Code pushed
to `fork` only; `PS3RECOMP_C690_NATIVE_21` stays **default 0**. Two pitch-meter
launches (the cap): launch 1 lost to a missing archive anchor, launch 2 captured
the corpus and ran the A/B.

- **Implemented the whole command**, not a leaf: `tools/c690_ref/gen_native_21_stage.py`
  flattens the 0x13080 dispatcher, the 0x13140/0x131A0 bodies and their
  transform/pack/normalize tails into `runtime/spu/c690_native_21_stage.inc`
  (80 blocks). Tail transfers are gotos; `brsl` calls keep the lifted
  `ctx->gpr` marshalling so the caller-visible state is exact. Weak hooks in
  the shared `spu_lifted` tree (the 0x2A hooks are untouched); a strong anchor
  in `spu_dispatch_mt.c` extracts the archive object; capture spool
  `PS3RECOMP_C690_21_CAPTURE`.
- **Offline exact:** 78 meter entries captured; on the **76/76** that the
  lifted reference can replay, the native stage matches the full LS (0 bytes),
  all 128 GPRs (0 words) and pc/status/stop. The 2 others hit a computed target
  the lifted resolver cannot resolve from an LS-only capture and fall back.
- **What the meter runs:** 11 selector 1 / 67 selector 7 entries (14/86 %);
  selector 7 is `0xA3F8 -> 0xA8A0`, selector 1 here is
  `0x13250 -> 0xA3C0 -> 0xA410`. The `0xBB80`/`0xBB98` C/D path is only taken by
  the `r6` bit-4 formats, not these entries.
- **No speedup.** Warm, back-to-back arms over 76 pairs (200 replays each):
  **31.44 us lifted vs 31.42 us native (0.998)**. An alternating microbench's
  2.5-3x is I-cache-confounded. Consistent with the codegen audit: the 0x21 gap
  is emitted instruction count, not call marshalling.
- **Live attempt:** `ab_live_native21ab_20261105_060006` captured 96 entries in
  the settle, ran a clean A segment, then stopped flipping 0.2 s after the
  first B switch. The pre-fix build wrote 1-2 wrong bytes in the ring word
  `0x1F0A2..A3` (dropped `brsl` marshalling); keeping the stores/reloads makes
  it bit-exact. Not re-run: 2-launch cap spent.
- **Decision:** keep it off; **do not send 0x21 to the GPU** (the hand-off
  costs ~5.96 ms/frame and there is nothing to save). The realistic 0x21 lever
  remains a register-mapped codegen backend (~1.2-1.4x per the audit), not
  another per-command native rewrite. One live soak of the fixed exact build is
  available if the user wants it.


## 2026-11-05 (present stalls, C4): the frame dump blocked the walker; async writer, default off

Full details in [`docs/present-stalls.md`](present-stalls.md). Branch
`exp/live-ab` in `ps3recomp-play`, build `recomp-work/build-play`. **Two
pitch-meter launches (the cap)**: one diagnostic with a stall tracer, one
confirm with the fix. Code pushed to `fork` only; the key is default off.

- **C4 is the synchronous frame dump.** `PS3RECOMP_VK_DUMP` (ab_live every 10
  presents, a 2.7 MB PPM) ran `fopen/fwrite/fclose/rename` on the RSX walker
  thread inside present. Under btrfs writeback it blocked 0.5-0.7 s; the walker
  cannot tick, so the pump stops and the display waits out its fallbacks.
  Measured `stalltrace dump=717.6 ms` (the exact 717 ms "present/pump stall" in
  meter-p50-pin), `596.6` and `541.6 ms`; the fence/acquire/vkQueue present
  calls were all < 200 ms and there were no dropped frames.
- **Fix, default off:** `PS3RECOMP_VK_DUMP_ASYNC=1` (`vk_dump_queue.c`): the
  walker keeps the packed RGB copy and hands the buffer to a one-slot writer
  thread; a newer frame replaces an unwritten one. 33/33 video fixtures pass
  (new `test_vk_dump_queue`), ABI check passes, no `spu_context` field.
- **Confirm run (same config):** dump stalls in the meter phase 2 (608, 563 ms;
  0.90/min) -> **0**, and 0 `stalltrace dump` lines in 132 s vs 3 in the
  baseline. The one in-window stall left is the x11/FIFO compositor's
  `vkQueuePresentKHR` 804 ms (23 no-slot, recovered), not this code.
- **Not the dump:** the meter-start 0.25-2.1 s drains are first-use
  pipeline/shader translation (`pipe=1267 ms`, `pipecrt=101` in one report),
  before the measured segments. **C5 is not covered**: its 4.3 ms menu tail is
  the buffer-ref wait after the flip handler, a producer-cadence wait.
- Census: 1,594 stalls >= 200 ms in 211 runs; 160 inside measured meter windows
  in 68 runs. Full split in the doc.

## 2026-11-05 (c690 GPU Edge): key-on cost diagnosed; the hand-off, not the submit, costs 5.9 ms/frame

Full details in [`docs/edge-gpu-binding.md`](docs/edge-gpu-binding.md) §9. Branch
`exp/edge-gpu` in `ps3recomp-gpu` (commit `ff29ad46`), build
`recomp-work/build-gpu`. **Two pitch-meter launches (the cap)**; run 1 is
discarded (a concurrent mission build loaded the machine and the off arm kept
the previous drain's ranges). Code pushed to `fork` only; the binding stays
default 0.

- **Four arms in one clean launch** (`ab_live_gpudiag2_20261105_052030`):
  A key off, B export-only (`DBG=1`), C resolve-only (`DBG=2`), D full.
  A 37.22 fps / p50 34.15 / p90 38.91 / walker 9.08; B 35.94 / 34.15 / 39.62 /
  10.47; C 34.72 / 34.16 / 40.31 / 12.41; D 27.47 / 40.38 / 46.83 / 18.29.
  Per cause: export **+1.39 ms** walker, resolver **+1.94 ms**, GPU hand-off
  **+5.88 ms** (p50 +6.22, p90 +6.52). Total D-A +9.21 ms walker, -9.75 fps.
- **The previous "queue switch ~1.9 ms per submit" was not the submit call.**
  4,106 submits, **16.3 us/submit, 4.67/frame = 0.076 ms/frame, CPU wait 0**.
  Arena staging 0.29 ms/frame, export copies 0.10 ms/lane, resolver probe
  ~0.02-0.1 ms after the fix. The residual is draw-loop driver time while the
  compute queue is in flight (~1.26 ms per submit if attributed to the
  submits); this run does not separate queue/context switch from GTT/PCIe and
  driver serialisation.
- **Resolver probe fixed (default off):** two 4 KiB page bitmaps reject the
  range scan. Offline entries/probe drop from ~50-100 to 0.02-1.7
  (98-99.8 %); replay perf share 0.92 %. The binding-gate hashes are unchanged.
  Also: `PS3RECOMP_C690_GPU_2A_DBG` is now a live control-file key, and key-off
  retires the ranges/jobs (the old A arm inherited them).
- **Ceiling.** The 0x2A lane slice is 1.07 ms at 31.2 %, 2.3 ms at 68.2 %,
  3.43 ms at full coverage. The hand-off costs ~5.9 ms, so the binding cannot
  win until the compute is recorded in-stream (no separate queue, no GTT round
  trip) and the pack is replaced; then the net is ~0.6/1.8/2.9 ms, and 0x21
  sel7 (4.74 ms) plus the culls (3.03 ms) are what reach the 5-7 ms target.

## 2026-11-05 (c690 Edge): native 0x2A field pack; policy-exact on 112/131 pairs, live A/B neutral

Full details in [`docs/edge-native-2a.md`](docs/edge-native-2a.md). Branch
`exp/native-2a` in `ps3recomp-n2a`, build `recomp-work/build-n2a`. Code pushed
to `fork` only; `PS3RECOMP_C690_NATIVE_2A` stays default 0. **Two pitch-meter
launches (the cap)**: run 1 carried a per-layout lookup bug found afterwards,
run 2 is the measurement.

- **The pack is not `0x10C70`/`0x109C0`.** Those parse the stream descriptor and
  build the per-stream conversion/prefix table. The record assembly is four
  field passes: `0x7840` position copy, `0x7FC0` normal 11:11:10, `0x7AC0`
  fixed s16 (2/3/4 components), `0x7540` f16 x 2. The driver (index pre-pass,
  allocator, cursor) stays lifted.
- **Offline gates.** Entry hooks replace the four passes for the five
  position-first classes. On the 192-pair `fncap2b2w3` replay:
  **112/131 classified pairs (85.5 %) policy-exact** (positions 0 differing
  bytes, packed <= 1 LSB, no writes outside the field; 58.3 % of all sampled
  0x2A pairs). The 19 attribute-first pairs fall back byte-identical; the 30
  no-position pairs are exact and the 31 first-in-job pairs are identical to
  key-off. GPRs: r0/r1/r80..r127 exact, only caller-saved r2..r79 scratch
  differs. New unit test PASS; fixtures 31/32 (`test_gcm_tick_in_drain` is a
  timing fixture that also flakes on the base tree); ABI PASS.
- **Live A/B (run 2, `ab_live_native2b_20261105_044645`).** A 35.15 fps / p50
  34.15 / p90 38.35 / p99 41.23; B 35.77 / 34.15 / 38.12 / 41.04; paired
  **-0.59 +/- 2.75 fps** (CI [-3.34, 2.16]). Per-command **0x2A 21.92 ->
  21.59 us/call (-0.33 us, -1.5 %)**; ~0.18 ms of the total c690 lane time per
  frame, below noise. Class-level fallback 18.4 %, pass-level native 56 %.
  Thumbnails unchanged.
- **Offline per class:** 20-byte -9.9 us (-43 %), position-only -5.8 us
  (-38 %), 44-byte -6.5 us (-22 %), but the largest class (32-byte
  position-first) is **+2.4 us (+11 %)** with the scalar pack. That offsets the
  gains in the live class mix.
- **Implication.** A perfect GPU pack replacement inherits a ~0.2-0.5 ms/frame
  ceiling at this coverage, while the measured GPU hand-off costs 5.96 ms/frame
  (and the async queue still loses ~7.6 ms at p90). The pack is not the meter
  bottleneck; the driver/pre-pass and the latency-bound burst are. 0x21
  (4.74 ms, pure compute) is the better target, but only as a whole-command
  replacement. Next: vectorise the 32-byte pack, then attack the whole 0x2A
  command or 0x21.

## 2026-11-05 (c690 GPU Edge): asynchronous 0x2A hand-off; submit 0.061 ms/frame, p90 still pays the compute queue

Full details in [`docs/edge-gpu-binding.md`](docs/edge-gpu-binding.md) §8. Branch
`exp/edge-gpu` in `ps3recomp-gpu`, build `recomp-work/build-gpu`. **Two
pitch-meter launches (the cap)**; the key stays default 0.

- **What changed.** The c690 compute now runs on a dedicated compute-only
  queue family (family 2 on the RTX 3060 Ti) with a timeline semaphore. The
  walker submits at the drain boundary and never waits: 4,525 submits,
  **14.8 us per submit, 0.061 ms/frame** over 1,104 key-on frames, **wait 0**
  (the target was <= 0.5 ms/frame). Jobs of a drain are one dispatch set.
  The resolver serves the exact SPU seed until the producing timeline value is
  observed complete, then the GPU bytes; no CPU reader blocks.
- **Run 1 found two things.** The instance was Vulkan 1.0, so the core
  timeline-feature query silently reported false and the binding used a fence;
  and waiting at the read on a compute-queue batch costs **2.48 ms/batch**
  (p50 74.1 ms). Run 2 fixed both (instance 1.2, no CPU wait).
- **Run 2 A/B** (`ab_live_c690gpu2e_20261105_043438`): A 37.12 fps / p50 34.15
  / p90 38.78 / p99 42.57 / walker 8.46; B 27.60 / 34.65 / 46.43 / 50.88 /
  15.14. p50 +0.50 ms, p90 +7.65, p99 +8.31, fps -25.6 %. Thumbnails clean.
  The remaining cost is **~1.9 ms of graphics time per compute-queue submit**
  (a queue-family switch), not the fence and not the maths (~0.12 ms/frame at
  the phase-2b rate). Recording the dispatches into the frame's own command
  buffer before the first reading draw is the way to remove it, but it only
  pays once the draw consumes the GPU bytes; the SPU still packs.
- **Gates.** Race harness ASan/UBSan + TSan clean (192 pairs, 5 lanes with
  `spu_id 0`), `test_c690_gpu_2a` PASS, fixtures 30/30, ABI check, replay hash
  unchanged, offline Vulkan validation arm 200 rounds **0 errors** (it caught
  a compute-queue barrier with a vertex stage; fixed). The second launch was
  the A/B, so the validation arm is the offline harness, not an in-game run.
- **Next.** Replace the pack (`exp/native-2a`) and make the draw consume the
  GPU range; fold the compute into the frame stream; then the other six
  classes (68.2 % offline), 0x21 sel7 (4.74 ms) and the culls (3.03 ms).


## 2026-11-05 (c690 GPU Edge): 0x2A live crash fixed, one clean A/B; overlay costs 5.96 ms/frame

Full details in [`docs/edge-gpu-binding.md`](docs/edge-gpu-binding.md) §7. Branch
`exp/edge-gpu` in `ps3recomp-gpu`, build `recomp-work/build-gpu`. **One
pitch-meter launch (the cap), no crash.** Code pushed to `fork` only;
`PS3RECOMP_C690_GPU_2A` stays default 0.

- **Root cause (from the run-2 core, still in `coredumpctl`).** The abort is on
  the RSX FIFO thread in `vk_c690_prepare` -> `vk_c690_job_free`, freeing a
  job buffer already freed. The lane key was `ctx->spu_id % 16`, but the c690
  jobs run through `spu_workload_dispatch_job` -> `spu_run_spurs_job`, which
  builds every lane's `spu_context` with `spu_context_init(&ctx, 0)`. Run 1's
  log ends with `[c690-stats] worker=0..4 spu=0`, so all five concurrent lanes
  shared `s_lane[0]`; one lane's exit/export could free or overwrite a buffer
  another lane had already queued.
- **Fix.** Lane state is thread-local (a lane is one host thread, which runs
  the job and its submit epilogue PUT synchronously); `g_c690_gpu_2a_on` is read/written
  with relaxed `__atomic` loads/stores; seed-fallback ranges now retain their buffers until the next
  drain clear; requeued jobs hand ownership over explicitly; the release path
  frees queued/retained jobs after clearing the ranges. Debug isolation values:
  `PS3RECOMP_C690_GPU_2A_DBG=1` export-only, `=2` resolve-only (exact SPU seed,
  no Vulkan).
- **Harness.** `ps3recomp-gpu/tests/test_c690_gpu_2a_race.c` and
  `tools/run_c690_gpu_2a_race.sh`: five lanes all with `spu_id 0` drive the
  real export/PUT path over the 192 captured `fncap2b2w3` tail pairs while a
  walker drains. The old source fails ASan with a heap-use-after-free in the
  drain (the live crash); the fixed source is clean under ASan/UBSan and TSan.
  Unit test, `test_video_fixtures` 30/30 and the ABI check pass.
- **Live A/B** (`build-gpu`, 5 x 10 s, drop 2 s, one X): A 30.38 fps / p50
  34.23 / p90 43.90 / p99 48.24; B 22.93 / 34.16 / 68.60 / 72.98. B is not a
  win, as expected (the SPU still packs). The path's own cost is the number:
  3,042 submits, 162,849 jobs, 61.2 M records, **2.41 ms submit+fence per
  drain, 5.96 ms/frame** (~98 % queue/fence serialisation; GPU compute is
  ~0.08 ms/frame scaled from the phase-2b burst). p50 is unmoved; p90/p99 rise.
- **Next.** Replace `0x10C70`/`0x109C0` so the SPU stops packing **and**
  pipeline the fence (timeline semaphore, walker waits only at a draw that
  reads the range). Estimated saving: ~1.07 ms at the current 31.2 % class
  coverage, ~2.3 ms at the 68.2 % now classified, 3.43 ms at full 0x2A;
  0x21 sel7 (4.74 ms) and the culls (3.03 ms) need the same async binding to
  reach the 5-7 ms Edge target.

## 2026-11-05 (c690 GPU Edge): 0x2A record-class decode + per-class kernels, 68.2 % coverage

Full details in [`docs/edge-gpu-2a-classes.md`](edge-gpu-2a-classes.md). **Offline
only: zero game launches**, no `ps3recomp-gpu` changes; the walker-binding
mission's worktree and the existing `c690_2a_32b.*` files were not touched.

- **Ten record layouts decoded, not four.** The 192-pair meter spool is 60
  position-first 32-byte, 17 twenty-byte, 17 position-only (the 48-byte family
  is four consecutive 12-byte positions), 19 attribute-first (nine 32-byte /
  ten 28-byte), 13 forty-four-byte, 5 twenty-eight-byte position-first, plus
  31 first-in-job pairs that enter the low-LS SPURS runtime and 30 outputs with
  no positions. `0x00A00004` is 50/192 = 26 %, not 51 %; only 19 of those are
  vertex records.
- **Seven class kernels (one shared `.inc`) are exact.** 131/131 classified
  pairs, 45,440 records: positions 0 differing bytes, packed <= 1 LSB, the
  SPU-written prefix/attribute bytes untouched, 0 GPU writes outside the
  records. The 9 packed-offset 32-byte pairs are included (same layout; the
  exported index is a u32 source entry, not the `cmd[0]` u16 list).
- **Coverage 131/192 = 68.2 %** (was 60/192 = 31.2 %). Fallback 61/192 =
  31.8 %: 31 first-in-job runtime-path (16.1 %), 19 attribute/index-only
  (9.9 %), 11 rare 64/40/16/4/112-byte layouts (5.7 %). The 5 % gate is still
  not met; the first-in-job path cannot move until the low-LS runtime is
  modelled or the SPU exports its result.
- Binding interface: per job it needs the IN LS, up to seven stream bases,
  a u32 per-record source entry index list, `out_base`, `count`, `record_size`
  and the class selector; the kernel writes only the class fields in the ring
  range. The current `PS3RECOMP_C690_GPU_2A` export covers only A/B.

## 2026-11-05 (c690 GPU Edge): 0x2A binding + live key; offline gates pass, live A/B crashes

Full details in [`docs/edge-gpu-binding.md`](docs/edge-gpu-binding.md). Branch
`exp/edge-gpu` in `ps3recomp-gpu` (merged `exp/live-ab`), build
`recomp-work/build-gpu`. **Two pitch-meter launches (the cap), both crashed on
the key-on arm; no A/B numbers.** Key `PS3RECOMP_C690_GPU_2A` is default 0.

- **Binding built.** The 0x10F78 wrapper exports the four source arrays and
  the A/B offset list; the renderer runs the proven 32-byte Vulkan kernel
  (one command buffer, one submit, one fence per drain); the draw engine's
  guest resolver serves the GPU-written mesh-ring range. Any other class is
  not exported. Guest memory and the lifted output are unchanged (overlay).
- **Why the pack is not skipped yet.** The first store-suppression design was
  exact on a synthetic body but the real handler reads back the record it
  writes (it stored the pre-body bytes over the correct record once the store
  was suppressed). The safe stage is the overlay; the skip needs a native
  per-stream pack (`0x10C70`/`0x109C0`).
- **Offline gates pass.** Kernel 51/51 (20,312 records, positions 0, packed
  <= 1 LSB, attr 0); compact-arena mirror PASS for A and B; resolver
  transparency through `replay_rsx_engine` gives the same hash as the guest
  lifted bytes (`44f962cef2a40ae3`, gate `7463a05e53fffdb7`) and differs only
  by the approved packed drift for the GPU bytes; unit test, 30/30 video
  fixtures, ABI check; ASan stress 1.2 M jobs clean.
- **Live blocker.** Both launches (03:11, 03:17) aborted with glibc
  `double free or corruption` within ~0.1 s of the first B switch, after the
  compute pipeline was created. Coredumps are truncated; the aborting thread's
  stack is unreadable. Per the repeat-failure rule and the cap, no third
  launch. Next: isolate with compute off / resolver only, fix, then replace the
  pack and re-run §6.

Newest first. Numbers are from one Linux PC (Ryzen 5 2600X, 12 threads, 32 GB, Fedora 44, clang 21) unless noted.
ps3recomp at upstream `4e8f3ca` (2026-10-26) with `mlb16-work`, `feat/presentation-thread`, and `feat/gpu-vertex-offload`.

## 2026-11-05 (capture tools): walker-prof sampling and full guest imaging fixed

Full details, tests and the user command are in
[`docs/capture-tools-fix.md`](capture-tools-fix.md). Branch `exp/capture-tools`
in a new `ps3recomp-tools` worktree, build `recomp-work/build-tools`. One
automated pitch-meter launch (the cap), X once; no RPCS3 source read or cited.

- **Profiler fix.** The sampler chose a whole frame from the flip counter, but
  the timed work is in a drain: a sampled drain could be empty while the
  window's draws sat in unsampled drains, printing `engine=0.000` with
  `draws>0`. Sampling is now per drain, scaling is `drains_total /
  drains_prof`, and a report waits until a sampled drain has produced draws
  (or the window is genuinely idle). Old log 322/1,698 bad lines (19.0 %);
  the new live log 0/2,709. New synthetic fixture `test_walker_prof`.
- **Capture fix.** The live capture could miss the tail of a 2 MB mesh-ring
  vertex read. Three causes: M3's cold-start notes ran before the capture was
  armed and were then cleared; the `.mem` writer extrapolated one EA across a
  range although the IO table maps main memory per MB; and the live frame can
  name a narrower span than a cold replay resolves. The capture now arms
  before noting, images every mapped main-memory IO page at begin, expands
  every main read to its whole 1 MB IO page, and resolves each page's EA on
  its own. New fixture `test_walker_capture`.
- **Numbers.** Old crowded `walker_replay`: 1,261 backend draws/frame,
  4,547 read misses. The same frame with the new page set: **1,694 backend
  draws (1,702 sink groups, the live count), 0 misses**. A live meter capture
  with the new code (45,279 pages, 176.9 MB) replays with **0 misses**.
  `meter_frame`/`walker_frame` hashes unchanged
  (`a6fcc849341eba9b` / `416abee2c5bff47d`); fixtures 32/32 (the pre-existing
  timing-sensitive `test_gcm_tick_in_drain` flaked under load, passes alone).
- **Next.** User runs the crowded-replay recapture in §5 of the doc; gate is
  `draws/frame >= 1300` and 0 misses. Nothing new is default-on.

## 2026-11-05 (c690 GPU Edge): 32-byte 0x2A index contract and exact kernel; live A/B blocked

Full details in [`docs/edge-gpu-2a-32b.md`](edge-gpu-2a-32b.md). Two pitch-meter
launches, both capture-only (the cap); **no live A/B**. Branch `exp/edge-gpu`
in a new `ps3recomp-gpu` worktree, build `recomp-work/build-gpu`; `build-play`
and `recomp-work/spu_lifted` were not touched.

- **Index pre-pass decoded.** The 0x2A handler gathers all four planar arrays
  at one per-record u16 byte offset: `cmd[0][k]` (format A) or the implicit
  `k*16` (format B). The 4 bytes at record `+28..31` are SPU-written.
- **Kernel built and gated.** `c690_2a_32b.comp` writes record bytes 0..27;
  on the regenerated meter spool it is **51/51 pairs, 20,312 records**:
  **0 differing position bytes**, **max 1 LSB** normal/tangent/UV, the SPU
  attribute word exact, 0 writes outside 0..27, ring position/attribute bytes
  exact. The 36-byte regression is still 0 differing bytes / 0 ring bytes.
- **Coverage is 31.2 %, not 43 %.** `STRIDE=1000` sampling over the meter
  window: 60/192 32-byte, 1/192 36-byte, 17/192 20-byte, 16/192 48-byte,
  98/192 other. The 32-byte stage leaves 68.8 % of 0x2A on the SPU (gate 5 %).
- **No live A/B.** There is no `PS3RECOMP_C690_GPU_2A` key or walker binding
  on any `ps3recomp-play` branch (`git grep` zero hits); §4 is still design.
  Both the missing wiring and the 68.8 % fallback violate §6's preconditions.
  The two launches went to regenerating the deleted corpora. The measured
  stage is ~1.07 ms of the 24.39 ms critical lane; next: walker binding + live
  key, then the remaining 0x2A classes, 0x21 sel7 (4.74 ms), the culls
  (3.03 ms); 0x22 (5.80 ms) is already CPU-native.

## 2026-11-05 (Bink video): the intro never opened because the fd spaces were split

Full details, the two fixes and the user check are in
[`docs/bink-video.md`](bink-video.md); the team-logo pointer is in
[`docs/team-logo.md`](team-logo.md) §11. Two automated launches (the cap);
no GPL source read or cited.

- **Failure site, from a live write-watch:** after `MLB_INTRO.BIK` opens on
  fd 12, BinkOpen's header read returns no data and the SDK writes
  **"Not a Bink file."** to `0x01476028` at `0x00EE5B10`. No `cellFs`/AIO read
  of fd 12 ever happens.
- **Cause:** the Bink SDK's default PS3 IO (`0x00EE7D00` open, `0x00EE8690`
  read) uses raw `sys_fs_lseek`/`sys_fs_read` (`0x00A41F1C`/`0x00A420A0`) on
  the fd the game opened with **cellFsOpen**. On hardware both share one
  kernel fd space; this runtime had two tables, so the sys_fs slot was empty
  and the read returned `EBADF` silently.
- **Fix 1 (movie open):** `sys_fs_read/lseek/fstat` bridge to
  `ppu_cellfs_file_for_fd` when the fd is not a sys_fs one; both allocators
  avoid the other table's numbers. Offline: `tools/test_sys_fs_cellfs_bridge.py`
  (12 PASS).
- **Fix 2 (decode):** the Bink SPU task was created with `szctx=0 lsp=0` and
  parked in `WAIT_SIGNAL` because `_cellSpursTaskAttributeInitialize` read the
  R8 form while `FUN_00EF8494` passes a `{context EA, size, lsPattern}`
  descriptor in r7 and r8=0. Auto-detect now decodes that shape when r8==0
  and the words are plausible. Offline:
  `tools/test_spurs_taskattr_desc.py` (18 PASS).
- **Live status:** the run with the stricter SPURS bounds still failed at
  `01:52:29` and showed the Bink IO table is the default `0x010D9760`; the
  automated nav still reached the pitch meter. The fd bridge landed after the
  2-launch cap, so it is offline-tested but **not live-verified**. User check:
  bink-video.md §6.

## 2026-11-05 (team logo): the resolver is a `BINK_LOGO` asset, not a mesh path

Full details and the user command are in [`docs/team-logo.md`](team-logo.md) §10.
**Static only: no game launch.** EBOOT in Ghidra, the four WADs streamed, the
existing team-select FIFO capture, and the 2026-11-05 logs.

- **Premise correction:** the capture's 0 triangle batches rule out a mesh, not
  a logo. The same frames carry 107 primitive-8 quads with 1024x1024, 512x1024,
  540x960, 341x683, ... textures. The 3D logo can be a Bink/UI-movie sprite or a
  rotated texture card; both are quads.
- **Resolver:** `UI_ResolveTopLevelAssetPath` (`0x000f56f4`, old `FUN_000f56f4`)
  is called from the UI property dispatcher `.opd.FUN_00297b60` for case 0xf.
  Its 8-entry asset-kind table at `0x01126600` names entry 2 **`BINK_LOGO`**;
  case 2 builds `logos\logo3d_<TEAM>` from `0x01177574`. The in-game twin is a
  `MovieSprite` (`UI_InGame3DTeamLogo` in the `IFF0ASSH` at 3,983,857,506).
- **Team select:** the `IFF0ASSH` at 1,188,031,035 (3.53 MB) has `TEAM_LOGO` and
  `[TeamSelect_Wheel`, no `ScriptModel`/`MovieSprite`; the logo is a run-time
  script property. The 00:47 log creates two 1024x512 colour surfaces at
  `00:47:27` (consistent with a movie/RT double buffer).
- **HLE:** no gap found. >4 GiB WAD I/O is 64-bit and tested; GCM config/tile/
  zcull, video out, and SPURS are implemented; the 00:47 log has no
  `[RSX fp]`/`FAILED (draw dropped)`/`TEXTURESET`/Bink error.
- **No fix was implemented**: no concrete runtime bug was proven. The next run
  watches the `logo3d` table (`PPU_RWATCH=0x01177574,0x228`) and dumps the FIFO
  at a **team switch** (the settled capture can miss a change-time 3D/RT pass).
  Command and greps: team-logo.md §10.6.
- **Ghidra renames:** the resolver/context, wheel, `UI_Movie_LoadBik`,
  `Model_LoadChunked`, `ScriptModel_Load`, `Model_Bind`, `Model_LoadChunk`,
  `Mesh_EmitTriangles`, `RenderTarget_Create`, and the ScriptUIElem render/copy
  functions (team-logo.md §10.7).

## 2026-11-05 (uniform moire): sampler census done, 2x1 derivative candidate is not it

Full details and the user command are in [`docs/uniform-moire.md`](uniform-moire.md).
Two pitch-meter launches (the cap), build `recomp-work/build-play`, branch
`exp/live-ab`; no GPL source read or cited.

- **Ruled out by the offline meter census** (`PS3RECOMP_ENG_SAMPLER_DUMP`):
  54 distinct sampler states, LOD bias **0** on all of them, full mip chains
  uploaded, min/max LOD matching the level count. The Vulkan 64-entry sampler
  table never fills in a 1.5-minute live meter run (no overflow warning), so
  the inFAMOUS-style "default nearest/level-0 sampler past 64" bug is not
  this scene.
- **Strongest remaining lead:** the cloth units request 2x-8x anisotropy
  (`TEXTURE_CONTROL0[6:4]`) and the Vulkan sampler ignores it, while the
  D3D12 backend honours it and the MIT ports keep an aniso path.
- **Candidate implemented, default off:** `PS3RECOMP_FP_AA` scales DDX/DDY
  and implicit 2D samples by the 2x1 sample grid (RSX derives per pixel, we
  derive per sample). Key off digest unchanged (`FP_TXD=1`:
  `3c3b3b85532a2ff5` / gate `0a9220f49d76e843`); key on a new digest; 51/51
  dumped shaders compile; `test_fp_decompiler` covers the text.
- **The A/B says no:** N=4 x 10 s per arm, fps **35.56 -> 30.44**, p50
  **27.2 -> 29.5 ms**, p90 **34.3 -> 38.0 ms**, paired +5.13 fps (CI
  -1.37..+11.62). Matched pants/jersey crops differ at the A/A level, so the
  derivative scale is not proven and costs fps. The 640x360 scene thumbnails
  are not a valid judge for this fine-detail bug; the user check in
  `uniform-moire.md` section 6 is the gate.
- **Next mission:** implement guest MaxAnisotropy in the Vulkan sampler
  behind a key, and dump the pants program's texture at a close-up. The meter
  capture cannot decode the character textures offline (78 texture-page read
  misses).

## 2026-11-05 (depth of field): the RPCS3 blur patch runs in the recomp

Full details and the user command are in [`docs/dof-patch.md`](dof-patch.md).
New code on ps3recomp `exp/dof-patch` (fork only), build
`recomp-work/build-dof`; `PS3RECOMP_DOF_PATCH` default 0. One game launch.

- **The patch bytes apply at ELF load.** `ppu_load_elf` writes the user's
  "Better depth of field" program (0xA90 bytes, 168 slots through END) to the
  unused ELF gap at 0x10A4400 and repoints both EBOOT program headers
  (0x1121cc0, 0x1122080). FNV-1a 64 fingerprints of the original headers and
  0x340-byte programs are checked first; a mismatched EBOOT, a nonzero target
  or a header that does not follow is refused and the original program runs.
  The backend keys pipelines on fragment-code bytes, so nothing caches the old
  program before the patch.
- **Offline proof.** The generated 0xA90 program decodes as 148 instructions
  with no unhandled opcode and its HLSL translates to SPIR-V for fp16/fp32
  export control and with gamma/sign-expand state on the three sampled units
  (`test_dof_patch`, 25/25 checks). Given the EBOOT, the same test applies the
  patch, checks both headers follow to 0x10A4400 and the bytes match, and
  checks the refusal paths.
- **No capture has the DOF pass.** The `.fifo` and `.mem` captures were
  scanned: the original program appears at guest 0x280 in `walker_replay` /
  `logo_cap`, but no draw references it and no captured local-memory page
  holds the game's run-time copy. The decompile/compile + EBOOT-apply test is
  the offline proof; there is no replay A/B of this program.
- **Key-on meter launch.** `[dof] applied ...` at boot; meter reached, X once.
  First four A/A segments 33.4/36.2/34.3/36.7 fps, in line with the A/A
  baseline (~33-36). Later segments fell to 27/19/16/16 fps while another
  mission relinked `build-play/MyGameRecomp` at 01:16:12 during the
  measurement; those are contaminated, not a patch result. No crash, no
  thumbnail corruption. User visual check (two short launches, key off/on) is
  in `docs/dof-patch.md`.


## 2026-11-05 (team logo): the 0x900 lookup is a world model, and the debug line goes to BADJUMBOS_PS3.TXT

Full analysis in [`docs/team-logo.md`](team-logo.md) §8. **Zero game
launches**; Ghidra, all existing `recomp-work` logs, the four ISO WADs, the
generated C++, and `ps3recomp-play` read only. No RPCS3 or other GPL source
read or cited.

- **The 0x900 node is a model.** `ScriptUIElem_FindTargetTexture`
  (`0x009d4c68`) walks the `"world"` scene's type-`0x900` children, keeps
  `node+0x18 == 0x100`, and iterates the model's materials
  (`Model_GetMaterial`, `0x004c5024`) and their two texture names
  (`TextureSet_GetNameById`, `0x004f8cf8`). The element's `targetname`
  (`elem+0xcc`) must match one of those names. This is the
  jumbotron/scoreboard-screen path: all 905 GD + 223 EXT plain-text
  `ScriptUIElem` definitions target stadium screens (`color_2x1`, `amber_5x1`, `k`, ...), and
  there is no `logo3d_` literal in any WAD. Whether the team-select floating
  logo is a `ScriptUIElem` is now the open question, not an assumption.
- **The miss text is not in the log.** On a miss the title writes
  `"%s (%s): Did not find target texture: %s\n"` to `BADJUMBOS_PS3.TXT` in the
  guest cwd and only prints `FAILED`; it also skips both for target names
  exactly `k`/`kled`. `Debug_Printf` is on by default (`noTTY` disables it).
  This corrects team-logo.md §5.5.4.
- **The old config command cannot work.** The ISO is mounted `ro` and
  `recomp-work/vfs/PS3_GAME` is a symlink to it, so `printf > .../BOOT/
  CONFIG_MAIN.TXT` is `EROFS`. §8.3.2 gives a throwaway writable overlay that
  redirects only `/dev_bdvd`; `/dev_hdd0`/`/dev_hdd1` stay on the normal vfs, so
  saves and caches are unchanged. §8.3.1 is a no-config `PPU_WWATCH` run on
  `0x013560d8..0x013561f0` (element counter, `DisableUIElem`, id counter).
- **No HLE cause found.** `cellFsAioRead`/`Write` succeed in `drive5.log`
  (GD WAD `fd=7`, 52,912-byte reads); `cellFsStat` fills the ABI struct; the
  lookup itself has no HLE call; generated `func_009D4C68`/`func_009D7B30`
  match the EBOOT branches. No runtime fix is justified yet.
- **Next:** the user runs §8.3.1 (or §8.3.2 for the target name). If the
  element counter never rises, the next path is the frontend 3D model, not
  `FUN_009d4c68`.
- Ghidra renames: `ScriptUIElem_FindTargetTexture`, `ScriptUIElem_Create`,
  `TextureSet_LoadAsset`, `Model_GetMaterial`, `TextureSet_GetNameById`,
  `Scene_GetOrCreate`, `Scene_GetChildByType`, `Config_LoadFile`,
  `Config_HasFlag`, `Debug_Printf` (see team-logo.md §8.5).

## 2026-11-05 (jerseys): the torso was two dropped character draws, not a texture

Full analysis in [`docs/jersey-transparent.md`](jersey-transparent.md).
Offline replay of the existing `meter_frame` capture plus one pitch-meter A/B
(`ab_live.sh`, N=4 x 10 s per arm). No RPCS3 or other GPL source read or cited;
the NHL Legacy and inFAMOUS 2 MIT notes were checked for the alpha /
render-to-texture classes and did not match.

- **Cause.** The character fragment programs use `DDX`/`DDY` then `TXD` on a
  gamma-flagged unit. The decompiler had no `TXD`/`DDX`/`DDY`, and a gamma
  `TXD` returned `-1`, failing the whole fragment program; the engine dropped
  every draw through it (`FAILED (draw dropped)`). The jersey torso is those
  draws: **23 per frame** at the meter (replay 502 off vs 525 on).
- **Fix.** `TXD` -> `SampleGrad(coord, ddx, ddy)`, `DDX`/`DDY`, and the
  existing gamma / `EXP_TEX` wrappers on `TXD`, all behind the live key
  `PS3RECOMP_FP_TXD` (default off). The key is part of the pipeline identity,
  so a live flip rebuilds the programs. Key-off replay digest is unchanged
  (`meter_frame` 64 frames: `461ff3a834c62331` / `089e7b1082b4bcc3`); key on is
  `3c3b3b85532a2ff5` / `0a9220f49d76e843`, 525 draws/frame. New checks in
  `test_fp_decompiler` (92 pass); `test_video_fixtures` 30/30 serial (the
  pre-existing parallel `test_gcm_tick_in_drain` flake).
- **Meter A/B.** fps **37.88 -> 36.38**, p50 **34.14 -> 34.15 ms**, p90
  **36.72 -> 38.62**, p99 **39.79 -> 41.78**, walker **7.55 -> 9.60 ms**
  (paired -1.50 fps, 95 % CI -3.68..+0.68, N=4). The cost is the 23 restored
  character draws; the bug had been hiding ~4 % of the meter's character work.
  The per-arm crops show the torso transparent off and the white jersey with
  name/number on (`recomp-work/visuals/jersey_txd/`).
- **Next:** the user's 5-minute check in `jersey-transparent.md` §6 (pitch
  meter + batter walk-up, live flip); decide whether to make it a play
  default. The key stays off until then.

## 2026-11-05 (auto-exposure): the 1x1 chain, a high-gain dead band, and a timing-dependent read

Full analysis in [`docs/exposure-flicker.md`](exposure-flicker.md).
**Zero game launches**, no runtime or build changes; static Ghidra on
`EBOOT.elf` plus the existing light/WCB logs. No RPCS3 or other GPL source
read or cited; NHL Legacy and inFAMOUS 2 MIT notes used with attribution.

- **Chain named.** `Lighting_InitStadium` creates `ToneMapSample` (32x32,
  6 mips, A8R8G8B8); `Lighting_RenderExposureRT` renders a temp camera into
  it and runs `ToneMapMipGen`; `Lighting_UpdateAutoExposure` (`0x004f3590`,
  renamed) runs on the PPU, locks the **last mip (1x1)** through the asset
  library (`Asset_LockTextureLevel` -> `Asset_TextureLevelPtrFromTable`, a
  plain guest pointer) and reads its first two bytes with ordinary loads.
  No SPU job and no GCM read in the path. The controller is a dead band:
  the byte above/below the per-mode threshold drives exposure at **62.5/s**
  (`dt` clamped to 1/60..1), clamp **0.3..1.1**, then
  `powf(..., 0.454)*255`. One sample on the wrong side moves the whole LUT.
- **Cadence/latency.** The chain is re-rendered every frame but read only
  ~0.32-0.35 times/frame per level (about every 2-3 frames), tied to
  `Lighting_UpdateTimeOfDay` (`0x004f3ac8`). It has no explicit sync and
  assumes the RSX has left the previous frame's chain in main memory (the
  title allows one render frame in flight).
- **What our readback actually did.** In every existing probe session the
  exposure chain is served fresh (`fresh=1`, serial advanced) and the 1x1
  luma is a steady 78-80 on the meter; the 11-02 user session that showed
  the drift has 80.5-82.1 at the reported moments, and the only zero/large
  swings are scene transitions/camera cuts. The "stale/zero on alternate
  frames" premise is **not reproduced** by any capture.
- **The hole.** An ondemand copy is made when the walker services the
  request (`eng_wcb_copy_surface`, `rsx_draw_engine.c:3784`), so which
  frame the PPU gets depends on how far the walker has advanced. On the
  meter the cadence is regular and the value flat; in live play the walker
  position varies and the high-gain controller turns one wrong sample into a
  visible step. `PS3RECOMP_RSX_WCB_PREFETCH` (already implemented, default
  off) makes the read the sync-point snapshot instead: measured in
  `wcb-async.md` at 32x32 copy **0.955 -> 0.117 ms/f**, guest wait
  **2.50 -> 0.93 ms**, walker **-4.2 %**, meter p50 unchanged.
- **Over-bright field** is either a zero 1x1 sample (NHL Legacy documents
  the same "1x1 luminance RTT read as 0 -> blown to white" failure; our
  logs have exact-zero runs at scene changes) or a separate
  tonemap/sRGB/format issue if the 1x1 is stable and non-zero.
- **Also check the cube/ambient path.** The 11-02 visual changes coincide
  with the scene-setup 32x32 sky reads (`0x018C9200`), one of which is pure
  black every time, while exposure is stable; the player-lighting half may
  be the cube probe, not exposure.
- **Fix proposed:** always serve the exposure chain (32x32 and <=4x4) from
  the last sync point, keep the first read synchronous, optionally batch
  the six levels. The one-launch pitch-meter A/B command (prefetch 0 vs 1,
  `PS3RECOMP_LIGHT_PROBE=1`, what to grep) is §5 of the doc; not run here.
  Correctness fix should be neutral-to-positive; a forced synchronous
  per-level read costs the existing ~1.3 ms copy + ~1.4 ms wait/frame.
- Ghidra renames: `Lighting_UpdateAutoExposure`, `Lighting_UpdateTimeOfDay`,
  `Asset_LockTextureLevel(Impl)`, `Asset_TextureLevelReadPtr`,
  `Asset_TextureLevelPtrFromTable`, `Asset_UnlockTextureLevel`,
  `Lighting_ToneMapMipGen` (table in the doc).

## 2026-11-04 (30 fps investigation): no active cap, sim is elapsed-time driven, 60 is a speed problem

Full analysis in [`docs/fps30-investigation.md`](fps30-investigation.md).
**Zero game launches**; static Ghidra analysis of the EBOOT plus existing logs
and docs only. No build and no runtime change.

- The title's own frame clock (`Engine_FrameClock`, `0x0005F120`) measures the
  79.8 MHz timebase, sleeps to a target period, and publishes
  `g_engine_frame_dt` as `max(param_1, floor((residual + elapsed) * 60)) / 60`
  seconds, carrying the sub-1/60 residual. `param_1` = 1 for 60 fps mode, 2
  for 30 fps mode. The main loop (`Game_FrameLoop`, `0x002ED360`) hands
  `g_game_frame_dt * g_game_time_scale` to every gameplay update, so at 16.7 ms
  frames dt = 1/60 and the sim is real time, not 2x.
- The pitch meter is dt-scaled (`Pitch_ActionUpdate` `0x00BE16B0` accumulates
  `+0x29BC` from `Pitch_GetFrameDelta` `0x006323B0` =
  `g_game_frame_dt * g_game_time_scale`). The older `boot-spu-hitches.md` §4
  "fixed per-update increment" reading is superseded.
- The title has a real 30 fps mode, selected by two flags that are off here:
  `g_engine_fps_setting_index` is 6 (60 fps) because the Gfx display-thread
  flag (`gfx_ctx+0x4E4`) is 1, and `Engine_FrameClock` only promotes `param_1`
  to 2 when `cellPadGetInfo2().system_info & 1` is set, which our HLE writes
  as 0 (`cellPadGetInfo2`, `libs/input/cellPad.c:1250`). **Do not set that
  bit.**
- The only other frame-time wait is the known flip deadline
  (`2 * DAT_011203F8`): 16,683 us (33.366 ms) in 720p, 20,000 us (40 ms) in
  the 576-line mode; written only by video-mode configure `FUN_00064B68`, not
  per scene. No `1/30`, `33333 us` or 2-vblank constant feeds the sim.
- Verdict **(a)**: no cap active; 60 fps is a pure speed problem. Above 60 fps
  the dt floor would make the sim run fast (<= ~5 % at the title's own ~63.2
  fps limiter); at 60.0 fps it is exact. One-run confirmation command (not
  run): `PPU_WWATCH=0x01132F7C` and check the stored value is 1 at game entry.

## 2026-11-04 (Edge E1 live): merged, fps neutral, but the added triangles draw giant occluders

Full analysis in [`docs/edge-e1-holes.md`](edge-e1-holes.md) §5.4. E1
(`exp/edge-e1-holes` `2b5f17d1`, fused native 0x25/0x26 holes, default off)
merged into `exp/live-ab` as `bba9787a` on top of `4ba548e6`; the shared
generated c690 module was re-patched and is **byte-identical** to the verified
private `spu_lifted-e1` module. Rebuilt `build-play`; no game was running.
Offline gates re-run: `test_c690_holes_merge` OK, native-screen holes gate
**128/128 PASS** (0 removed, 0 added, full LS/GPR = lifted union, counts and
buffer extent consistent), `check_spu_context_abi.sh` PASS (no `spu_context`
field added). `test_video_fixtures.py` is **29/30**: the one failure is the
pre-existing timing-sensitive `test_gcm_tick_in_drain`
(`test_gcm_tick_in_drain.c:170`, a 1 us margin; it fails identically with and
without the E1 merge, same source as `4ba548e6`).

One pitch-meter A/B (`ab_live_e1holes_20261104_225522`, 12 segments,
N=6 x 10 s, `MLB16_CULL_BYPASS` unset):

- fps **36.04 -> 35.71** (paired **-0.33 +/- 1.26 SE**; -0.16 +/- 0.54
  excluding the two segments hit by a ~1 s in-game stall), p50
  **34.14 -> 34.15 ms**.
- Tail worse in every B segment: p90 per-segment median **38.95 -> 41.20 ms**,
  p99 **42.67 -> 45.34 ms** (ship gate "p90/p99 not worse" fails).
- c690 wall/s **2.75 -> 3.04 s**, us/job **200.5 -> 223.9**, lane busy
  **0.560 -> 0.614**.
- Holes counters (B windows only): **374,001** sample runs, **121,242** added
  triangles, **253,711** fallbacks, **175** cap-skips, 37.47 s of sample-run
  time.
- **Visual regression:** arm B draws giant brown occluder polygons over the
  pitch view; A does not. Thumbnail mean-abs A/A **2.18**, B/B **30.01**,
  A/B **18.36**. B differs from A only by `PS3RECOMP_C690_HOLES_NATIVE`, so
  the added sample-only geometry is the cause. The 128-pair corpus has 0
  added triangles, so the live add path was never covered.
- **Nothing default-on.** The key stays off and the branch is not shippable
  until the bogus added triangles are isolated; the user eye-check command is
  in §5.5 (it will also show the occluders). One launch used; no second.

## 2026-11-04 (walker texture cost): crowded `tex` is per-bind work, not the rehash

Full analysis in [`docs/walker-tex-cost.md`](walker-tex-cost.md). Branch
`exp/live-ab` in `ps3recomp-play`, build `recomp-work/build-play`. **Two game
launches**: the first A/B was killed by the system for low memory after two
segments, the second completed 12 segments at the pitch meter. Offline: the
crowded capture `walker_replay`, the user's 40 ms hitch recordings, and the
crowded play logs. No RPCS3 or other GPL emulator source was read or cited.

- **The crowded `tex` phase is the crowded frames' own population.** In the
  real crowded reports of `play_20261104_173326.log` / `165634.log`, `tex` is
  **13-31 ms for 2,300-3,400 unit binds (6.8-8.3 us/bind)**. The low-`tex`
  lines are the frame-sampled profiler timing an idle drain (phases on 1
  drain in 8, draws counted every frame). The same engine on the crowded
  capture costs **2.4-3.0 ms** (1.9 us per cache slot, null backend).
- **Uploads are the tail.** `tex.upload` in the 40 ms recording is 3/frame
  median (0.03 ms), **p90 6.6 ms**, max 290 ms; the bad cases are whole 8 MB
  images decoded and uploaded (20-33 ms spans). The median crowded frame
  uploads nothing.
- **Targets:** a crowded capture frame answers 647 depth-target binds from
  the per-serial snapshot and 53 colour targets through the backend view;
  the view lookup scanned all 2,048 objects. The meter keeps 1,024 texture
  entries (full) with ~0 evictions.
- **Fixes, all live keys default off:** `PS3RECOMP_ENG_TARGET_BLOOM` (a
  plain bind skips both target-table scans; inFAMOUS 2 MIT bloom),
  `PS3RECOMP_ENG_TEXCACHE` (4,096 entries + 64K lookup index + backend
  object table 8,192), `PS3RECOMP_ENG_TEXMEMO_LOCAL` (per-entry validation,
  survives target/upload churn elsewhere), `PS3RECOMP_VK_VIEW_MEMO` (O(1)
  view lookup). Offline the bloom cuts the non-slot part of `tex_bind`
  **0.326 -> 0.222 ms** and target-table scans **3,506 -> 1,024** per
  crowded frame (2,482 skipped).
- **Exactness:** `meter_frame` 64 and `walker_replay` 10 give the same hash
  and gate with every key off and on; `test_tex_memo` 17/17;
  `tools/test_video_fixtures.py` **30/30**. TEX_DIRTY is untouched (its
  44,656-pair/0-stale check still applies). No `spu_context` fields.
- **Meter A/B (12 segments, N=6 x 10 s):** fps **36.72 -> 36.53** (paired
  +0.19, CI [-2.23, +2.61]), p50 **34.15 -> 34.15 ms**, walker
  **9.56 -> 8.84 ms**, `tex` **0.346 -> 0.292 ms**; A/B thumbnails 1.98 vs
  A/A 1.70. The meter is c690-bound, has no uploads and no cache pressure,
  so this is a no-regression gate; the crowded replay is the test.
- **Nothing is default-on.** The user's keys-off/keys-on zoomed-out replay
  command (40 ms hitches, live-ctl fallback, what to look for) is section 6
  of the doc.

## 2026-11-04 (Edge E1): fused native 0x25/0x26 holes pass, caller modelled, one command pass (offline only)

Full analysis in [`docs/edge-e1-holes.md`](edge-e1-holes.md). Worktree
`ps3recomp-e1` (branch `exp/edge-e1-holes`, from `exp/live-ab` `230acd9f`),
own build `recomp-work/build-e1` (build-play flags, private
`recomp-work/spu_lifted-e1`). **Zero game launches** in this mission; nothing
under `ps3recomp-play`, `build-play` or `build-mt` was touched.

- **The 0x132E0 caller is modelled.** `r4` is a stack slot whose word 0 is the
  candidate triangle count and word 2 the kept triangle count; after `0xD080`
  the caller writes `+0x114 = 3 * word2`, `+0x118` and `+0xA0`. The old step-4
  blocker was that write. The fused pass leaves it untouched, runs the body
  once, and merges the sample-only triangles afterwards in
  `c690_cull_fix_end()` (then bumps `+0x114`).
- **One command pass, two stage evaluations.** The game grid runs exactly on
  the real context; the sample grid runs on a private LS/GPR copy with the 2x
  transform patch. No real-context LS/GPR snapshot, no second body run. The
  key is `PS3RECOMP_C690_HOLES_NATIVE` (live, default 0).
- **128-pair gate PASS** (`corpus_edge1_25` + `_26`): 0 removed/reordered vs
  the SPU, 0 added on this corpus, full LS/GPR bit-identical to the lifted
  union, `+0x114` equals the list length / a multiple of 3 / the union count,
  buffer pointer and end unchanged. The old 400-pair corpus (7 added) is gone;
  the add path is covered by the merge unit test plus that bit-identical union
  equality. `test_c690_holes_merge` passes.
- **A gate-caught bug:** the private refactor cached a negative transform scan
  for 256 calls, so the 0x26 pairs after 64 0x25 pairs scored inert. Negatives
  are never cached now; only the fused probe may skip a rescan, and only for
  0x25 (every captured 0x25 job has no transform, every 0x26 job has one).
- **Cost (gcc -O2, one 0x132E0 call):** lifted 22.2-24.9 us, native exact
  18.9-19.9, **fused holes 39.7-41.9**, native union 112.8-116.3, lifted union
  116.2-119.9. ~2.7-3.5x cheaper than the union and ~1.6-1.9x the lifted cull; 0x25 is at native cost
  (faster than lifted) and only 0x26 pays the sample evaluation. Estimate:
  ~-0.5 to 0 fps at the meter, inside the A/B noise. Not measured live.
- **Next:** merge, re-patch the module and rebuild `build-play`, then run the
  meter A/B and the user's pitch close-up check with `MLB16_CULL_BYPASS=0`
  (exact commands in `edge-e1-holes.md` §5).

## 2026-11-04 (walker MT repair): build fixed, C hang fixed, black field gone, timing/visual gates still fail

Full analysis in [`docs/walker-mt-repair.md`](walker-mt-repair.md). Branch
`exp/live-ab`, `build-play` live and `build-mt` offline. Four launches (the
cap): two pre-fix A/B/C reproductions, one validation-layer A/B/C, one timing
A/B. Nothing default-on.

- **build-mt fixed for the mission targets.** Its `Release` flags lacked
  `-march=native`, so `c690_native_cull.c` hit the `-mfma` `#error`. Reconfigured
  to build-play's `-O2 -DNDEBUG -march=native`; `replay_rsx_engine`, the queue /
  readback tests and the new `test_mt_turns` build. A full build still stops on
  three pre-existing, unrelated target source-list gaps (`replay_rsx`,
  `fiber_switch`, `test_vdec_stub`).
- **Black field: the worker's depth clear and scratch-depth setup used the
  walker's shared `s_vk.cmd` and submitted outside the chunk's turn.** They now
  record into the worker's chunk at the packet position, state-thread
  clears/uploads are submitted before the chunk that uses them, and texture
  handles are immutable/deferred under MT. The field is no longer black (dark
  fraction 0.137 vs A 0.143), but A/B thumbnails still differ by mean abs
  **38.8/255 vs an A/A baseline of 3.4**, so the visual gate fails.
- **C hang reproduced twice and fixed by removing the third thread.** The
  `rsx-tex` resolver ran texture create/upload on its own thread with
  `vk_t_user == NULL`, sharing the walker's one-shot command buffer, fence and
  staging buffer; the walker wedged (no `[walker-prof]`, guest `[flip]` still
  advancing, frame frozen). Plain textures now resolve on the state thread
  before the packet is queued; C runs its whole window. `test_mt_turns` (five
  turn/epoch scenarios, 10 s watchdog) passes. No gdb stack dump: both
  watchdog scripts failed before attaching (non-executable, then a `pgrep`
  process-name mismatch), and the last launches were kept for validation/timing.
- **Texture drain mostly removed, gate still fails.** Plain and surface-view
  bind drains are gone for a recording backend; texture releases are deferred.
  Live `tex` 14.5 (M4) / ~50 (first repair build) -> **7.85 ms/frame** (MT off
  0.34). Timing A/B (N=6 x 10 s, 2 s dropped, `ab_live_walkermt8timing`):
  fps **35.10 -> 33.03**, walker **9.50 -> 12.77 ms**, wait 16.51 -> 17.84,
  state `draw` 2.62 -> 1.04 and `bind` to 0. The remaining ~17 us/texture slot
  is the target/depth/reinterpreted readbacks and the forced create-and-swap;
  the next step is ordered upload records in the worker buffer.
- **Validation** (`ab_live_walkermt7valfinal`): no VUID and no error in A/B/C;
  only 10 pre-existing swapchain threading errors during boot/menus, also
  present with MT off. No hang, no fallback. The final binary (last deferred
  release) was built after this run, so it was not re-validated (launch cap).
- **No user zoomed-out command.** B is slower, visually different and has no
  timing win.

## 2026-11-04 (menus, revision 2): user confirms ~60 fps; FIFO fall-through and 17 ms bound kept

Full analysis in [`docs/menu-flip-fix.md`](menu-flip-fix.md). Branch
`exp/live-ab` (`dcdd6728`), `build-play`. The user's two recordings confirm the
fix (`PS3RECOMP_FLIP_TICK_OWN_STATE=1`): **42.10 -> 59.44 fps**, ticks/flip
**1.485 -> 1.052**. The 34 ms two-timeout class falls **20.81 -> 2.55/s
(-88 %)**; it is not zero, because 257 of 5,989 frames still see the tick land
after the 17 ms bound. New distribution: 17-20 ms 3.44/s, 20-32 ms 2.65/s,
32-36 ms 2.55/s (the rest are < 17 ms and below the recorder threshold).

- **FIFO fall-through (revision 2).** The own-state test only judges a direct
  request; when it does not complete one, the tick now falls through to the
  FIFO-reached half (`gcm_flip_fifo_take`) under the old `WAITING` status gate.
  A flip queued in the command stream has no direct-pending bit and was never
  completed while the key was on. The fall-through never consumes the direct
  flag, so it cannot bypass the age bound.
- **Selectable bound.** Key value 1 = 17,000 us (guest's first wait), 2 =
  33,366 us (whole two-wait deadline), other non-zero = 2. Unit tests cover
  the fall-through, both bounds, no double completion, and no completion past
  the abandon point; 30/30 video fixtures pass.
- **Three-arm meter A/B** (`ab_live_menufix2_20261104_203012`, one launch,
  N=4 x 10 s, 2 s dropped, A/B/C = 0/1/2):
  - 0: fps 34.97, p50 34.15, walker 10.43, two-timeout 0.324/frame.
  - 1 (17 ms): fps 36.22, p50 **34.15**, walker 8.91, two-timeout
    **0.336/frame** (paired differences mixed sign, mean +0.012; neutral).
  - 2 (33.4 ms): fps **29.09**, p50 **36.05**, walker **14.69**, two-timeout
    0.121/frame. It removes deadline frames by completing in the second wait,
    but the guest then advances and blocks on the next buffer's ref: the
    `KEEP_STATUS` failure mode (-5.9 fps).
- **Thumbnails** inside the measured windows: cross-arm means 2.49-2.56/255
  vs within-arm 2.48-2.69, so no visual regression. 0 dropped presents.
- **Recommendation:** keep **1 (17 ms)**. Default stays off in code; the play
  default is set after review.

## 2026-11-04 (menus): flip tick fix implemented behind a live key, meter A/B neutral

Full analysis in [`docs/menu-flip-fix.md`](menu-flip-fix.md). Implements the
[`menu-34ms.md`](menu-34ms.md) proposal on branch `exp/live-ab`,
`build-play`. `PS3RECOMP_FLIP_TICK_OWN_STATE` (**default off**,
`PS3RECOMP_CTL` switchable): `cellGcmTickFlip` judges a direct flip by our own
`s_flip_direct_pending` + request timestamp instead of the guest's
`s_flip_status`, and only while the request is younger than the guest's first
17 ms wait (the age bound KEEP_STATUS lacked). Key off keeps the old gate
exactly.

- **Unit-tested** (`test_gcm_tick_in_drain`, 30/30 video fixtures): request ->
  `SetPrepareFlip` clobbers status to DONE -> tick completes once; a request
  older than 17 ms is not completed and stays outstanding; no double
  completion; no completion without a request. No `spu_context` change.
- **Pitch-meter A/B** (`ab_live_menufix_20261104_201037`, one launch,
  N=6 x 10 s, 2 s dropped, arms alternated): p50 **34.15 ms both**, p90
  38.01 -> 37.27, p99 41.00 -> 40.89, fps 34.75 -> 36.71 (mixed per-segment
  sign; one A segment has a host stall), 0 dropped presents, no hang. Two
  consecutive-timeout frames 0.327 -> 0.374 per frame (sampled every 2 s;
  reported, not a frame-time regression).
- **Scene thumbnails**: A/B mean abs diff 2.97/255 vs A/A 3.44 and B/B 2.89
  (inside the A/A control). No visual change.
- **Menu number is the user's**: start screen + main menu + team select, key
  off vs on, 17 ms hitch recording to `recomp-work/captures/` (command in the
  doc, plus a `PS3RECOMP_CTL` fallback). Expect ~55-60 fps, no stutter,
  animations/audio normal, no hang on screen change or match load. Nothing is
  default-on; user decides, then a medium review before default.

## 2026-11-04 (menus): the 34 ms frames are the display tick skipping a status the title rewrites

Full analysis in [`docs/menu-34ms.md`](menu-34ms.md). Diagnosis only: no code
change, no build, no launch. Evidence: the user's menu capture
`recomp-work/captures/menus_20261104_193448.pstl` (146.2 s, every frame
> 17 ms) and `recomp-work/play_20261104_193448.log`.

- **41.8 fps, not work-bound.** 6,120 flips / 146.23 s; 3,044 frames at
  34.12-34.16 ms (89 %), 353 at 17-20 ms (10 %). Walker work p50 0.13 ms,
  idle wait 23.6 ms, zero `c690.job` events. Log vblank counter: 9,154 ticks
  / 6,120 flips = 1.494; 3,044 two-tick + 3,076 one-tick frames = 6,120.
- **The 34 ms mode is two 17 ms timeouts because the completion is never
  generated.** The only edge is the display tick (`GCM_FLIPCB_ONTICK=1`);
  `cellGcmTickFlip` requires `s_flip_status == WAITING` (cellGcmSys.c:1054),
  and `cellGcmSetPrepareFlip` writes `DONE` (cellGcmSys.c:4334). In menus the
  prepare lands p50 3.07 ms after the flip request, the tick ~13 ms later.
  The 353 short frames are the cases where the producer was blocked, so the
  status stayed WAITING: tick completion p50 +13.43 ms, handler +0.08 ms,
  signal at +13.55 ms, frame end +17.6 ms. Handler-thread latency 0.08-0.15 ms
  and present p50 0.15 ms rule those out.
- **Proposal (not run):** take the tick gate off the guest's status word, use
  our own outstanding flip and an age bound (< the guest's first 17 ms wait);
  menus should go to one flip per vblank, ~55-60 fps (RPCS3 parity). It is a
  no-op at the meter, whose 282/853 two-timeout frames are the c690
  drain/producer ([`meter-p50-pin.md`](meter-p50-pin.md)); KEEP_STATUS=1 only
  moved 823 -> 776 deadlines there and cost 4.3 fps. Differs from
  FLIP_ON_DONE / TICK_IN_DRAIN / FLIP_FENCE_DONE / KEEP_STATUS (see doc §5).

## 2026-11-04 (walker MT M4 live): MT-on frame is wrong and the resolver arm hangs

Full analysis in [`docs/walker-mt-live.md`](walker-mt-live.md). Branch
`exp/live-ab` (`87cfe3aa` + measurement-only live-ctl key `e381d650`),
`build-play`, one pitch-meter `ab_live.sh` launch. The Khronos validation
layer is not installed; no validation arm.

- **No usable A/B/C sample.** A = MT off, B = MT W=3 `TEX_RESOLVE=0`, C =
  MT W=3 default. A and B each got one 8 s measured window; C **hung on its
  first frame** (zero flips / zero `[walker-prof]` for 13 s, no Vulkan
  error, no fallback). Per the stop rule the run ended and no second launch
  was made.
- **B loses live.** p50 34.76 -> 38.08 ms (+9.5 %), fps 34.0 -> 27.5,
  walker 12.2 -> 24.0 ms, `tex` 0.44 -> 14.47 ms. Draws/frame drifted
  553 -> 617 (+11.6 %), so per draw: walker 23.7 -> 37.2 us, `tex` 0.86 ->
  22.4 us. Per-worker recording does cut the state-thread `draw` phase
  (7.3 -> 2.0 us/draw) and `bind` to zero, but the per-texture
  `eng_mt_sync` drain swamps it.
- **B's frame dump is visibly wrong.** Thumbnail mean abs diff vs A 78.9
  (A/A 2.2); pixels below RGB 12 rise 14.5 % -> 58.6 % overall and 13.5 % ->
  93.9 % in the lower half (the field is black for the whole B window). C's
  thumbnails are frozen, byte-identical to each other and essentially the
  last B frame (mean abs diff 1.8).
- **Cause of the C hang (leading candidate).** Worker waits for
  `tex_taken` before starting a segment (`rsx_draw_engine.c:7630`); the
  resolver waits for its group turn (`:7207`), which the state thread
  allocated inside that same chunk (`:7147`, packet epoch `:7991`). The
  null replay never takes the per-worker recording path, so M4's exactness
  tests could not see it. Proving which side is stuck needs a stack dump or
  a C-from-boot repro; the stop rule excluded both.
- **Verdict:** stop MT in its current form. One measurement-only code change
  (make `PS3RECOMP_WALKER_MT_TEX_RESOLVE` live-switchable) is on
  `exp/live-ab`; offline `meter_frame` VSRANGE hashes stay
  `461ff3a834c62331` for MT off / W=3 `TEX_RESOLVE=0` / W=3 on. Nothing is
  default-on and no user replay command is issued.

## 2026-11-04 (walker MT M4): per-worker Vulkan recording + texture resolver, timing gate fails

Full analysis in [`docs/walker-mt-m4.md`](walker-mt-m4.md). Branch
`exp/live-ab`, `build-mt`. M4 of
[`docs/walker-multithread-design.md`](walker-multithread-design.md), default
off (`PS3RECOMP_WALKER_MT=0`). No game launch: the mission stopped at the
first failed gate (offline timing).

- **Per-worker Vulkan recording implemented.** Each record worker gets a
  private command pool/buffer/fence, descriptor pool, GPU-unpack set and
  ring slices, plus per-thread E2 bind state (targets, constants, textures,
  render-pass/framebuffer caches, scratch depth, sampler cache). Workers
  record segments; the engine submits them at the M3 global turn, so
  `vkQueueSubmit` order is guest order. Primaries, not secondaries (every
  draw is its own render pass). Pipeline variants are created under a
  mutex around the shared `VkPipelineCache`. The null replay backend keeps
  the M3 serialised turn path.
- **M4 texture resolver implemented** (`PS3RECOMP_WALKER_MT_TEX_RESOLVE`,
  default on with MT): the state thread enqueues plain guest textures, the
  packet carries a token, and the `rsx-tex` thread resolves at an ordered
  texture-group turn between packets. Guest bytes are snapshotted at
  enqueue by default (`PS3RECOMP_WALKER_MT_TEX_SNAP=1`). Target/depth/cube/
  feedback stay on the walker.
- **Exact.** MT off/W=0..4, 5 and 64 frames, normal and
  `REPLAY_NULL_VSRANGE=1`: `meter_frame` `a6fcc849341eba9b` /
  `ff2a220c28750bc3`, `461ff3a834c62331` / `089e7b1082b4bcc3`;
  `walker_frame` `416abee2c5bff47d` / `0bdb6c112e64e643`,
  `9b78ac38a25c5cbf` / `e20a9511ff2547c3`. Fixtures 30/30;
  `test_draw_queue` OK; `test_color_readback_guest` 85/0 (its CMake target
  now links `rsx_draw_queue.c`).
- **Timing gate FAILS.** 7 interleaved runs, 120 frames + 20 warm-up,
  `REPLAY_NULL_HASH=0`: `walker_frame` MT off 14.756 ms, W=1 15.731,
  W=2 15.912, **W=3 16.055 (+8.8%)**, W=4 17.194; W=3 with the resolver off
  **12.869 ms (-12.8%)**, still short of -25%. State `tex` ~6.1-6.6 ms, not
  <=2. Nearly every packet binds a new texture, so the ordered texture group
  becomes a third thread in the serial chain; per-worker recording has no
  effect on the null replay (no command buffers). No live A/B, nothing
  default-on.

## 2026-11-04 (walker MT M3): N workers + ordered commit, exact but timing gate fails

Full analysis in [`docs/walker-mt-m3.md`](walker-mt-m3.md). Branch
`exp/live-ab`, `build-mt`. M3 of
[`docs/walker-multithread-design.md`](walker-multithread-design.md), default
off (`PS3RECOMP_WALKER_MT=0`, workers 1..4 via
`PS3RECOMP_WALKER_MT_WORKERS`). No game launch: the mission stopped at the
first failed gate.

- **M0 capture fix (code only, recapture pending).** The live capture images
  only the pages the captured frame reads, so a warm fetch-plan memo or a
  cached texture leaves out pages a cold replay asks for; the replay then
  drops 427 draws (1,278 of 1,705) with 23,535 misses. The capture boundary
  now bumps the fast-cache epoch and notes every cached texture span. An
  offline replay-as-capture reproduced the bug and improved 1,278 -> 1,703
  draws / 23,535 -> 211 misses; the user recapture command is in the doc.
- **M3 implemented, exact.** N workers take contiguous chunks of immutable
  packets; each does its own remap, span resolve, index rebuild, mesh-ring
  snapshot and conversion in per-worker scratch, and commits only at its
  packet's global turn, so the backend still sees guest order on one thread
  at a time. `REPLAY_NULL_VSRANGE` digests are identical for N=off/0/1/2/3/4
  on `meter_frame` (`461ff3a834c62331`) and `walker_frame`
  (`9b78ac38a25c5cbf`); fixtures 30/30.
- **Timing gate FAILS.** 7 interleaved runs, 120 frames + 20 warm-up,
  `REPLAY_NULL_HASH=0`: `walker_frame` N=1 **16.07**, N=3 **14.14 ms
  (-12.0%, gate -25%), N=4 14.52 (worse than N=3); `meter_frame` N=3 10.60,
  N=4 10.89. N=3 is level with M2 (14.21 ms), not 25% better. Killed per the
  gate; no live A/B, nothing default-on.
- **Why:** the state thread still owns texture create/upload/snapshot,
  pipeline fingerprint and reference building, and every texture operation
  drains the worker chunks. On `walker_frame` the state `tex` term rises
  1.45 -> 7.47 ms at W=3, eating the fetch/convert win. The 25% step is the
  M4 texture/pipeline resolver, not more workers; N=4 also oversubscribes
  this 6C/12T box.
- **Concurrency:** TSAN on `test_draw_queue` found the SPSC producer reusing
  a slot after a relaxed read of the consumer cursor; it now acquire-loads
  `r` before overwrite, and the queue test is TSAN-clean.

## 2026-11-04 (walker MT M1/M2): packet path + one record worker, meter p50 flat

Full analysis in [`docs/walker-mt-m1m2.md`](walker-mt-m1m2.md). Branch
`exp/live-ab`, builds `build-play` (live) and `build-mt` (offline). M1 and M2
of [`docs/walker-multithread-design.md`](walker-multithread-design.md), both
default off (`PS3RECOMP_WALKER_MT=0`, live key `PS3_LIVE_WALKER_MT`).

- **M1: packet ABI, serial path.** `sink_end_work` now builds an immutable
  `eng_draw_pkt` and commits it in the old backend-call order. `WORKERS=0`
  commits inline. Hashes unchanged on `meter_frame`
  (`a6fcc849341eba9b` / `ff2a220c28750bc3`) and `walker_frame`
  (`416abee2c5bff47d` / `0bdb6c112e64e643`; the design doc's
  `3e6f6ad4ebb23ed6` is stale). Fixtures 30/30. Interleaved 9-run
  `walker_frame` median 14.479 -> 14.643 ms (**+0.16 ms**, gate +0.5).
- **M2: one SPSC record worker** (`rsx_draw_queue.c`), inFAMOUS 2 shape:
  packet-owned payload copies, ordered replay, every state-thread backend
  call drains first. New `test_draw_queue` covers order/wrap/full/sync. MT
  digests equal serial on `meter_frame`, `walker_frame` and `walker_replay`.
  New opt-in `REPLAY_NULL_VSRANGE=1` exercises the ranged VS-constant call:
  it caught the first A/B's black infield (worker rows lacked the `first`
  leading rows).
- **Live meter A/B (run 2, after the fix):** p50 **34.16 -> 34.26 ms
  (+0.10, gate +/-0.5)**, walker **10.88 -> 9.65 ms**, draw **3.05 -> 2.26
  ms**, fps 34.48 -> 33.19 (one noisy B segment; p90/p99 flat), 0 validation
  errors, scene diffs at A/A noise (2.32 vs 1.96). The first run
  (`walkermt_20261104_180852`) is a visual FAIL, fixed here.
- **M0 capture gate FAILS:** `walker_replay` is **1,278 draws and 23,535
  read misses** (needs >= 1,300 and 0), so crowded-replay timing stays
  untrusted; a recapture is the next prerequisite for M3.
- Offline M2 timing (replay has no Vulkan recording): `walker_frame` -0.18 ms,
  `meter_frame` +0.82 ms (replay-only texture drains). Default stays off.

## 2026-11-04 (walker profiler + remap): profiler sampling is a free win, direct remap is neutral

Full analysis in [`docs/walker-remap.md`](walker-remap.md). Branch
`exp/live-ab`, build `build-play`; offline replay plus two meter A/Bs (the
second after fixing a profiler-report bug the first exposed).

- **The play default had `PS3RECOMP_WALKER_PROF=1`** (`play_windowed.sh:101`,
  `ab_live.sh:255`). On the meter replay it costs **8.12 -> 8.92 ms/frame
  (0.80 ms, ~1.6 us/draw at 502 draws)**; most is the FIFO loop's per-method
  `clock_gettime` pair, so sampling draws did nothing. New
  `PS3RECOMP_WALKER_PROF_SAMPLE` (default 8) samples **whole frames** and
  keeps the `[walker-prof]` line, exact `draws=` and the per-frame
  `walker=`/`wait=` averages: **8.33 ms/frame**, ~0.2 ms left over the
  profiler-off 8.12. `SAMPLE=1` restores exact every-frame profiling. This is
  the only default-on change (pure instrumentation, measured cheaper).
- **`rsx_vertex_remap_build` is unique-vertex dedup for the GPU indexed
  draw** (NHL #62). The existing ascending/one-base run already skips it and
  `REMAP_FAST` answers nearby repeats; dynamic index streams still hash every
  reference. `PS3RECOMP_REMAP_DIRECT` (live key `PS3_LIVE_REMAP_DIRECT`, off)
  dedups dense single-base lists through a direct slot
  `(vertex_id - min) & mask`, exact and unit-tested; the replay gate is
  unchanged on/off (`a6fcc849341eba9b` / `ff2a220c28750bc3`).
- **Live A/B: neutral.** Run 2: p50 34.20 vs 34.16, segment-mean fps
  32.27 vs 33.13, paired -0.86 fps CI [-2.16, +0.45]; the draw-normalised
  walker medians say the opposite (39.6 vs 45.3 us/draw) because the arms
  sampled 600 vs 553 draws. Run 1 had B 0.72 fps *slower*. Both CIs cover 0;
  the key stays off.
- **memmove 6.5 %** is the GPU staging: raw vertex span
  (`rsx_vulkan_backend.c:5142`, `span*stride`), remap array (`:5144`) and
  rebuilt index buffer (`:5151`). No safe trivial cut in this build; trimming
  the raw copy to the referenced min/max range is the follow-up.

## 2026-11-04 (walker multithread): design for 2-3x crowded-replay walker time

Design-only mission: **zero code changes in `ps3recomp-play`, zero builds,
zero game launches**. Full dependency map, design comparison, Vulkan rules,
missions and kill criteria in
[`docs/walker-multithread-design.md`](walker-multithread-design.md). Data is
the user's two crowded-replay logs (`play_20261104_165634.log`,
`_165829.log`) plus the existing walker/WCB/c690 docs.

- **Premise confirmed, with a qualification.** The guest orders the *method
  stream* and the *render-target contents at sync points*, not which host
  thread computes a vertex or records a `vkCmdDraw`. With an immutable
  per-draw packet and an ordered commit, fetch/remap/convert, plain texture
  revalidation, pipeline fingerprint and Vulkan recording are independent.
  What must stay serial: FIFO parse/state, surface/RT creation + serials,
  clears, WCB arm/service, `SET_REFERENCE`/labels/semaphores, inline
  `NV3062`/`NV3089`, `GET_REPORT`, flip/present and ref publication.
- **Crowded medians** (`draws >= 1500`, first log, n=185): walker **59.5 ms**,
  parse 4.0, fetch 8.3, tex 10.9, pipe 6.1, bind 0.6, draw 14.2, other 8.1;
  p90 walker 105. The second log (n=138) is 76.7 ms with parse 18.1/tex 2.0,
  so the phases are bimodal; the per-draw cost is unchanged. Meter
  (`walker-profile.md`) is the same split at 552 draws.
- **Designs compared.** (a) ordered packets -> N workers -> ordered commit:
  **20-28 ms (2.1-3.0x)**. (b) frame segments: 1.15-1.25x for record-only,
  1.4-1.8x best with engine split, high cross-segment/cache risk. (c) narrower
  helpers + one record thread (the inFAMOUS 2 `BE_THREAD` shape): 1.3-1.7x
  realistic, best ~2x, because creates/snapshots/readbacks drain the queue.
- **Recommended: (a)**, delivered through (c): M1 packet ABI serial (hashes
  unchanged), M2 one record thread, M3 N workers with contiguous chunks and
  per-worker command pools/rings/descriptors, M4 texture/pipeline resolvers +
  `WCB_PREFETCH`, M5 hardening + meter A/B + the user's zoomed-out replay.
  Each has a numeric gate and kill criterion; defaults stay off until the
  crowded test passes, and a live key falls back to the single thread.
- **Key constraint found:** deferred workers must not re-read guest bytes the
  SPU/PPU may have reused. Packet-owned snapshots for the five c690 mesh rings,
  fence-only for static data, is the rule; it is the same lesson as NHL #82
  (fragment program copied at record time) and #179 (movie textures snapshotted
  at record time), and inFAMOUS 2's copied borrow payloads.
- **Prerequisite:** there is no `walker_replay` capture yet (only
  `meter_frame` 502 draws and `walker_frame` 668 draws). M0 is one
  `PS3RECOMP_WALKER_CAPTURE` frame from the zoomed-out replay, in the next
  game-running mission; the design's offline gates use the two existing
  captures until then.
- **Running remap mission:** `PS3RECOMP_REMAP_DIRECT` inside
  `rsx_vertex_remap_build` is inherited per worker; its sampled
  `WALKER_PROF` means MT worker time must be counted separately. No wait for
  it: the packet ABI is independent.

## 2026-11-04 (WCB async): readbacks are two fences; prefetch removes the copy, meter p50 unchanged

Branch `exp/live-ab` in `ps3recomp-play`, build `recomp-work/build-play`. Full
evidence, code paths and the user's replay command in
[`docs/wcb-async.md`](wcb-async.md).

- **Attribution (code + live trace):** an ondemand readback is **two**
  `vkQueueSubmit`+`vkWaitForFences` round trips -- flush the open draw batch
  and wait, then submit the image->host copy and wait (`rsx_vulkan_backend.c:3088`,
  `:3247`, `:841`); no queue-idle wait. At the meter the guest reads a
  six-level 32x32 RGBA8 exposure mip chain by page fault, 0.34-0.35 reads/frame
  per level. In the user's 40 ms crowded recording: **p50 10 copies/frame**,
  96 % <= 4 KiB, **13.5 ms/frame** copy+sync; the 4 KiB 32x32 level dominates
  (mean 6.12 ms, p50 3.23 ms) -- waiting for the draws, not bandwidth.
- **`PS3RECOMP_RSX_WCB_STREAM` (new, live, default off): neutral.** One
  submission for the copy plus the open batch: walker **32.62 -> 32.50
  us/draw**, 32x32 copy 0.985 -> 1.030 ms/f, p50 34.16 -> 34.19. The saved
  submission was not the cost; the in-batch barriers cost about as much.
- **`PS3RECOMP_RSX_WCB_PREFETCH` (new, live, default off): the win.**
  Queue the hot target's copy at the sync point and submit the batch without
  waiting; the later read collects the in-flight ticket. Meter A/B (one
  launch, N=6 x 10 s, 2 s dropped): readback copy **1.18 -> 0.17 ms/frame
  (-86 %)**, guest wait **2.50 -> 0.93 ms/copy (-63 %)**, 32x32 copy
  **0.955 -> 0.117 ms/frame**, walker **32.39 -> 31.04 us/draw (-4.2 %)**;
  fps 32.85 vs 32.19 (paired +0.52, CI -0.88..+1.91) and p50 34.16 vs 34.17
  are unchanged because the meter frame is the c690 producer.
- **Live per-draw profile** (`perf record` on the `rsx walker` thread, 13,033
  samples, 25 s): `rsx_vertex_remap_build` 16.3 % (5.3 us/draw), `memmove`
  6.5 %, `WALKER_PROF` timestamps ~6.7 %, `sink_end_work` 5.1 %,
  `eng_pipeline_get` 5.0 %, FIFO loop 4.9 %; Vulkan recording+descriptors
  ~2 %. Lock/futex samples < 0.05 % -- no SPU-lane contention on the walker.
- **Tests:** `test_color_readback_guest` 85 checks (new stream and prefetch
  cases, including "drawn again before the read => sync-point bytes"),
  30/30 video fixtures, replay gate unchanged (`a6fcc849341eba9b` /
  `ff2a220c28750bc3`). No `spu_context` fields. Nothing default-on.
- **Next:** the user's zoomed-out replay A/B (`play_windowed.sh` with
  `PS3RECOMP_RSX_WCB_PREFETCH=0/1` and the 40 ms hitch recorder, command in
  the doc). The crowded frames are where the 13.5 ms/frame copy+sync can
  overlap; the meter cannot show it.

## 2026-11-04 (walker per-draw): offline -14 %, live neutral, nothing default-on

Branch `exp/live-ab` in `ps3recomp-play`, build `recomp-work/build-play`. Full
rank, exactness and commands in [`docs/walker-per-draw.md`](walker-per-draw.md).

- **Offline replay, 7 interleaved runs x 240 frames:** from the play defaults
  (`PIPE_MEMO=1 REMAP_FAST=1`) the reference-list remap memo plus an
  incremental reference hash and an indexed memo lookup cut
  `meter_frame` **8.986 -> 7.742 ms (17.90 -> 15.42 us/draw)** and
  `walker_frame` **13.146 -> 11.294 ms (19.68 -> 16.91 us/draw)**. All replay
  hashes/gates identical (`a6fcc849341eba9b` / `ff2a220c28750bc3`,
  `416abee2c5bff47d` / `3e6f6ad4ebb23ed6`). 30/30 video fixtures pass,
  including a new fold-vs-full reference-hash check.
- **Perf rank (meter, play defaults):** `rsx_vertex_remap_build` 18.3 %,
  texture sparse/full hashing 16.8 % (replay-only; live uses `TEX_DIRTY`),
  `eng_pipeline_get` 5.2 %, vertex conversion 5.1 % + 3.3 % fetch blocks,
  FIFO loop 4.1 %. The whole offline win is `fetch_plan` (meter 1.90 -> 0.47
  ms; walker 2.87 -> 0.69 ms).
- **Live meter A/B (one launch, N=6 x 10 s, 2 s dropped):** p50 **34.15 vs
  34.17 ms** (c690-bound, unchanged), walker **17.72 vs 19.49 ms**. The arms
  did not sample the same scene (A ~519 draws/flip, B ~555, +7 %); normalised
  the walker is **31.5 vs 32.0 us/draw (+~2 %)**, i.e. neutral to slightly
  worse. The static capture's remap memo hits 497/502 with 0 misses; live the
  index streams change, the memo misses, and the save copy is pure overhead.
- **Keys:** `PS3RECOMP_REMAP_CACHE` (existing, still off in the play scripts),
  new `PS3RECOMP_REFS_HASH_FAST` / `PS3_LIVE_REFS_HASH_FAST` and
  `PS3RECOMP_MEMO_INDEX` / `PS3_LIVE_MEMO_INDEX`, all default **off**. No
  `spu_context` fields changed. Recommendation: do not default them; a future
  retry needs a two-strike save and a live A/B, and the live-only Vulkan
  recording / phase-2 WCB waits are the real per-draw levers.
- **Also:** the team-select capture is a single 4-vertex draw (the logo is
  never submitted), so it cannot rank per-draw work. A crowded-replay capture
  command is in the doc (section 7.2) for the next renderer mission.

## 2026-11-04 (meter p50 pin): 34.15 ms is the c690 producer, not the flip/vblank model

Two meter launches, branch `exp/live-ab` in `ps3recomp-play`, build
`recomp-work/build-play`, native dispatch on. Details and reproduce commands in
[`docs/meter-p50-pin.md`](docs/meter-p50-pin.md).

- **Diagnostic timeline (`ab_live_p50pin_20261104_152125`, 25 s, 853 frames):**
  frame p50 **34.15 ms**, c690 lane union p50 **32.01 ms**, first c690 job
  p50 +0.10 ms, last c690 job p50 **+32.97 ms** (p90 +34.81), walker drain p50
  22.39 ms, put-wait p50 11.36 ms. The hard 32-34 ms class (285 frames) has
  union 32.78 and last job 33.08 in a 34.15 ms frame; the remaining ~1.1 ms is
  the walker/ref tail. 54.5 % of hard frames finish c690 before the guest's
  true 33.366 ms deadline and still end at 34.15.
- **The flip deadline is 2 x 16683 us, not a 30 fps setting.**
  `Gfx_DisplayFlip` (`0x000692dc`) gives up at `DAT_011203f8 << 1`;
  `FUN_00064b68` sets it to 0x412b (16683) at 720p. Two host
  `Cond_Wait(17000)` rounds are 34.0 ms + overhead = 34.13-34.15. There is no
  `cellGcmSetFlipMode` call; 31 % of meter frames are <20 ms, so the title is
  not capped at 30 fps.
- **Capping the wait to 33.366 ms is neutral (A/B
  `ab_live_p50fix_20261104_153029`, 4 x 8 s per arm, 2 s dropped):** p50
  **34.15 vs 34.15**, p90 37.93 vs 38.45, p99 41.19 vs 43.04; per-segment p50
  A 34.15/34.25/34.14/34.15, B 34.15/34.17/34.14/34.15. B hit three unrelated
  0.7-0.9 s present/pump stalls, so its pooled fps (29.88 vs 35.16) is not the
  comparison. The cap moved the wait exit but not the frame boundary: the
  display thread then waits for the next buffer ref, published by the same
  c690 + walker producer at ~34.1 ms.
- **Code:** new live numeric key `PS3RECOMP_FLIP_DEADLINE_US` (default **0**),
  `runtime/ppu/flip_deadline.h` state machine, hook in
  `runtime/ppu/ppu_sysprx.cpp`; unit test `tests/sync_stress/test_flip_deadline`
  passes, no `spu_context` fields, shared-field ABI check passes. It is an
  experiment only and stays off; the A/B shows it is not the fix.
- **Conclusion:** native dispatch bought the 40-50 ms tail (p90 43.2 -> 39.2,
  40-50 share 24.3 -> 7.1 %, p50 unchanged), not the median. The median is
  still Edge compute: 0x21 emit 25.0 %, 0x22 whole-body 21.2 %, 0x2A 13.5 %,
  0x25/0x26 cull 11.1 %, 0x0A 6.5 %. If the meter p50 is the target, unpark
  those; the renderer/walker is the lever for the real-play 40-80 ms frames
  (walker-bound, c690-serialisation.md). The parked Edge items are the
  remaining p50 levers, not finished work.

## 2026-11-04 (c690 serialisation): the replay long frames are walker-draw-bound; the Edge premise is wrong

Offline only (zero game launches, no build). Data is the user's 40 ms hitch
recording `longframes_20261104_142459.pstl` (1,930 frames >= 40 ms) plus its
matching `play_20261104_142459.log`. Details, band tables, candidate verdicts
and the Gantt in [`docs/c690-serialisation.md`](docs/c690-serialisation.md).

- **The "Edge on one lane of five" is a frame-length average, not
  serialisation.** Inside the Edge burst the five lanes run **3.82 of 5
  concurrent** in 80-250 ms frames (4.00 in 40-50, 3.92 in 50-80). The c690
  lane-sum is nearly constant across the bands: **102 / 111 / 109 ms**
  (575 / 712 / 796 jobs, ~22 ms per lane). The last job is at
  **+29 / +29 / +32 ms** in 40-50 / 50-80 / 80-250 ms frames. Edge finishes;
  the frame does not.
- **drain + put-wait is 100 % of the frame** (drain 78 / 83 / 83 %). The
  `hitch_table.py` "92 % c690-dominated" is the summed-lane artefact: five
  lanes x 22 ms = 110 ms is ranked against the single walker thread. On wall
  time the walker is the chain in nearly every long frame.
- **Representative 86.1 ms frame (797 jobs):** c690 union 23.9 ms, last job
  +24.2 ms; walker put-wait 0-20.1 ms then drain 20.1-86.1 ms (66.0 ms). PPU
  main is blocked 58.8 ms in the ref lwcond / 22.1 ms spinning `gcm.finish`.
  The Gantt makes the 62 ms Edge-free tail visible. Inside the walker span,
  **10 guest-mandated WCB readbacks sum to 35.9 ms** of copy + GPU sync (13.2
  ms before the drain, 4.5 and 17.4 ms inside), and ~44 ms is draw/parse CPU.
  The `[rsx wcb]` log in the same seconds reports 3.2-4.7 copies/frame and
  5.8-11.3 ms/frame copy; the long frames carry the heavy copies.
- **Same run's `[walker-prof]` cross-check:** ~1,570 draws/frame (meter ~500-800),
  walker 45-67 ms, engine 36-41, parse 8-25, wait 10-20. Frame time scales
  with draws: **47.5 us/draw**, r = 0.803; 240 draws -> 20.2 ms, 1,451 -> 74.9,
  2,131 -> 111.7, 3,025 -> 179.6. This is "scales with players on screen":
  more draws, not more Edge jobs (796 c690 jobs/frame here vs ~680 at the
  meter).
- **Candidates rejected with evidence:** per-list PPU wait
  (`Mesh_WaitPrevJobList`/`WwsJobList_SpinUntilDone`) is real but hidden under
  the walker; no barrier/serial tail (lanes finish within 1 %, gaps p50 10 us);
  walker park/starvation no (put-wait is 17 % at the start, then it drains);
  PPU main is waiting; native-dispatch fairness and lock-line are not limiting.
- **No code change, no live key, no A/B.** An Edge key cannot shorten a frame
  whose Edge batch ends ~60 ms early; the premise stop rule was used instead
  of a launch. Best lever is the walker span: -25 % of the 44 ms draw/parse
  CPU is ~-11 ms on an 86 ms replay frame (12.8 -> 13.3 fps), -50 % ~-22 ms
  (15.6 fps); the 35.9 ms WCB sync is guest-driven and not addressed by
  RECT/BATCH/SKIP (they target bytes / pending merges, neutral at the meter).
  `PS3RECOMP_WALKER_FAST` stays off (5 % live hit, +2.4 ms). Meter c690 A/Bs
  (`native2`, `walk2b`) stand for the meter's own c690-bound frames.

## 2026-11-04 (c690 0x2A wiring): the meter is not one 32-byte class, so the fallback gate fails

Offline only (zero game launches). Branch `exp/live-ab` (`ps3recomp-play`);
no code was wired and `build-play` was not rebuilt. Details and the design in
[`docs/edge-gpu-2a-wiring.md`](docs/edge-gpu-2a-wiring.md).

- **Premise correction.** The meter's 0x2A output is a family, not one
  32-byte class. Over the two meter tail spools (128 sampled pairs):
  **36-byte 0, position-first 32-byte 55 (43.0 %), 20-byte 12 (9.4 %),
  48-byte 16 (12.5 %), other 45 (35.2 %)**. The descriptor at LS `0x1F690`
  selects the layout, but even one descriptor value (`0x00C40008`) does not
  have a single stride, so the stride/prefix classifier is what is reported.
- **The gate fails.** The 36-byte kernel covers **0/128** meter pairs and a
  32-byte kernel covers **55/128**. With both, **57.0 %** of sampled 0x2A
  pairs fall back; at 0x2A's clean 44.3 % share of c690 jobs that is **~25 %
  of c690 jobs**, five times the 5 % gate. A live A/B would measure the
  fallback, so the mission stopped at the coverage measurement (its own stop
  rule for a wrong premise).
- **The 32-byte layout is decoded.** `[pos.xyz f32][normal 11:11:10]
  [4x s16][2x f16][two u16 index pair]`; the packed conversion is
  `clamp(trunc(v*scale)-1)` with scales 1024/512 for the normal and 32768
  for the tangent, `f16` for the UV. The last 4 bytes come from an index
  pre-pass the handler builds in LS (`0x2B580`/`0x27350`), not from the four
  arrays, so the next unit of work is that contract; a whole-pair kernel is
  the ~1-2 week job `edge-gpu-plumbing.md` §5 predicted.
- **No walker binding, no live key.** §4 of the doc lists every §2.7 path
  (CPU vertex fetch, GPU staging copy, index decode, WCB, SPU submit PUT,
  PPU) and how the binding must handle it; §6 is the exact 5 x 10 s live A/B
  for the mission that first passes the kernel gates.
- **Nothing enabled.** No `spu_context` field, no play-script change, no
  `build-play` rebuild. The only code change is the stride/coverage
  classifier in `tools/replay/test_c690_2a_plumbing.py`
  (`--coverage-only`).

## 2026-11-04 (c690 native dispatch): FAST_RESCHED 64, and a fairness-gated lane hand-off wins 4 ms of p90

Branch `exp/live-ab` (`ps3recomp-play`), build `recomp-work/build-play`. Three
meter launches: `fastresched1`, `native1` (staged-only first attempt),
`native2` (native key with the fairness fallback). Details, tables and the
reproduce commands in
[`docs/c690-native-dispatch.md`](docs/c690-native-dispatch.md).

- **`SPURS_PM_FAST_RESCHED=64` wins the 8/16/32/64 probe.** 3 x 10 s per arm,
  2 s dropped: fps 33.04 -> **33.54**, p90 **44.63 -> 43.53 ms**, p99
  **48.03 -> 46.84 ms**, p50 34.15 ms in every arm, 40-50 share 26.4 ->
  26.6 %. D=64 wins all three paired segments (+0.62/+0.50/+0.38 fps, p90 and
  p99 on all three); 16 is neutral, 32 slightly worse. **64 is now the play
  default** in both `play_windowed.sh` copies and `ab_live.sh`.
- **Why the earlier dispatch-cuts bundle lost ~2 fps.** `FAST_ACTIVE` is the
  only cut that changes when the fast path fires: it reran at
  `active == maxContention`, where the coordinator round trip is the
  cross-workload arbitration point. The bundle cut the host phase 23.54 ->
  9.19 ms and the burst wall 34.25 -> 30.99 ms, but the frame got slower:
  walker wait +0.94 ms, engine +0.91 ms, fps 35.30 -> 33.29, fewer sub-16 ms
  frames. `ONE_LOCK`/`GEN` only shave the fast path's own overhead.
- **Where a gap goes today** (existing timelines): critical-lane 10.61 ms of a
  35.00 ms burst = **tail 2.95 / host 5.03 / head 2.56 ms**; mailbox/tag/event
  waits are 0 records, `LOCKLINE_RETRY` 0.3 %. The new live 2 s window: A
  (FAST_RESCHED=64) gap p50 70.3, p90 104.2, p99 1216 us, lane busy 0.589.
- **Native dispatch implemented behind `SPURS_PM_NATIVE_DISPATCH` (wid
  bitmask, default 0).** It re-enters the lifted wwsjob PM in the same worker
  when it has a staged job, or when the guest still has ready/signal work and
  **no other enabled workload wants this lane**. The PM still owns the WWS job
  list protocol, so order/dependencies, `maxContention`, priorities,
  `WwsJobList_SpinUntilDone` counters, labels/signals and workload state are
  unchanged; everything unrecognised falls back to the coordinator. Unit test
  in `test_spurs_scheduler` (pure predicate + end-to-end staged rerun with
  coordinator delta 1 and native delta 1); ABI check and all SPURS fixtures
  pass.
- **The A/B is a large win** (`native2`, 5 x 10 s per arm, A = FAST_RESCHED=64,
  B = same + `SPURS_PM_NATIVE_DISPATCH=6`): fps **34.00 -> 35.33**, p90
  **43.20 -> 39.21 ms**, p99 **46.67 -> 42.66 ms**, p50 34.15 ms both,
  40-50 ms share **24.3 % -> 7.1 %**, gap p50 **70.3 -> 53.8 us**, gap p90
  **104.2 -> 69.8 us**, gap p99 **1216 -> 397 us**, lane busy 0.589 -> 0.613.
  B segment 9 has a present stall (31.62 fps); excluding it B is ~36.3 fps.
  Native fired 746,839 times; fairness yielded 26,904, no-ready 5,599.
- **The staged trigger did not fire** (`nstage=0`): the wwsjob PM stores the
  job before exiting, so its `loadJobNum`/`runJobNum`/`storeJobNum` are
  `0xFFFFFFFF` at exit. The effective mode is the fairness-gated ready/signal
  rerun; the staged path is implemented and unit-tested for a PM that
  preloads. The native key stays **off** by default per the mission; enabling
  it is `SPURS_PM_NATIVE_DISPATCH=6`.

## 2026-11-04 (SPU inlining): implemented behind a flag, static gate fails, no meter run

Branch `exp/live-ab` (`ps3recomp-play`), flag `--inline-direct` (default off).
Offline only: no game launch, `build-play` untouched. Details in
[`docs/spu-inline-callees.md`](docs/spu-inline-callees.md).

- **Implemented** the audit's §8(2) option: acyclic `SPU_DIRECT` tail targets
  inlined in tail position, `brsl` callees inlined when they return cleanly
  through the link, recursion/indirect edges left on the trampoline,
  `--local-regs` run once over the combined body.
- **Gate (a) fails.** x86 per SPU on the probes: `0xAF98` 2163 -> 2516,
  `0xA410` 1243 -> 1243, `0xFDA0_body` 5090 -> 5090, `0x10F78` 1190 -> 1479.
  Only the `0x10F78 -> 0x10C70` path improves (2471 -> 1479 x86).
- **Executed instructions do not move.** On the four `0xFDA0` corpus pairs:
  **810.89 M vs 811.06 M** host instructions (+0.02 %, `perf stat`, 3
  alternating runs), same cycles, neutral wall time. The inline object is
  5.0 MB of text vs 1.35 MB.
- **Why:** the 26-37 % GPR/stack share is intra-function register pressure,
  not call-boundary traffic. A tail transfer still must publish the dirty GPRs
  at the chain's final exit; merging bodies enlarges the live-register union
  and adds spills. A `brsl` boundary is ~2 % of a body, not 15-25 %.
- **Stopped at gate (a)**: no exactness run, no meter A/B, and the `0xC0F4`
  back-edge item was not attempted (it was conditional on a gain).

## 2026-11-04 (long frames): the 40-80 ms population, and a capture for real play

Offline only (no game launch, no build). Data: 35 `play_*` sessions
(MangoHud CSV + log), the four non-empty `hitches.pstl` (>250 ms), and the
per-frame meter timelines `ab_live_timeline2_20261103_065615` and
`ab_live_pinburst0/1_20261104_*`. Details in
[`docs/long-frames.md`](docs/long-frames.md).

- **40-80 ms frames are a mesh-scene problem, not a menu/boot one.** 176,821
  presented frames in 78.6 min: 16,651 are 40-80 ms (9.4 %, 222/min). Mesh
  phase 14,787 of 66,132 (22.4 %, 495/min); menus 0.38 %, boot 0.3 %. The 1:1
  present subset is 9,916 of 27,729 mesh frames (35.8 %, 611/min); the current
  large sessions are 25-39 % of in-game presented frames in that band.
- **>250 ms is the known other problem.** 53 hitches, 46.3 s: 48 are
  `spu.run` (boot/start-screen DXT), 4 `rsx.drain` (first-use shader; incl. the
  5.55 s match load), 1 c690. Not c690.
- **Where per-frame data exists (meter), the 40-80 frame is the same c690 work
  running slower, not more work.** pinburst0 40-50: 666 jobs vs 668 in 33-36,
  per-job lane time +3 %, occupancy 0.698 -> 0.671, host/coordinator gap
  +1.40 ms/lane (+29 %, 62 % of the extra burst wall). timeline2 40-50:
  674 jobs vs 584 in 33-36 but 680 in 36-40; occupancy 0.644 -> 0.558.
  Class shares: SAME WORK SLOWER 80.7 % / 95.1 %, DEADLINE SLIP 2.4 / 1.2 %,
  OTHER 16.9 / 3.7 %, **MORE WORK 0 %**. Extra frame = +9.47 ms (50 % c690,
  50 % tail) in timeline2, +7.46 ms (30 % burst, 70 % outside burst) in
  pinburst0. No vertex-count / content lever for this band.
- **Real-play 40-80 cannot be attributed frame-by-frame today** (recorder
  keeps >250 ms; `[flip]` is every 10th flip). It is 89 % mesh-phase and
  over-represented in seconds with a >=40 ms SDL pump gap (90.4 % vs 78.0 %
  for 33-36), but that is a coarse flag. Next session: record with
  `PS3RECOMP_TIMELINE_HITCH_US=40000`, ring 32, flush 2 (hitch mode clamps to
  2), ~0.5 GiB per 5 mesh minutes; command and analysis in the doc.
- **Cheap explicit guards:** keep PIN=0 (`play_windowed.sh:107`) and
  x11/FIFO/PRESENT_NEW_ONLY/PACE (`:173`, `:178`, `:192`); do not enable PIN=1
  (140 vs 79 long frames, more present stalls). Zero-build probe for the host
  gap: `SPURS_PM_FAST_RESCHED=16/32` (default 8;
  `libs/spurs/cellSpurs.c:1807`). Ranked frame removers: host/dispatch gap
  (~40 %), c690 compute (~30-40 %), present/pump starvation (20-40 %, unproven
  until the capture), non-burst tail (~15-20 %, high risk), deadline exactness
  (~2 %).
- `tools/hitch_table.py` gained `--bins`, `--classify` and `--baseline-jobs`:
  the six-bin histogram and MORE WORK / SAME WORK SLOWER / DEADLINE SLIP /
  OTHER labels for the 40 ms capture.

## 2026-11-04 (c690 GPU plumbing): 0x2A skip/export -> GPU -> ring bytes; the rest of the map

Offline only (no game launch). The data is the clean 60 s meter window of
`ab_live_copy1_20261104_120526`, the phase-2a/2b captures, and a new
offline harness. Details and tables in
[`docs/edge-gpu-plumbing.md`](docs/edge-gpu-plumbing.md).

- **The `0x14/0x0E/0x0F/0x18..0x1D/0x23` chain is 20.75 us/job, 11.21 %**
  of the c690 wall (907,390 jobs, 167.96 s wall), not ~26 us/13 %. The
  per-command averages sum to 29.98 us, but 0x14/0x0F/0x23 run only 0.18
  times/job and 0x18/0x19 only 0.75. **0x1A is the only DMA handler**
  (6.56 M small MFC GETB copies, 793 ms, 0.4 % of the wall); **0x1E is a DMA
  tag wait**; every other handler is SPU/LS ring/list/stream control. None
  is a GPU compute kernel; the GPU-able tail is still 0x21/0x25/0x26/0x2A.
- **0x2A plumbing proven offline.** For the captured 36-byte pitch job
  (`fncap21b/0x10F78`, 196 records, 7,056 bytes at LS 0x2F870) the exported
  arrays are 12,544 bytes; one GPU dispatch gives **0 differing record
  bytes**, and the real submit PUT mapping
  (`LS 0x2F000..0x320D0 -> EA 0x11765D00`) gives **0 differing ring bytes**.
  A captured `meter_frame` replayed through `replay_rsx_engine` gives the
  **same output hash** for the SPU and GPU record bytes.
- **Not wired live.** The meter's sampled 0x2A tail is a different format:
  **0/128** captured pairs are the validated 36-byte class; they are a
  32-byte record class. The 36-byte-only fallback is far above the 5 % gate,
  and the walker binding is not small (the CPU vertex fetch and the GPU
  unpack staging copy read the ring from guest memory; the index list is
  CPU-decoded). The wiring plan and honest estimate are in the doc.
- **Nothing enabled.** No live key, no `spu_context` field, no `build-play`
  change. New research tools: `tools/replay/test_c690_2a_plumbing.c` and
  `.py`.

## 2026-11-04 (c690 0x13/0x0A): the 17.7 us copy was an opcode-map artifact

One pitch-meter A/B (native 0xFDA0 off/on, `PS3RECOMP_C690_OP21=1`), no
capture spool. Details and the clean 60 s table in
[`docs/c690-copy-unpack.md`](docs/c690-copy-unpack.md).

- **0x13 is a real 16-byte copy at 0.72 us/call, not 17.73 us.** The step-5
  map named only 15 handlers; 26 were missing, so the commands after 0x13
  (0x14/0x0E/0x0F/0x18..0x1E/0x23, ~17-21 us/job) were charged to it. With
  the complete map (commit `1f1cbd57`) 0x13 is **0.37 % of the c690 wall,
  ~0.075 ms/frame**; the copy is a ring ticket (`0xE6B8`), a 16-byte header
  and one 16-byte `0x5680` store, with no DMA/tag/atomic wait.
- **0x0A is genuine and cheap:** 2.76 us/call, 6.4 % of the clean window
  (~1.3 ms/frame); the bit-exact native leaves are already at parity
  (`0x6D68` 1.00x, `0x7C18` 0.99x), so no native win.
- **Clean 60 s meter map** (907,390 jobs, 177.55 s wall): 0x21 25.6 %,
  0x22 **20.7 %, 573.6 us/call**, 0x2A 13.5 %, 0x25 7.9 %, 0x0A 6.4 %,
  0x26 3.7 %, 0x13 0.37 %. The 0x22 map fix is confirmed live.
- **Native 0xFDA0 is neutral** because it covers only the `0xFDA0` subtree
  (~25-80 % of the 573.6 us 0x22 class); 1.30x on that is ~0.2-0.7 ms/frame,
  below the ~1 ms A/B noise. A/B: 35.12 vs 34.42 fps, p50 34.15 both, p90/
  p99 faintly better in the native arm.
- **Nothing implemented or enabled.** The only kept change is the
  measurement-only hot-list extension naming the 0x13 and 0x22 chains; no
  `spu_context` field was added (ABI check PASS).

## 2026-11-04 (c690 GPU phase 2b step 5): clean map — 0x22 tree/cull was hiding in 0x21 sel1

Two pitch-meter launches (the second after fixing the command map), no
capture spool, plus offline parsing of the 2 s reports and the raw module
handler table. Details and tables in
[`docs/edge-gpu-phase2b-step5.md`](docs/edge-gpu-phase2b-step5.md).

- **Capture cap fixed.** The per-key pair cap is checked before the 256 KiB
  snapshot; a command-boundary 0x21 defers its snapshot to the sub-selector
  entry and checks the cap there. The new `cap` unit case runs six same-key
  commands at a cap of two: **2 pairs, 2 snapshots**, six histogram calls.
- **The 0x21 sel1 390 us/call was mostly opcode 0x22.** The raw handler
  table (LS 0x14770) maps **0x22 -> 0x10190**, the 0xFDA0 tree/cull
  continuation. The stats map omitted 0x22, so a 0x21 command never closed
  there and the whole tree/cull command was charged to sel1. The function
  table (87.8 % at 0x10190) and the raw table agree.
- **Clean 30.2 s meter window** (403,212 jobs, 74.31 s lane wall): 0x22
  tree/cull **23.79 %, 618.9 us/call**; sel7 **19.41 %, 57.6 us/call**; sel1
  **3.31 %, 48.7 us/call**; 0x2A 14.06 %, 0x13 9.62 %, 0x25 8.37 %, 0x0A
  6.84 %, 0x26 4.04 %. True 0x21 is **22.8 %** of the c690 wall, not
  43-47 %.
- **Full-tail GPU C is ~12 ms, not ~17 ms.** On the 24.39 ms critical-lane
  basis, 0x21 + culls + 0x2A is 49.3 % -> **12.0 ms**. Adding the
  already-native 0x22 tree/cull returns ~17.8 ms, but that is a CPU-native
  D target, not a new GPU kernel.
- **Re-rank.** 0x2A is the cheapest remaining win (bit-exact kernel already
  exists); 0x22 is the largest slice but already has a native CPU port; the
  best remaining GPU target is sel7/A3E0 (~4.7 ms / 2,493 lines), then the
  0xD080 culls (~3.0 ms / 1,551 lines). sel1 is 0.8 ms and no longer worth
  a port.
- **Sanity.** Same run, build-play with wrappers stripped: p50 **34.15 ms**
  (exactly the known value), clean segment 35.88 fps. The added timing
  instrumentation is estimated at ~0.9 % of one lane on top of the existing
  stats timer.

## 2026-11-04 (c690 GPU phase 2b step 4): 0xB830 leaf kernel exact; the 47 % was capture overhead

Offline only, zero game launches. Kernel, gate, burst and wrapper review in
[`docs/edge-gpu-phase2b-step4.md`](docs/edge-gpu-phase2b-step4.md).

- **0xB830 leaf kernel is bit-exact on all 64 meter pairs.** 0 differing LS
  bytes (4,194,304 words, max ULP 0) and the nine GPRs that survive the
  `0xA660` tail call are exact too. The shader transliterates the 78-line
  lifted body (25 shufb, 22 selb, 9 fma, 2 rotqby, 5 loads, 4 stores) with
  SPU byte order, FTZ and fused FMA emulated.
- **One dispatch per frame burst is 0.101-0.103 ms/frame.** 4,394 calls in
  one `vkCmdDispatch`, device-local (0.023 us/call); CPU record 1.7 us +
  submit 2.25 us. The dispatch/fence floor is ~0.08 ms; the extra calls add
  ~0.02 ms.
- **The step-3 9.06 us/call / 47.16 % is a capture artifact.** The tail hook
  snapshots 256 KiB in `c690_tail_open` before the pair cap in `_close`, so
  1,270,050 calls copied ~312 GB in 10 s (6.75 us per 256 KiB here). The same
  lifted leaf is **47.8 ns/call offline**: 0.209 ms/frame (4,379 calls) and
  60.7 ms/10 s, not 11.505 s. The real GPU saving over the lift is ~0.1
  ms/frame, not ~5.6 ms. The cull/A3E0 histogram times are similarly
  capture-inflated; a corrected capture (cap check before the snapshot) is
  the next step.
- **Option D estimate.** AVX2 native is ~71 vector ops + 9 live outputs,
  ~80-120 uops, ~6-10 ns/call (0.03-0.04 ms/frame) and would beat the GPU
  kernel, but still saves only ~0.17 ms/frame.
- **Wrapper is opt-in now.** With capture off it costs median 0.54 ns/call
  (1.1 % of the leaf, range 0-3.5 %), ~0.07 ms/s at 127k calls. The
  0x2A/0x21/0xA730 capture wrappers are now `patch_cull_hooks.py --capture`
  only; the play module was stripped, build-play rebuilt, and the replay is
  unchanged (0x2A/0x25/0x26/0xA7 64/64 exact). ABI check still agrees; no
  `spu_context` field was added.

## 2026-11-04 (c690 GPU phase 2b step 3): A730 body unblocked, cull/A3E0 kernels blocked

One pitch-meter launch (`ab_live.sh`, build-play, governor performance) and
offline replay. Evidence and port plan in
[`docs/edge-gpu-phase2b-step3.md`](edge-gpu-phase2b-step3.md).

- **A730 is unblocked at the right body.** The heavy path is
  `0x13140 -> 0x13250 -> 0xA3C0/0xA3F8 -> 0xA410 -> (bi gpr[82]) 0xA730 ->
  0xA798 -> (bi gpr[82]) 0xB830`; the sub-selector wrapper sees only a
  12.9 us prefix. A default-off wrapper at `0xB830` (op key `0xA7`) is in
  `patch_cull_hooks.py`; the unit test covers its pair, bitmap and histogram
  entry. No `spu_context` field; ABI unchanged.
- **One capture, 64/64 A730 body pairs exact.** The meter run
  (`fncap2b3a730`) saw 1,270,050 `0xB830` calls / **11.505 s (47.16 %)** of
  the 24.398 s tail; the `0x13140` prefix is 0.201 s (0.83 %). The `0xA7`,
  0x2A, 0x25 and 0x26 pairs all replay with 0 differing LS bytes.
- **Cull kernel killed, not built.** The 0x132E0 body is the 24-block /
  1,551-line `0xD080` skin/transform + index-pack state machine plus the
  0x49D4/0xE6B8 allocators, not a screen-space formula. The 64/64 exact
  meter pairs are ready, but a bit-exact GPU port is a multi-day
  C-to-GPU translation; no kernel/diff/burst number is claimed.
- **A3E0 kernel blocked.** The clean pairs reach a 19-function /
  2,493-line chain (`0xA730 -> 0xA798 -> 0xAF98/0xB238` + pack), not a
  compact formula; no kernel/diff/burst number is claimed.
- **Running total signed off as GPU-exact is still 0x2A only: 23.1 % of the
  step-2 meter tail (18.2 % in step 3), 0.23–0.27 ms per 429-job burst.**
  A730 is measurable and exact but not ported; the culls and A3E0 are not
  ported. The full tail cannot move with this result; the next lever is the
  A730 leaf port (~5–8 days), not another capture.

## 2026-11-04 (c690 GPU phase 2b step 2): meter-gated histogram, exact 0x2A/cull pairs, A730 kernel blocked

Two pitch-meter launches (`ab_live.sh`, build-play, governor performance;
the second only to validate the pairing fix) and offline replay. Evidence and
coverage plan in
[`docs/edge-gpu-phase2b-step2.md`](edge-gpu-phase2b-step2.md).

- **Capture gate fixed.** The tail hook has a dedicated, non-consumed
  `PS3RECOMP_C690_TAIL_TRIGGER`; `ab_live.sh` touches it only after the
  meter is confirmed and refuses to share `CAPTURE_PREFIX.trigger` (the
  walker capture deletes that file). The run log shows the trigger seen at
  the meter and the file still present afterwards.
- **0x2A is one command per pair.** `patch_cull_hooks.py` wraps the `0x10F78`
  body (and the cull body `0x132E0`) with the corpus hooks. The second run's
  **64/64 0x2A, 64/64 0x25 and 64/64 0x26 pairs replay byte-exact** through
  the lifted module; the unit test adds back-to-back 0x2A and
  trigger-not-deleted cases. Nothing default-on; no `spu_context` field.
- **Meter-window histogram.** 10.0 s / 395,784 commands / 23.242 s tail:
  A730 7.532 s (32.4 % time, 4.7 % jobs), 0x2A 5.359 s (23.1 %, 44.3 %),
  A3E0 4.655 s (20.0 %, 18.6 %), 0x25 3.060 s (13.2 %, 20.7 %), 0x26
  1.332 s (5.7 %), BB98 1.304 s (5.6 %, 4.5 %). A730 is 55.8 % of 0x21 time.
- **0x21 replay.** Command-boundary pairs: 21/32 A730, 21/32 A3E0, 18/32
  BB98 match the output arrays exactly except the parser-owned `+2` byte at
  `0x1F0F3`; the rest are boundary-contaminated by a following direct
  dispatch and are excluded.
- **A730 kernel blocked.** The class writes four 142 x 16-byte planar arrays
  through a heavily unrolled transform/format chain (matrix palette at
  `0x27270`, packed streams, jump tables), not a compact formula; a
  sub-selector wrapper captures only its 58 us prefix because the body is
  reached through the trampoline. No kernel, no diff, no burst number is
  claimed; step 4 was not attempted. The exact 0x2A kernel remains
  0.23-0.27 ms per 429-job burst device-local.
- **Coverage.** The 5 % job gate needs all classes except BB98 (4.52 % fallback,
  5.61 % time); the safe set is all six keys plus the culls. Projected lane
  saving from the meter histogram: A730 ~5.6 ms, A3E0 ~3.5, BB98 ~1.0,
  0x2A ~4.0, 0x25 ~2.3, 0x26 ~1.0 of the 17.4 ms full tail.

## 2026-11-04 (c690 GPU phase 2b step 1): stage corpus, three real 0x21 classes, burst dispatch is cheap

Two pitch-meter capture attempts (`ab_live.sh`, build-play, governor
performance; both launches used) plus offline analysis and a headless Vulkan
batch kernel. Evidence and plan in
[`docs/edge-gpu-phase2b-step1.md`](edge-gpu-phase2b-step1.md).

- **New stage capture, unit-tested.** `PS3RECOMP_C690_TAIL_CAPTURE=<dir>`
  records IN/OUT pairs for 0x21/0x25/0x26/0x2A at the command boundary: full
  256 KiB LS + 128 GPRs + 40 meta words + a 16-byte-line write bitmap, keyed
  by the 0x21 sub-selector and the A730/BB80 bit. It rides the existing
  `c690_stats` dispatch hooks, so it also covers the 0xBB80/0xBB98 direct
  calls without editing the generated module. `tools/test_c690_tail_capture.py`
  passes (off/key/cap/stride/trigger/bitmap). Nothing default-on; no
  `spu_context` field added.
- **Three real 0x21 classes, not one:** sel1 A730 (`0x13140`), sel7 A3E0 and
  sel7 BB98 (`0x131A0`, r6 bit 4 clear/set). Run-level live split
  (`PS3RECOMP_C690_OP21`, 405,079 calls): `0x13140` 10.3 % of calls / 45.5 %
  of 0x21 time (399 us/call), `0x131A0` 89.7 % / 54.2 % (54.5 us/call). The
  corpus is 96 pairs (16 per key); the last A730 pairs (frame 8682) are at
  the run's pitch/meter end. No class is >50 % of both jobs and time, so the
  conditional single-class 0x21 kernel was **not** built.
- **Meter gating missed.** The second run's trigger was
  `CAPTURE_PREFIX.trigger`, which the walker capture consumes and deletes
  (`rsx_draw_engine.c:976-1000`); the tail hook polls and missed it, so the
  exact meter-window class shares are still unmeasured. This is a capture
  gap, not a GPU-cost gap; a future run needs an unconsumed trigger.
- **Burst dispatch is cheap.** The exact 0x2A kernel over 429 jobs in ONE
  `vkCmdDispatch` is 0.23-0.27 ms device-local (0.54-0.62 us/job; 12.6 ms
  host-visible, PCIe-bound) with ~2-4 us CPU record/submit. The whole tail is
  well under the ~5 ms kill threshold; option C is not killed on GPU cost.
- **Verdict: GO on cost, coverage is the blocker.** At least three 0x21
  classes with a ~7x per-call spread; any one-class kernel leaves 27-90 % of
  0x21 jobs on the SPU. The corpus's command-boundary 0x2A pairs are also
  unreliable when two 0x2A commands run back to back (0x2A returns normally;
  capture it at function exit next). The validated `fncap21b/0x10F78` capture
  remains the exact 0x2A reference. No walker binding, no runtime key, no A/B.

## 2026-11-04 (c690 GPU phase 2a): 0x21-only is dead, the tail must move; 0x2A kernel exact

Two pitch-meter capture runs (`ab_live.sh`, build-play, governor performance;
2/3 launches) plus offline static analysis and a headless Vulkan kernel.
Evidence and plan in [`docs/edge-gpu-phase2a.md`](edge-gpu-phase2a.md).

- **The phase-2a premise is contradicted.** A GPU 0x21 that leaves its result
  in VRAM removes **0 ms** of SPU critical-lane time. On the real captured
  jobs the 0x2A assembler reads **all four** 0x21 output arrays
  (normal 200/200, position 200/200, colour 200/200, uv0 199/199 lines); the
  0x25 cull reads only its position array (235/235 lines, zero normals) and
  0x26 reads the position block plus one `0xB0` normal slice.
- **What moves.** 0x21 is 42.9 % of the c690 wall (~10.5 ms of the 24.39 ms
  critical lane), 0x2A 15.7 % (~3.83 ms), cull 12.7 % (~3.10 ms). The full
  tail removes ~17.4 ms (burst 35.0 -> ~17.6 ms); 0x21+0x2A removes ~14.3 ms.
  0x21 alone removes nothing. Phase 2a must be the whole tail.
- **0x2A GPU kernel built and exact.** `tools/replay/c690_2a_assembler.comp`
  + `test_c690_2a_shader.c`: bit-exact on two real captures and on a synthetic
  colour stress (0 differing bytes, max float 0, max u8 0). Lifted 0x10F78 is
  14.5-19.5 us/call; the GPU kernel is 10.7-14.2 us/dispatch at N=64, i.e.
  overhead-bound. It is a building block, not a win by itself.
- **0x21 shader not signed off.** The real 0x13140 at the pitch is the A730
  format-conversion/table path (jump tables at LS 0x143D0..0x14480), not the
  validated 12/12/11 stream. The BB80 body also runs (0xBED0/C018 captured)
  but 0xBB80 is a direct call, so its 0x13140 input was not captured. A
  stage-level corpus is the first 2b task.
- **Verdict: KILL 2a as scoped.** No walker binding, no A/B, no runtime key,
  no `spu_context` change (ABI check passes), nothing default-on. 2b: full
  tail (0x21 + cull + 0x2A) in one per-burst submission, gate burst <= 28 ms
  and p50 <= 30 ms with fallback <= 5 %.

## 2026-11-04 (c690 GPU phase 1): no CPU reads the mesh, so a GPU tail can feed the walker directly

Offline only (no game launch). Static analysis of the EBOOT/lifted c690 module,
the corpus and our walker, plus one Vulkan transport microbenchmark; design and
evidence in [`docs/edge-gpu-phase1.md`](edge-gpu-phase1.md).

- **Consumer map is closed.** The five 2 MB mesh rings (EA
  `0x11900000`-`0x12300000`), the kept-index list and the 0x90-byte command
  hole are read **only by the RSX walker**. The guest PPU reads only per-SPU
  job-list counters (`WwsJobList_SpinUntilDone`); the ICE draw builder
  (`FUN_000730c8`) copies vertex/index **EAs** into methods and never
  dereferences them. No CPU read of post-transform vertices or post-cull
  counts.
- **The hand-off is the inFAMOUS shape.** The job writes the data first and
  patches the self-JUMP command hole last; the walker parks until the hole is
  patched (`cellGcmSys.c:3287-3331`), and the ring labels are unchanged. A GPU
  producer can keep that contract with no payload readback.
- **But 0x21 alone is fence-bound.** 0x21 is 42.9 % of the c690 wall
  (390 us/call sel 1), and the same SPU job's cull/index/assembler consume its
  LS output. A per-job GPU->LS readback costs one fence; `record+submit+wait`
  is 32.8 us p50 (10k iters, this host), 429 jobs = 14.1 ms/frame serialised.
  Option (iii) is no-go.
- **Recommendation: option (i).** GPU-resident geometric tail
  (unpack/transform/pack/cull/assemble) into a Vulkan ring buffer; SPU keeps
  the job ABI, ring allocation, command hole and fallback; the walker binds
  the GPU buffer. Option (ii) only works with per-frame batching and a
  GPU-resident payload (then it is (i) plus a small readback).
- **Prototype:** reconstructed a real 0x13140 invocation from the existing
  corpus (384 vertices / 96 iterations, output `0x1F750..0x20F4B`) and
  snapshotted both format paths (A730 and BB80); Vulkan transport cost
  measured (32.8 us fence, 1.9 us/dispatch batched N=64). The bit-exact BB80
  shader is not signed off because the offline corpus only witnessed the A730
  variant. One capture command for the user is in the doc (SPU_FN_CAPTURE of
  0x13140/0xBB80/BED0/C018/A730/B830 at the meter); nothing was enabled or
  added to the default build.
- **Kill criterion for phase 2a:** >1 LSB on >0.1 % of packed values or any
  cull change, fallback >5 %, or burst wall/p50 not <=30 ms in the A/B.

## 2026-11-04 (team logo capture): the capture is valid, and the 3-D logo is never submitted

Offline only (no game launch). The user's armed team-select capture was
replayed with `tools/fifo_dump_scan.py` and `replay_rsx`; the plan, the
capture hashes, the scan figure and the next diagnostic run are in
[`docs/team-logo.md`](team-logo.md) §5.5. Branch `exp/live-ab`, build
`recomp-work/build-play`.

- **The raw capture is complete:** 116,280 B / 29,070 big-endian words, 12
  `0xFEAD` flip markers, ends on the last marker. Fixed scanner: **12 frames,
  657 draw batches, 0 triangle batches**. `replay_rsx` dispatches the same 657
  draws / 6 clears and is deterministic (state hash `0x9a78d56f446ad930`).
- **The optional walker capture is complete but the wrong flip:** `.fifo` is
  292 B (32-byte `RSXF` + 65 words, ends `0xFEAD0001`), the 1-draw
  display-copy frame, not a UI frame. `.state` 148,008 B, `.mem` 10,012,744 B.
- **No 3-D draw anywhere.** The 6 full UI flips are 107/107/106/106/106/107
  primitive-8 quads (109/109/108/108/108/109 dispatched `0x1814`; the two
  extra are a second word in a non-incrementing packet). The 6 copy flips are
  one quad each sampling `0x01110000` into `0x00890000`. Only two surfaces
  appear in the whole stream (main `0x01110000` fmt `0x3148`, copy
  `0x00890000` fmt `0x0148`); there is no `ScriptUIElem_RT` pass and no draw
  that samples a non-main surface. The logo is lost before submission, so
  culling/blend/RT-resolve/texture decode are ruled out for this symptom.
- **Likely silent site:** `FUN_009d7b30` target-texture lookup
  (`FUN_009d4c68`, "world" scene) returning 0, or the script never calling the
  element constructor. The title loads its debug flags from the missing
  `BOOT/CONFIG_MAIN.TXT`, so `DisableUIElem` is not set; the failure path logs
  only with `StressTestUIElem`. Next run (documented, no code change): create
  the VFS config with `StressTestUIElem` and grep for
  `Did not find target texture`.
- **No logo fix was implemented** because the draw never reaches the
  renderer; a speculative one could not be proven offline.
- **Tool fixes (offline):** `tools/fifo_dump_scan.py` now counts each
  dispatched data word of a non-incrementing `0x1814`/`0x1824` packet (645 ->
  657) and reports 12 not 13 frames; `tools/test_fifo_dump_scan.py` PASS 5/5.
  `tools/replay_rsx_core.c` maps the full 32-bit guest window so the raw dump
  no longer segfaults on the NV3062 inline write to local `0xC0004390`; the
  pitch dump is unchanged (646 draws, deterministic).

## 2026-11-04 (c690 0x21): the emit/ring wall is compute, not a wait

One meter launch (`ab_live_op21meas_20261104_043452`), `N=3 x 10 s` (2 s
dropped), `PIN=0 WCB=2`, standard `ab_live.sh` plus `METER=1 SKIP_CINE=1
PITCH_THRESH=35` and the new `PS3RECOMP_C690_OP21=1`. Full tables, static
trace, and the fix estimate: [`docs/c690-emit-ring.md`](c690-emit-ring.md).
Branch `exp/live-ab`, build `recomp-work/build-play`.

- **0x21 is 99.9 % compute, 0 % wait.** 619,067 calls, 64.99 s inclusive
  (43.4 % of the command wall), mean **104.98 us**, p50 54, p90 112, p99 926.
  The run issued **0** MFC GET/PUT, **0** GETLLAR/PUTLLC, **0** tag-status,
  **0** atomic-status and **0** decrementer reads during the command.
- **The time is two emit/format sub-selectors.** `0x13080` dispatches through
  an 8-entry table at LS 0x1311C: `0x13140 -> 0xBB80` is 15.7 % of calls but
  **58.2 %** of the time (**390.4 us/call**); `0x131A0 -> 0xBB98/A3E0` is
  84.3 % of calls and 41.5 % of the time (**51.7 us/call**). Both run the
  `0xBCB4`/`0xA410` float-transform/pack kernels (`fm`/`fma`/`shufb`); the
  first is a larger batch.
- **The ring/DMA association was wrong.** The measured path never calls the
  ring allocator (0xE0A8), never reserves a line and never DMAs; those live in
  the inter-job gap. Walker publishing, cheaper reservations, a larger ring
  and DMA-emulation speed are ruled out for this command.
- **Native emit is the only lever and is not small.** At 1.3x on both
  selectors it saves ~2.9 ms of the 24.4 ms critical-lane busy (burst 35.0 ->
  ~32 ms); 2x saves ~5.2 ms (~29.8 ms). That stays on the 34.15 ms p50 floor
  and is near the parity of the existing native stages. Nothing was
  implemented, no live key was added; the second launch was not used.

## 2026-11-04 (native c690 step 5): the meter stage order is emit/ring, not unpack/pack; per-stage SIMD is at parity

Two 90 s meter measurement launches (`ab_live_stage5meas_20261104_040459`,
`ab_live_stage5meas2_20261104_041417`), `PIN=0 WCB=2`, standard
`ab_live.sh` plus `METER=1 SKIP_CINE=1 PITCH_THRESH=35`. Full tables and the
decision: [`docs/edge-native-step5.md`](edge-native-step5.md). Branch
`exp/live-ab`, build `recomp-work/build-play`.

- **The old stage shares are not the meter.** Per-command inclusive time on
  2,038,861 jobs: **0x21 emit/ring 42.9 % (102 us/call)**, 0x2A assembler
  15.7 %, 0x25+0x26 cull 12.7 %, 0x13 copy 9.7 %, 0x0A bind/convert 7.5 %,
  entry/parse 5.6 %. The sum-check is exact (-0.0002 % vs the job wall).
- **The geometry stages are 23.2 % of the wall.** Unpack/pack/skin/normalize
  live in 0x0A+0x2A; even 3x on them plus the cull and the 0x13 copy leaves the
  burst at ~27.6 ms, not <= 25. The 0x21 emit/ring path is the largest term
  and is not a named stage.
- **Per-function SIMD is at parity.** Existing offline gates: 0x7C18 0.99x
  (8-wide AVX2, this session), 0x6D68 1.00x and 0xAF98+B238 1.03x (commit
  `e3386c5`), 0xFDA0 1.30x, 0xD080 1.53x. The compiler already emits the same
  vpshufb/vpsllvd/vfmadd mix; no stage reaches 3x.
- **No native stage, no A/B, no default change.** Two of three launches used;
  the measurement changes (62-entry named list + per-command timer) are kept
  under `PS3RECOMP_C690_STATS`; no `spu_context` field was added.

## 2026-11-04 (frame-pacing floor): 34.15 ms p50 is the c690 producer, not the flip/present path

One windowed meter launch (`ab_live_pacingfloor_20261104_033642`), N=4 x 10 s
per arm, 2 s dropped, `PIN=0 WCB=2`, fifo, plus a 90 s PSTL timeline (774 MiB,
deleted). Full tables, the frame timeline and the code:
[`docs/frame-pacing-floor.md`](frame-pacing-floor.md). Branch `exp/live-ab`,
build `recomp-work/build-play`.

- **Hard frames are producer-bound.** A hard meter frame is `c690 lane union
  32.81 ms` in a `34.28 ms` frame; the next frame's `SET_REFERENCE` fence is
  published **0.26 ms** before the frame ends. `dur - c690 union` is 3.2 ms.
- **No 20-32 ms population.** Arm A: 22.7 % of flips 8-16 ms, 7.5 % 16-20,
  **0.5 % 20-32**, 40.3 % 34-36. Frames either include the c690 burst (~34) or
  not (~14-18); there is no early-done work rounded up to 34.15.
- **34.15 is the guest's 2 x 17 ms deadline, not the cause.** The host lwcond
  rounds 17000 us up to 17 ms, so the two deadline timeouts are 34.13 ms; the
  branch fires because the next buffer's fence lands at ~34.4 ms (timeline
  sample: request 0, present 4.36, drain 10.18-33.74, fence 33.738, deadline
  34.128, tick completion ~36.7, handler ~40.1).
- **Completing earlier loses.** Fixed `TICK_IN_DRAIN` (B) and the marker-side
  fence completion (C) both reproduce `FLIP_ON_DONE`: fps **33.91 -> 25.47 /
  24.56**, p50 **34.15 -> 38.05 / 38.12**, deadline frames 431 -> 16 / 749 per
  32 s; the fast mode disappears and the ref-wait sum rises 28.2 -> 48.9 ms.
  Neither key is enabled.
- **Present is not on the guest path.** Dedicated present thread;
  `vkQueuePresentKHR` 0-2 ms (one 10 ms), and a 1,139 ms driver stall dropped
  7 presents while the guest kept flipping at 34 ms. Present mode is fixed at
  swapchain creation and only affects the window thread.
- **Review fixes from `ec4c75cf`:** `presented_seq` now carries the queued
  image's sequence (direct and FIFO-marker path), the three completion copies
  share one helper, and `test_gcm_tick_in_drain` covers the marker identity,
  tick-in-drain and fence gates. 30/30 video fixtures pass; `spu_context` ABI
  check passes.
- **Next lever unchanged:** c690 / walker. RPCS3's ~20 ms is a faster
  producer, not a different flip edge; the completion path can recover at most
  0.26 ms of measured slack.

## 2026-11-04 (c690 dispatch turnover): the host phase is arbitration, not overhead; cuts lose frames

One launch at the pitch meter (N=6 x 10 s per arm, 2 s dropped,
`PIN=0 WCB=2`, governor `performance`) plus a 95 s timeline window. Full
tables, the code and the sixth-lane evidence:
[`docs/c690-dispatch-cuts.md`](c690-dispatch-cuts.md). Branch `exp/live-ab`,
build `recomp-work/build-play`, commit `84e5dd06`.

- **Gap anatomy.** Big bursts (arm A): 34.25 ms wall, 70.3 % lane occupancy,
  29.4 % of lane-wall in gaps = **28.5 % tail / 46.8 % host / 24.7 % head**.
  The PM is lifted (`wwsjob_pm`, image 6), not interpreted; the 67 KiB job
  re-DMA is ~0.9 GB/s and cache-resident, which is why `SPU_CODE_DMA_MEMO`
  was flat. Atomics are 0.3 % of gap time.
- **Three default-off cuts** (`SPURS_PM_FAST_ACTIVE`, `SPURS_PM_FAST_ONE_LOCK`,
  `SPURS_PM_FAST_GEN`): a lane already at maxContention may rerun; one
  lock-line acquisition for check+consume; the dispatch-time workload snapshot
  is trusted while the registry epoch holds. Unit tests and the `spu_context`
  ABI check pass.
- **Live A/B.** Fast-rerun share 34.5 -> 85.5 %; host phase **23.54 -> 9.19 ms
  per burst (-61 %)**, all-lane gap 29.4 -> 22.6 %, occupancy 70.3 -> 76.6 %,
  burst wall **34.25 -> 30.99 ms**. But guest fps **35.30 -> 33.29** (paired
  +2.00 +/- 1.52, 95 % CI [0.48, 3.53]), walker wait 11.32 -> 12.26 ms,
  p50 unchanged at 34.15 ms (two-vsync floor). c690 jobs/s and job wall are
  unchanged, so this is not extra work.
- **Reading.** At maxContention the coordinator round trip is the arbitration
  point for seven workloads on six lanes. Removing it compresses the c690
  burst but delays DXT/Crowd/synth/MP3/Kinematics, and the frame loses more
  than the burst gains. A fairness-aware rerun (yield when another workload
  is ready) is the next candidate; it is not built.
- **Sixth lane: no safe version.** `nSpus=6`, `MeshJobs0/1 maxC=5`,
  `priority[5]=0`; the five-instance protocol is the guest's. The prior
  `SPURS_LANE6=1` probe wedged in ~10 s, and lane 5 serves DXT/audio. Not
  re-run.
- **Default:** nothing enabled. Both `play_windowed.sh` copies and
  `ab_live.sh` keep `SPURS_PM_FAST_RESCHED=8` only. Timeline (775 MiB)
  deleted after analysis.

## 2026-11-04 (native screen cull step 4): exact 0xD080 stage is 1.53x offline, neutral live; the union still costs ~5 fps

Three launches (the hard cap): a shadow/live soak, the A/B at the meter, and
a timeline mapping run. Details, tables and the gate in
[`docs/edge-native-step4.md`](edge-native-step4.md). Branch `exp/live-ab`,
build `recomp-work/build-play`, `METER=1 SKIP_CINE=1`, `PITCH_THRESH=35`,
`PIN=0 WCB=2`, governor `performance`.

- **Native stage.** The 24-block 0xD080 tail-call subtree is flattened to one
  function (`c690_native_screen_stage.inc`): `sr0..sr97` stay live, per-block
  loads disappear, stores stay so the terminal writeback is exact. It replaces
  the two `spu_link(0x13400) ... 0xD080` brackets in the 0x25/0x26 paths
  (`patch_native_screen.py`); the shared WWS ring helpers stay lifted. No
  `spu_context` field was added (ABI check PASS).
- **Exactness.** 400 captured pairs: native mode 0 matches the **full LS, all
  128 GPRs and the PC** (0 differences). Live shadow: **884,058 calls, 0
  mismatches**; the A/B run then made **1,690,624 live calls with 0
  fallbacks** and no wedge.
- **Speed.** Offline whole-command body **22.9 -> 15.0 us/call (1.53x)**;
  native-assisted union **122.9 -> 107.9 us (1.14x)**. Live c690 burst wall
  **36.65 -> 35.70 ms** (-2.6 %), job wall 2.74 -> 2.68 lane-s/s, lane busy
  0.548 -> 0.537.
- **A/B (N=6 x 10 s).** A 34.98 fps; B (native 0xFDA0 + cull mode 0) 34.94
  (paired -0.04 +/- 0.60); C (union) 30.31 (paired **-4.67 +/- 0.65**, p90
  40.67 -> 50.19 ms). Mapping run: B +0.21 +/- 0.08, C -5.17 +/- 0.47.
- **Not a fused union.** The kept-index count is finalised by the 0x132E0
  caller *after* 0xD080 returns, so the 0xD080 hook cannot know the final
  list; mode 1 reuses the proven union driver (two rule evaluations, native
  stage in both) and reproduces the lifted union exactly (7 added, 0 removed
  on 400 pairs). A true one-pass union needs the caller-side count writeback
  modelled.
- **Default:** nothing enabled. B does not win cleanly and C fails the
  < 1 fps gate, so `PS3RECOMP_C690_NATIVE_SCREEN` stays 0 in both
  `play_windowed.sh` copies and `ab_live.sh`. Play-test command for the holes
  and cinematics is in §6 of the doc.
- **Deviations:** cinematics were left to the user's windowed run; the first
  A/B timeline was accidentally in hitch mode, so a third launch measured the
  burst wall.

## 2026-11-03 (native DXT step 3): the kernel is lifted, bit-exact, and the boot hitches are gone

Two launches (the hard cap), both start screen, `PIN=1 WCB=2`, capture on:
A `ab_live_dxtliftA_20261103_225911` off, B `ab_live_dxtliftB_20261103_230713`
on. Details, tables and the gate in
[`docs/dxt-native-step3.md`](dxt-native-step3.md).

- **Lifted.** `lift_jobmods.py --engine-elf` accepts a header link base of 0
  (PIC) and lifts the module at `0x4000`, entry `0x4030`, source EA
  `0x010A1700`, image 207, verified span `[0x4030,0x67DC)`. Thirteen PIC
  call-stub/function-table entries are seeded (`0x4050, 0x5318, 0x6010,
  0x603C.., 0x61BC, 0x6734, 0x6748, 0x67D4`); without them the lift stopped at
  `0x600C` and the encode/DMA body stayed interpreted. 300 functions, 0
  unsupported. Generated as `jobmods_register_dxt.c`, gated by
  `PS3RECOMP_DXT_LIFT` (default on).
- **Instrumentation gap closed.** The in-context rejoin
  (`spu_smc_microstep -> spu_interp_run_until`) now emits `spu.interp` with the
  instruction count in `b`. A reports 35,426 spans / 18.51 s; 18.50 s and
  240,988,752 instructions are the DXT range. The earlier "zero spu.interp"
  attribution was the missing hook, not lifted compute.
- **Gate: 100 %.** Offline replay of the lifted kernel (plus the WWS policy
  module's lifted helpers) on the B entry states and DMA: **346/346 complete
  entries, 5,101 PUTs, 890,688 block pairs, 0 mismatched, 0 missing input
  GETs** (one torn tail skipped). Lifted **136 ns/block** (0.121 s total);
  interpreter **~20.8 us/block** lane time (18.50 s / 890,688).
- **A/B at the start screen.** A: 14 hitches / 5,389 ms on the timeline (flip
  gaps >250 ms: 11 / 7,623 ms). B: no timeline record >=250 ms, one 308 ms
  flip gap, 621 flips vs 600, no wedge. Wid-0 policy lane time from the
  capture: **11,722 ms -> 867 ms**, on a larger B workload (344 vs 273
  captured policy runs).
- **Default:** the lift stays on; `PS3RECOMP_DXT_LIFT=0` is the off arm.
  Play test: `cd recomp-work && ./play_windowed.sh`, start screen, then a game
  and an instant replay. The encoder is not started; its plan is in §5 of the
  doc.
- **Deviations:** A's raw `frame.ppm` was overwritten by B before copying, so
  no direct A/B pixel diff (B's frame is saved as
  `recomp-work/dxtlift_B_frame.png`); B's timeline is empty by design (no
  hitch >=250 ms) and its lane time comes from the capture timestamps. Both
  arms ran the entry capture (~230 MiB each), so the A/B is conservative.

## 2026-11-03 (native DXT step 2): the kernel is an unlifted module; stop at the gate

One launch (`ab_live_dxtcap1_20261103_224034`, start screen, `PIN=1 WCB=2`).
Details, tables and the corpus numbers in
[`docs/dxt-native-step2.md`](dxt-native-step2.md).

- **The DXT kernel is the unlifted base-0 module at `0x010A1700`.** Every
  wid-0 job loads 0x2C10 bytes from that EA to LS `0x4000` on all four lanes;
  header `42465602431A9282422D77024363A382`, entry `0x30`, image `0x2C30`,
  load base 0. `spu_overlay_match_sig` is 0: none of the seven lifted jobmods
  is the DXT code, and the module runs through the interpreter rejoin
  (`0x4030` has no lift; `spu_smc_microstep` -> `spu_interp_run_until`).
- **The capture is complete.** New default-off hook
  `PS3RECOMP_DXT_CAPTURE=<dir>` spools wid-0 GET/PUT plus code/param records:
  93 MiB, 304 code records (300 clean jobs), 327 parameter blocks, 53.8 MB input /
  841,340 blocks, 6.4 MB DXT1 (801,308 blocks) + 0.6 MB DXT5 (40,032).
  Each 16 KiB input GET maps to one 2 KiB (DXT1) or 4 KiB (DXT5) PUT; the
  parameter `+0x20` output EA equals the first PUT EA on all 300 jobs.
- **Parameter block correction:** it sits at +0x70 of the 0xA0-byte job
  source; the four swizzle words are host-endian (all `0x03020100`), the rest
  is big-endian. No DXT3 in this sample.
- **Stopped per the phase-1 rule.** The required gate is native vs the lifted
  kernel in isolation, and the kernel is not lifted; `lift_jobmods.py`
  rejects load base 0. Next step: lift `0x010A1700` (base 0x4000, entry
  0x4030) and re-run the 841,340-block gate before any encoder work.
- **Deviations:** one launch of the three allowed; no shadow/live A/B and no
  encoder because the gate premise is absent. Governor `performance`, no
  system or test-setup change. Corpus kept under `recomp-work/dxt_cap1`
  (below the 1 GiB cleanup threshold).

## 2026-11-03 (native Edge step 3): live wedge fixed, cull family identified, native stage neutral

Three launches (`ab_live_edge3s1_20261103_205745` wedge,
`ab_live_edge3s2_20261103_210626` shadow + corpus,
`ab_live_edge3s3_20261103_212337` A/B). Details and tables in
[`docs/edge-native-step3.md`](edge-native-step3.md).

- **Live wedge root cause: the native kernel never published its return PC.**
  The lifted `0xFDA0` body ends in `SPU_RET` (`pc = gpr[0]`, trampoline at
  `host_depth == 0`); the flattened kernel only saved LS+GPR, and the shadow
  gate compared LS+GPR only. Publishing `pc`/trampoline in the live path and
  adding a pc comparison to shadow fixes it: **130,304 live native calls, 0
  fallbacks, 0 mismatches, six 20 s segments, no wedge.**
- **Holes: 0x25/0x26, not 0x2C.** `0x2C` is **0.0 %** of the command
  histogram in both the user's 20:26 play test and our meter run; `0x25` is
  3.8 %, `0x26` 1.0-1.2 %. `MLB16_CULL_BYPASS=3` is the family test.
- **Cull corpus + exact replay.** 400 meter cull pairs captured; the
  unpatched replay matches the game's LS output on all 400. The screen
  transform block is `{scale_x, offset_x, scale_y, offset_y}` (720p
  `{595,640,-335,360}`, four copies). Scaling x into sample space changes
  35/400 lists but also drops triangles on 77-104 pairs (separate fixed
  bound), so the experimental switch stays default-off; a safe fix needs the
  bound or an index-list union.
- **Speed: native stage neutral.** A/B N=3x20 s, 2 s drop: A 32.36 fps,
  B 32.68 fps, paired -0.32 +/- 1.97 fps (CI covers 0); per-flip p50 34.15
  both, p90 41.61 -> 40.95, p99 48.36 -> 46.78, std 10.80 -> 10.63 ms.
  No clean winner; native stays off in the play defaults.
- **New switches (default off):** `PS3RECOMP_C690_CULL_FIX[_2C]` (experimental
  sample-grid scale), `PS3RECOMP_C690_CULL_PAIRS/_STRIDE/_GAP_MS` (cull spool),
  and the existing native/shadow keys now report progress every 256 calls.
- **Deviations:** the A/B is N=3 x 20 s (not N=6 x 10 s) because launch 1 was
  spent on the wedge; the cull capture is 400 pairs (not 20). No system or
  test-setup change, governor `performance`.

## 2026-11-03 (hitch census): the 5.1 s load gap is shader translation, boot gaps are SPU jobs

Two launches (`ab_live_hitchcold2_20261103_155504` cold census,
`ab_live_hitchwarm_20261103_160035` warm pipeline cache), plus one discarded
first attempt. Details, tables and artifacts in
[`docs/hitch-census.md`](hitch-census.md).

- **824 frames > 50 ms** in the 318 s cold run (10,051 flips): 67.4 s of
  frame time, 39.9 s above a 33.3 ms budget. Boot 18/5.92 s, menus **0**,
  match load 1/5.11 s, pregame cinematics 798/55.97 s, pitch meter 7/0.37 s.
- **5,112 ms match-load gap = first-use shader translation.** The walker
  drains 3.74 s, of which 2.95 s is our HLSL -> SPIR-V translation (352
  spans, 176 pipelines) and 0.25 s `vkCreateGraphicsPipelines`; tex 0.31 s,
  surface 0.04 s, c690 0.24 s. The frame thread waits 5.10 s on its flip
  lwcond and main waits 3.79 s on ref + 1.15 s in usleep. File I/O is **20
  reads / 514 KiB / 0.16 ms**, all on `ppu DataStream Process Thread`; none
  on the frame/main thread, so this is not synchronous `cellFsRead`.
- **237-654 ms boot gaps = SPU asset loading.** 18 boot frames of
  353-589 ms; e.g. 589 ms: DisplayFlip flip lwcond 577 ms, main `usleep`
  576 ms over 2,300 calls, walker idle 569 ms, six SPU lanes 1.53 s
  lane-union; one 40 KiB/0.02 ms read, no pipeline. The menus themselves
  had no >50 ms frame, so the old "boot/menu" label lands in boot.
- **Cinematics are c690.** 790/798 cinematic hitches are `c690.job`-
  dominated (79.1 s lane-union); six are first-use shader translation.
  Same producer/drain as the meter.
- **Persistent `VkPipelineCache` is not the fix.** Implemented behind
  `PS3RECOMP_VK_PIPELINE_CACHE` (default off, keyed by
  `pipelineCacheUUID`+driver). Cold -> warm: driver `pipe.create` 322 ->
  33 ms whole run, 248 -> 19 ms in the load frame, but `shader.compile`
  2.95 -> 3.01 s and the load frame 5.11 -> 5.26 s. First 261 s: 66.9 ->
  66.2 s of hitch time (-1.1 %, inside variation). Cache removes ~0.3 s
  of 67 s; a disk cache of our translated SPIR-V would target ~3.8 s.
- **Instrumentation:** hitch-only timeline (`PS3RECOMP_TIMELINE_HITCH_US`,
  new `pipe.create`/`shader.compile`/`tex.upload`/`surface.create`/
  `fs.read`/SPURS-add/remove/overlay-verify/resync events), and
  `tools/analyse_hitches.py` + `tools/hitch_run.py`. Raw 229 MB/804 MB
  timelines deleted after analysis; `check_spu_context_abi.sh` passes.
- **Deviation:** the warm run did not reach the pitch menu (stayed in
  cinematics ~800 s); the comparison uses the first 261 s that both runs
  followed identically. The cold run reached the meter at 264 s. No
  changes to the play scripts; the cache switch stays off.

## 2026-11-03 (flip deadline): the tick is starved by the walker drain; earlier completion loses

Two meter launches (`ab_live_flipdiag_20261103_150246` diagnosis,
`ab_live_prepflip_20261103_151553` A/B). Details in
[`docs/flip-deadline.md`](flip-deadline.md).

- **The 33 ms mode is the title's 2-vblank deadline, and it fires often.**
  369/1,289 frames (28.6 %) at the meter left `Gfx_DisplayFlip` by its
  `2 x 16683 us = 33.37 ms` deadline; they are a tight **34.15 ms**. The wait
  is `sys_lwcond_wait(0x1245100, 17000 us)`; two 17 ms timeouts reach the
  deadline. When the completion arrives it is fast: request -> present p50
  2.45 ms, request -> tick completion 6.75 ms, handler +0.1 ms, wait exit
  +0.02 ms.
- **The flip is presentable; the completion beat is late.** Deadline frames
  queue the present p50 0.35 ms after the request (368/369 before the
  deadline), but no completion beat lands in the frame. At the deadline the
  walker drain is active 80.2 % of the time and a c690 job 59.9 %; present
  fence and `vkQueuePresentKHR` 0 %. The display tick and the drain share the
  `rsx walker` thread, so a 23.3 ms drain (+10.8 ms) spans the deadline; the
  next completion lands p50 7.5 ms after it.
- **Neither candidate wins.** `FLIP_ON_DONE` was already 34.06 -> 26.00 fps
  (ref hand-off); a new default-off `PS3RECOMP_PREPARE_FLIP_KEEP_STATUS`
  (do not let `SetPrepareFlip` complete the outstanding flip) is **32.54 ->
  28.21 fps**, p50 34.15 -> 37.13 ms, walker 17.75 -> 21.84 ms, worse on all
  six paired segments, and it does not remove the deadline frames (823 -> 776
  per 60 s arm). Both switches stay off.
- **No >200 ms frames at the meter with batch 1 on**: max 83.96 ms; the run's
  18 gaps >200 ms (237-654 ms, one 5,152 ms) are all pre-meter asset loads /
  match load, not lwcond timeouts.

## 2026-11-03 (batch 1): lwcond, audio pacing, fs write, SPURS teardown, GCM offset tables

Five audit findings fixed, each behind its own default-off switch; the play
scripts now default them on. Details, line references and the test list are in
[`docs/hle-fixes-batch1.md`](hle-fixes-batch1.md).

- **#9 `PS3_LWCOND_REAL`**: the real wait/signal/timeout core already existed
  (it was left off as neutral, not because it was broken). Its gap was destroy:
  lwcond now returns EBUSY while parked and otherwise frees the slot
  (tombstone, so the probe chain survives), and lwmutex destroy does the same
  for its 24-byte structure. Live A/B: poll 314,893 waits/57 timeouts vs real
  34,336 waits/4,619 timeouts in the measured windows; the flip-done cond left
  by signal 1,236 times and by deadline 822 times. A boot handshake still
  relies on the designed 10 ms fallback wake.
- **#6 `PS3RECOMP_AUDIO_PACE`**: POSIX cellAudio now uses backend room or a
  monotonic 5333 us deadline instead of 2/5 ms sleeps. A/B audio 190.7 ->
  187.5 blocks/s; armed from boot it is 187.5 from the first window.
- **#11 `PS3RECOMP_FS_WRITE_CHECK`**: `cellFsWrite` (runtime and libs paths,
  plus `cellFsUtilWriteFile`) returns EIO on a short/failed write instead of
  OK, through one shared predicate.
- **#2+#3 `PS3RECOMP_SPURS_TEARDOWN`**: core SPURS base is `0x80410800` (BUSY
  `0x8041080A`), fiber `0x80410700`; Shutdown stops dispatch and Wait drains
  `active[wid]` with a bound (default 2 s), returning BUSY on expiry. Off, the
  old swapped BUSY literal is preserved so the stub cannot cause a hot spin.
- **#7 `PS3RECOMP_GCM_OFFSET_SYNC`**: populate/clear also write the guest
  io/ea offset tables, and init/reset republish them. Clear now rounds partial
  pages up like populate.

Live A/B at the pitch meter (N=6, 10 s, 2 s drop, one launch): fps A
**32.42 +/- 0.97**, B **32.24 +/- 0.23**, paired **+0.19 [-0.97, +1.34]**;
per-flip fin p50/p90/p99 A 34.1/41.5/49.5 ms, B 34.2/41.4/47.7 ms; walker and
wait unchanged. B's std is inflated by one segment with three long gaps (max
921 ms, still 32 fps); without it std 13.4 vs 10.7 ms. A second launch with all
five on from boot navigated menus and a match load with `SKIP_CINE=0` and
reached the pitch in 246 s, no hang, audio 187.5 blocks/s, SPURS workloads 0-6
dispatched normally.

Offline: new tests for each switch pass; `check_spu_context_abi.sh` passes;
video fixtures 28/29 (the pre-existing `test_gcm_fifo_user_command` link
failure). Full `tools/test_*.py` sweep: 46 failures at HEAD, 45 now (the batch
fixes `test_lwcond_layout.py` and `test_gcm_io_mappings.py`); no new failure.

## 2026-11-03 (HLE contract audit): how many more return/out-param bugs?

Static-only audit of every imported HLE function this title calls, prompted by
the `cellGcmSetPrepareFlip` bug. **433** imports; **407** called (**1,604** call
sites); **26** never called. At **548** call sites the lifted code reads `r3`;
**234** imports have at least one such site. **32** contract mismatches found;
none fixed, no game run.

Top ones (guest addresses, evidence and one-line fixes in
[`docs/hle-contract-audit.md`](hle-contract-audit.md)):

- `_cellSpursTaskAttributeInitialize` mis-decodes the descriptor form used by
  the Bink SPU task: context becomes the stack descriptor address, size 0,
  lsPattern 0; the task parks in `WAIT_SIGNAL` (11-02 log).
- The SPURS core error base is the fiber base (`0x80410700` instead of
  `0x80410800`), so the guest's `RemoveWorkload == 0x8041080A` BUSY test never
  matches; `ShutdownWorkload`/`WaitForWorkloadShutdown` are also success stubs.
- `cellAudioOutGetState` writes downMixer/soundMode at `+4/+8` instead of
  `+8/+12`; the Bink path reads `soundMode.channel` at `+13` and gets 0,
  choosing 6/8-channel instead of stereo.
- `cellAdecOpenEx` is not implemented and the first handle is 0, which the
  guest rejects; streamed AAC/ATRAC audio never opens.
- The POSIX `cellAudio` mixer free-runs at 200-500 blocks/s vs 187.5 and
  publishes that read index to the guest.
- GCM offset tables are published only by `cellGcmGetOffsetTable`, so pages
  from a later `cellGcmMapMainMemory` stay `0xFFFF` in guest memory.
- `cellFsRename`/`cellFsTruncate` use the libs `./gamedata` mapping while
  open/write use `$PS3_VFS_ROOT`, so renames of installed USRDIR files fail.
- Default lwcond wait polls 1 ms and ignores timeouts; lwmutex destroy is a
  no-op.
- `cellSearch*` is entirely unimplemented (fake success, no callback or
  out-params).
- `cellFsWrite` returns OK on a short/failed write (cache worker can spin);
  `cellSaveDataDelete2` is a no-op; `cellGameDataCheck` reports `NOTCALC` for
  `sizeKB` and `cellGameGetSizeKB` always writes 0.

## 2026-11-03 (backward frames): `cellGcmSetPrepareFlip` returned 0, so every flip named buffer 0

The whole-frame backward step is fixed. The title renders alternately into
display buffers 0 (`0x10000`) and 1 (`0x890000`), but our
`cellGcmSetPrepareFlip` returned `CELL_OK` (0) instead of the buffer id. The
title stores that value in its flip queue (`Gfx_FrameEnd`, 0x00066530) and its
display thread passes it, masked to 8 bits, to `cellGcmSetFlipImmediate`
(`Gfx_DisplayFlip`, 0x000692dc), so every flip named buffer 0. When buffer 0
still held a frame from two frames earlier, the window showed that image after
a newer buffer-1 frame; the next flip went forward again. Camera, HUD and pitch
meter are all in that buffer, which is why the whole frame stepped back. The
real SDK and RPCS3 both return the buffer id.

- **Fix:** return `bufferId` from `cellGcmSetPrepareFlip`, live key
  `PS3RECOMP_PREPARE_FLIP_RET` (default off; now exported as 1 by both
  `play_windowed.sh` copies). It still does not queue a FIFO flip: this title
  flips through `SetFlipImmediate`, and queueing here would flip twice.
- **Instrumentation:** `PS3RECOMP_PRESENT_TRACE=1` logs the flip index,
  requested buffer id/offset, source surface and its last colour-write serial,
  and flags a present whose source is older than the previous present's
  (`back=1`); `PS3RECOMP_PRESENT_DUMP_DIR` writes 160x90 PGMs of the frames
  actually displayed (after the `PRESENT_NEW_ONLY` gate).
- **Live A/B at the meter (one launch, N=6, 2 s drop):** stale-source presents
  **4.71 % -> 0.038 %**; source buffers 0/1 went from 2403/414 to 1370/1292
  (the flips now alternate). Guest fps A **30.34 +/- 0.29**, B **29.34 +/-
  2.38** (one 24.77 fps resync segment; other five mean 30.25), paired
  **+1.00 fps**, 95 % CI **[-1.27, +3.28]**. Present intervals A p50 **35.24**
  / p99 **57.45 ms**, B p50 **34.91** / p99 **61.14 ms**.
- **Dumped images (moving frames, nearest-of-previous-8 match):** **219.9**
  backward frames per 1000 moving frames with the fix off (210 of 955; peaks
  90/min in the pregame cinematics, most exactly two frames back), **6.2** with
  it on (5 of 804, all at cuts). Example: frames 5300 and 5302 are identical
  with 5301 between them when off; monotonic when on.
- **Offline:** `test_rsx_draw_engine` and the other engine fixtures pass;
  `test_gcm_fifo_user_command` still fails to link on the pre-existing missing
  `rsx_draw_engine_set_*` stubs. `spu_context` ABI check passes.

Details, tables and images: [`docs/backward-frames.md`](backward-frames.md).

## 2026-11-03 (freeze): the 08:30 stall was a `spu_context` ABI split; VFS exonerated

The first post-08:30 boot with commit `decb3fb8` added `uint64_t src_seq` in
the **middle** of `spu_context` (`resident_code[]`). The lifted SPU modules are
compiled against the other copy of `spu_context.h` reached through
`recomp-work/runtime -> ps3recomp-native/runtime`, so every field after
`resident_code` (`dcache_id`, `host_depth`, `steps`, ...) moved in the runtime
but not in the lifted code. The c690 job dispatch then read stale state and
took the invalid-command `stop 0x1` at LS `0x1095C`; all five `spurs pm` lanes
ended at `pc=0x108E8`, `jobs=3 in-flight=5`, and the FIFO park/resync lines were
a downstream symptom. The 08:09 glibc crash was the already-fixed
`SPURS_PM_FAST_RESCHED` double unlock and is unrelated.

- **VFS was not the cause.** No `recomp-work/vfs` file changed between 07:40
  and 08:36; the 08:05 good run and the 08:30 bad run opened the same six
  CACHE files at the same relative times; both VFS WADs and SDATs are
  byte-identical to the ISO. The `.corrupt` files from 11-01 are a separate
  incident.
- **`build-good` "stalls too" is not evidence:** it was configured with a
  non-existent `SPU_DIR`, so it has zero lifted SPU sources and no jobs.
- **Fix:** keep the memo, move its state to the **end** of `spu_context`
  (`resident_code_src_seq[4]`); the shared prefix offsets match the lifted
  modules again. Offline ABI probe confirms every shared field matches (only
  `sizeof` differs, by 32 B). `run_code_dma_memo_test.sh` 15/15,
  `run_texdirty_range_test.sh` 16/16, the three SPURS fixtures PASS.
- **Guard:** `recomp-work/ab-tools/check_spu_context_abi.sh` compares the build
  header with the lifted header and refuses to start `play_windowed.sh` /
  `ab_live.sh` on a shared-field mismatch (`SPU_ABI_CHECK=0` overrides).
- **Verified in 3 launches** (cap 6): reverted `decb3fb8` works (968,913
  c690 jobs), pre-fix HEAD stalls (`jobs=3`), fixed HEAD works (1,092,754 jobs,
  no resync/`[pm-end]`, pitch meter confirmed at 162 s).

Details and the offset table: [`docs/freeze-20261103.md`](freeze-20261103.md).

## 2026-11-03 (c690 code memo, O(1)): generation-tracked reloads; A/B launch wedged in baseline

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. The follow-up mission had
a **one-game-launch** cap. Unit tests run before it; the launch was used and
failed to reach the meter. Details and tables:
[`docs/c690-job-gaps.md`](c690-job-gaps.md).

- **`SPU_CODE_DMA_MEMO` now has an O(1) check.** The first GET after a load
  still compares the bytes, then registers the full source span with the
  TEX_DIRTY page write sequence (write-protecting the pages). Later GETs skip
  on one per-range generation compare, not a 67 KB `memcmp`; a source write
  (including a direct PPU store, which faults into the page writer) moves the
  generation and the next GET falls back to the exact compare. Untracked
  ranges (switch off, TEX_DIRTY off, 16-entry table full) always compare. The
  per-write range scan is skipped while no range is registered, so the
  baseline write path is unchanged with the switch off.
- **Offline tests, all passing.** `run_code_dma_memo_test.sh` (byte cases,
  generation establishment, source page `PROT_NONE` to prove the tracked path
  does not read it), a new `libs/video/tests/run_texdirty_range_test.sh`
  against the real `rsx_wcb.c` range tracker (tracked write, direct PPU store,
  unrelated ranges, enable/disable epochs), and the SPURS scheduler fixture,
  which now drives `spurs_pm_fast_consume` and every
  `spurs_pm_fast_can_rerun` early return and re-acquires the lock line after
  each call (the run-2 double-unlock class). `runtime/spu/tests/run_tests.sh`
  is 11/11 after weak c690 stubs were added to the shared fixture support.
- **No A/B result, no winner.** The one launch (`jobgap3`) wedged in the
  **baseline** arm (both switches off) during the pregame cinematic, before
  any segment: no flips after 08:32:34, five `spurs pm` lanes at `pc=0x108E8`,
  c690 jobs=3 with `in-flight=5`, then the pitch poll timed out. The switches
  were never entered, so this is not a measured B wedge; the cap allowed no
  retry. Both `play_windowed.sh` copies are unchanged; both switches remain
  default off.

## 2026-11-03 (c690 job turnover): the gap is tail + host dispatch + head, not one lock

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. One mapping launch
(timeline + 25 s `perf` + `PS3RECOMP_C690_GAP`) and one A/B launch that was
aborted by a runtime crash in its second arm; the two-launch cap is used.
Full tables, the code changes and the crash: [`docs/c690-job-gaps.md`](c690-job-gaps.md).

- **The inter-job gap is three phases.** 628,783 same-lane gaps under 2 ms:
  job end -> policy-run end **28.6 us** p50, policy-run end -> next start
  **27.5 us**, start -> next job **26.2 us** (gap p50 **85.1 us**, mean 118.5).
  The host phase has the long tail (mean 52.5, p99 471 us), so at the mean it
  is the largest single term.
- **The PM runs once per job.** ~136k jobs and ~139k policy spans per lane;
  policy span p50 196 us vs job 136 us, inter-policy gap 27.5 us. The PM exits
  to the kernel after each job and the coordinator re-dispatches it.
- **The c690 module is re-DMA'd every job.** 86 s window: 7.74 M code GETs /
  **85.4 GiB** (70 KiB per gap = the 67,648 B module), and the overlay verifies
  67 KiB again once per job. This is the avoidable half of the head.
- **Atomics are not the bottleneck.** 2.56 GETLLAR and 2.55 PUTLLC per gap,
  7 % of PUTLLCs fail; the gap phases show no lock-dominated tail. The c690
  job body is 47 % of `spurs pm` perf samples; PM + runtime are small leaves.
- **Implemented default-off:** `SPURS_PM_FAST_RESCHED=<n>` (bounded re-run of
  a still-ready PM in the same worker, mirroring the coordinator's ready /
  signal / contention bookkeeping) and `SPU_CODE_DMA_MEMO=1` (skip a code GET
  already byte-identical in local store; exact, unit-tested).
- **A/B aborted, no winner.** B crashed after ~13 s with a double
  `spu_lockline_unlock()` in the fast-reschedule consume path
  (`pthread_mutex_lock` owner assertion). Fixed and rebuilt, but not re-run
  because the cap was reached. The invalid B stretch looked better (gap p50
  67.7 vs 79.8 us, lane busy 0.548 vs 0.512); that is a direction, not a
  result. Both `play_windowed.sh` copies are unchanged.

## 2026-11-03 (frame timeline 2): the long frame is the c690 burst, not the walker drain

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. One meter launch,
45 s `PS3RECOMP_TIMELINE` window armed 22 s after the meter. Details, tables
and the counterfactuals: [`docs/frame-timeline-2.md`](frame-timeline-2.md).

- **p50 did not move, the tail did.** 300 frames: p50 **34.23 ms** (old
  33.85), p90 **44.33** (51.32), p99 **49.14** (59.60), mean 32.43 (33.64).
  Full 45 s: 34.11 / 43.55 / 49.96. The >45 ms frames collapsed into 35-45.
- **The walker drain left the critical chain.** 26.37 -> **18.85 ms**; the
  saved time went into producer waits: kick-idle 3.17 -> 6.47 and
  `rsx.put_wait` 4.10 -> 7.12 ms. `drain + idle + put` is the whole frame.
- **A long frame is one c690 burst + ~6 ms.** Long >35 ms: L 41.6, c690 union
  36.0, burst wall 37.0, drain 25.8 (below c690), idle 10.0, put 5.9; frames
  with the whole burst inside: 42.1 = 36.1 + 6.0.
- **The five c690 lanes are 64 % occupied inside the burst.** Big bursts:
  wall 37.1, union 35.1, per-lane busy 20.0-24.9 ms, mean 3.19/5 lanes busy;
  ramp 0.86 ms, tail 0.39 ms. The idle is 139,664 inter-job gaps (p50 60 us,
  85 % < 0.1 ms), staggered across lanes (k=0 only 5 %): per-job
  dispatch/turnover, not a global scheduler tick or lane start skew.
- **Not two game frames per burst.** `gfx.frame_end` / `ppu.frame_end` /
  `flip_request` are 1.00 per frame, user handler 0.70/frame, and a 1 ms
  burst holds 0.77 flip ends (1.27 at 2 ms). 135 bursts touch one frame, 81
  touch two (pattern long 34.0 -> short 16.9: the tail spills over).
- **Halving c690 wall (per-frame counterfactual):** p50 **16.9-18.7**, p99
  **28.7-30.5**, mean 19.4-20.3. c690 x0.8 -> p50 27.0/p99 41.6; x0.69
  (perfect lane packing) -> 23.1 / 37.5.
- **Next levers:** (1) c690 lane packing / per-job dispatch (-11 ms p50,
  -12 p99); (2) native unpack/pack, skin and normalize kernels (-4-7 ms, and
  with (1) p50 19-22 / p99 32-36); (3) PPU main work only after c690 < 20 ms.
  Present fence rose 3.30 -> 7.18 ms but is off the guest chain. The walker
  is no longer a p50 lever.

## 2026-11-03 (pinning and idle retest): `lanes` no longer wedges, no layout winner; back-off neutral

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Full numbers and method:
[`docs/pinning-and-idle.md`](pinning-and-idle.md). Two launches (the retest
budget), both completed.

- **`PS3RECOMP_PIN` is live in `build-play`.** The earlier weak-reference bug
  is fixed by `ps3_thread_pin_link_anchor()` referenced from
  `templates/project/main.cpp`; the `[pin] PS3RECOMP_PIN=1 topology ...`
  banner appears in both runs. The registry and layout-name fixes were
  revalidated offline: 12 layout switches produced 12 `[pin] layout` lines,
  and launch 2 (no layout change) produced 0, not the old 107,588-line storm.
- **`lanes` retested with the fixed registry: no wedge, still a loss.**
  Three 10 s segments at the meter: **29.17 fps** vs **30.82** for `today`,
  and three presented-frame gaps > 100 ms (699.7 / 899.8 / 707.6 ms) with 20
  skipped flips; `today` and `os` never exceed 96 ms. Rejected.
- **`today` vs `os` is lane affinity only, and is inside run-to-run noise.**
  Both pin walker cpu 0 and ppu main cpu 1. Launch 1: 30.82 vs 30.53 fps,
  p99 50.96 vs 51.14 ms, std 11.12 vs 10.41, walker+wait 31.66 vs 32.58 ms;
  the lane-0 job rate is 2,371 vs 2,482 jobs/s (max/min 1.25 vs 1.23).
  `today` repeated at **31.33 fps** in launch 2, so the 0.95 % gap is noise.
  No controlled `PIN=0` vs `PIN=1` arm; `PIN` is still not enabled in
  `play_windowed.sh`.
- **Loading is not a spin problem.** Valid 30 s `top -H` samples: no thread
  reaches 90 %. With back-off off, `spurs pm` means 19-35 % (max 78.8),
  `rsx walker` 34.0 % (max 85.8), `ppu main` 23.7 % (max 87.8). The on half is
  later in the same load (total CPU 219 % -> 444 %), so its higher SPU numbers
  are the load stage, not the back-off.
- **SPU idle back-off at the meter is neutral.** Six segments per arm on
  `today`: 31.33 fps off vs 31.40 on, p50 34.18 vs 34.14, p99 50.45 vs
  49.04 ms, walker/wait within 0.13 ms, c690 rates within 1 %; one 540.8 ms
  one-off outlier in the on arm. Left default-off.
- **No `play_windowed.sh` change.** `today` is the in-code default when
  `PIN=1`; no layout or back-off beat the baseline outside noise.

## 2026-11-03 (native c690 Edge, step 2): corpus fix, 0xFDA0 kernel, live attempt

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Details, algorithm
description, exactness tables and the updated stage plan:
[`docs/edge-native-step2.md`](edge-native-step2.md).

- **Corpus fixed.** Stage snapshots are capped on outermost `0xFDA0` entries and
  written to a per-lane spool independent of job sampling; the job file stays
  open through the submit epilogue. The job-file run records **249,886 PUTs**
  (step 1: 0). Four spool runs collected **403 / 400 / 400 / 401** pairs; the
  last is a meter burst (frames 9116-9125; the meter stage is bursty so a
  >= 2 s spread of 300 meter pairs is not achievable).
- **Native kernel, bit-exact offline.** The 45-block `0xFDA0` state machine is
  flattened into one C function (locals for `sr0..sr93`, continuation calls as
  gotos, unchanged `spu_helpers`). Full 256 KiB LS + all 128 GPRs match on
  **1,604 / 1,604 spool pairs and 12 / 12 job-file pairs** with the shipped
  runtime TU. The in-game mismatches were harness/TU bugs: stale fallback
  compare, exit-MXCSR use, and the runtime TU missing `SPU_FAST_FLOAT`.
- **Speed: 1.30x, not 5x.** Lifted 136.7 us -> native 106.1 us median on 12
  pairs (1.28-1.35x). That is ~-1.0 ms per worker per frame at the 31.8 %
  tree/cull share, below the frame A/B noise. The step-1 kill criterion was
  5x.
- **Not landed live.** Two meter arms with `PS3RECOMP_C690_NATIVE_CULL=1`
  wedged the guest (`FIFO resync 0x0136E970`, `w=2136E970`, no flips ~10 s
  after the switch). The switch exists, default-off, and is not enabled in
  `play_windowed.sh`. Next: capture the live meter class with the fixed spool
  (10x cap) and shadow for 60 s, or move to unpack/pack (23.5 %, 5-10x target).

## 2026-11-03 (walker quick wins): remap cache, fetch memo, pipeline memo, `[unknown]`

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Offline replay A/B plus
two live meter launches; details, tables and the perf call-site map:
[`docs/walker-quick-wins.md`](walker-quick-wins.md).

- **Pipeline memo is the only live win.** `PS3RECOMP_PIPE_MEMO=1`, 85.0 % hit
  live, walker `pipe` **2.107 -> 1.633 ms** (-0.47) and -0.84 ms offline.
  Enabled in both `play_windowed.sh` copies.
- **The fetch plan memo is a live loss and explains the old `WALKER_FAST`
  regression.** Live hit rate **5.4 %** (reference sets change every frame):
  `fetch` 4.080 -> 6.493 ms (+2.41) in the bundled arm. The one-frame replay
  hits 497/502 and hides it. Left off.
- **Remap cache (`rsx_vertex_remap_build`, 20.3 % of the offline replay) is
  exact and -1.74 ms offline, but live-neutral:** 28.5-30.2 % hit, `fetch`
  +0.31 ms, fps -0.21 +/- 0.69. Left off.
- **Offline all three: 10.283 -> 8.077 ms/frame (-2.21).** Replay gates
  unchanged for every switch: meter `a6fcc849341eba9b` /
  `ff2a220c28750bc3`, pitch `416abee2c5bff47d` / `3e6f6ad4ebb23ed6`.
- **The 27.6 % `[unknown]` is not a JIT.** Walker samples by DSO: 80.4 % our
  symbolized binary, 11.3 % libc (memmove/syscalls), 8.3 %
  `libnvidia-glcore` (Vulkan driver). The driver time is per-draw recording
  from `vk_eng_draw_from`: `vkCmdBeginRenderPass` 14.5 % of driver samples,
  `vkCmdDispatch` (vertex unpack) 12.8 %, descriptor alloc/update ~7 %,
  `vkCmdEndRenderPass` 4.1 %, `vkCmdBindPipeline` 5.7 %, `vkQueueSubmit`
  2.3 %.

## 2026-11-03 (native c690 Edge, step 1): perf, corpus, 0xFDA0 reference

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. One successful meter
launch with a 20 s whole-process `perf record` plus an env-gated c690 corpus;
details, tables and the ranked plan: [`docs/edge-native-step1.md`](edge-native-step1.md).

- **Per-flip (20.008 s perf window, 640 intervals):** p50 **34.05 ms**,
  p90 43.58, p99 49.35, mean 31.19, std 11.20, 32.0 fps. Measured from
  `PS3RECOMP_FLIP_LOG`, not the every-10th `[flip]` decimation.
- **New c690 ranking (48,116 c690 samples):** `0xFDA0` 19.7 %, `0xD4A8` 8.4 %,
  `0x6D68` 6.4 %, `0xFA58` 5.9 %, `0xAF98` 5.7 %, `0x8160` 5.1 %,
  `0x7C18` 5.0 % (already native), `0xD2B8` 4.9 %, `0xFCB8` 3.2 %,
  `0xF520` 2.8 %. Tree/cull family **31.8 %** (~4.6 ms/worker/frame);
  unpack/pack 23.5 %; skin/transform 13.6 %; normalize 8.2 %.
- **Corpus:** 2,000 jobs / 227 frames / 19 opcode layouts; 12,138 DMA GETs
  (1,217 B/job); 4 `0xFDA0` stage pairs. 0 PUTs: the output DMA is in the
  submit epilogue after the job-end command. Record format and variant counts
  in the doc; capture data stays in `recomp-work/`.
- **Reference:** `ps3recomp-play/tools/c690_ref/` - a 1,799-line C
  transliteration of the 45-block `0xFDA0` state machine (runtime plumbing
  removed, exact `spu_helpers.h` arithmetic). **4/4 stage pairs bit-exact**
  (0 LS bytes, 0 GPRs differ). NHL's stage split/corpus method credited in
  `THIRD_PARTY_NOTICES.md`; no NHL code copied.
- **Plan:** tree/cull -> unpack/pack -> skin/transform -> normalize ->
  format/parser, then the remaining codegen tail. Hook at the outermost
  `0xFDA0` call, keep the job framework lifted, fall back on layout mismatch,
  exactness = corpus LS+GPR diff + in-game shadow + per-job hash. Expected
  c690 union 35 -> ~23 ms after the first three stages, ~19 ms after the tail;
  the 19.5 ms main-ref cycle is the floor.

## 2026-11-03 (texture dirty): revalidation by guest-page write generation

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. One meter A/B launch
plus one paranoid-check launch. Details, coverage table and limitations:
[`docs/texture-dirty.md`](texture-dirty.md).

- **Replaced the texture content hash** (`eng_texture_slot`'s sparse hash
  every frame plus a full hash every 8) with a per-4K-guest-page write
  sequence. A texture stores the sequence sampled before its upload;
  revalidation is a max over its pages. Writers bump the sequence at their
  choke point (SPU PUT/atomic, 2D transfers, WCB/engine readback, file and
  network reads, HLE memcpy/memset); the PPU's own lifted stores fault
  through `PROT_READ` pages into the existing WCB handler. WCB-armed pages
  and texture pages share one protection state.
- **Live meter A/B (6 x 10 s per arm):** walker drain **26.64 -> 23.27 ms**,
  `tex` **10.34 -> 5.89 ms** (-4.45), engine 25.16 -> 21.64, guest fps
  **29.21 -> 31.13**. Per flip p50 **33.80 -> 32.00 ms**, p99
  **44.57 -> 41.06**, stddev 4.12 -> 3.73.
- **Paranoid mode** (`PS3RECOMP_TEX_DIRTY_PARANOID=30`, full hash of every
  bound texture every 30 frames compared with the uploaded bytes):
  **44,656 checks, 0 stale** over the meter run.
- **Gates unchanged** with `PS3RECOMP_TEX_DIRTY` on and off: meter
  `a6fcc849341eba9b` / `ff2a220c28750bc3`, pitch `416abee2c5bff47d` /
  `3e6f6ad4ebb23ed6`. New unit test drives a tracked writer and a direct PPU
  store and asserts the next bind re-uploads.
- **Default:** engine off, `play_windowed.sh` now exports
  `PS3RECOMP_TEX_DIRTY=1`; the live key can flip it per run. Off is the old
  hash path unchanged.
- **Left on the table:** `tex` is still 5.9 ms, mostly the real decode/upload
  of textures that do change. Sub-page/rectangle dirty regions or persistent
  GPU textures per target are the next step; the readback cuts and Vulkan
  recording items in `walker-profile.md` are unaffected.

## 2026-11-03 (walker own work): texture hashing, Vulkan recording, vertex fetch

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. One meter launch
(`ab_live_metercap_20261103_002238`) captured one frame and produced the live
phases; the per-draw split is offline replay of that capture. Details, tables,
ranked plan and exactness gates: [`docs/walker-profile.md`](walker-profile.md).

- **Live meter, 552 draws/frame:** walker drain **27.64 ms**, engine
  **26.02 ms**, parse 1.62 ms. Per draw: 50.1 us drain / 47.2 us engine.
- **Engine split:** `tex` **11.33 ms** (43 %), `draw` **5.51** (21 %),
  `fetch` **4.28** (16 %), `other` 2.61, `pipe` 2.07, `bind` 0.23, layout
  ~0.40. Texture work is the cache (98 % of `sink_bind_textures` is
  `eng_texture_slot`: lookup + sparse/full content hash), not surface scans.
- **Vulkan recording** (`VK_DRAWPROF`, 526 draws/frame): vtxgpu 1.53 ms,
  rp_begin 0.89, copy 0.79, desc 0.68, barrier 0.42, state/bind_draw/rp_end
  0.21 each = 4.94 ms. The null replay's `draw` (1.4 ms) is CPU conversion
  only.
- **Readback** is a separate 6.2 ms/frame (4.60 copy + 1.57 wait,
  `readback-cost.md`) in the walker wall wait, outside the drain.
- **Capture frame:** 502 draws, 525 groups, 114,688 vertices, 4.0 MiB
  converted, 276 texture binds, 0 new pipelines after warm-up, 525 pipeline
  lookups/frame.
- **Offline replay now models a steady scene:** `rsx_draw_engine_replay_advance_frame()`
  ticks `g.frames` after each replay frame (the replay has no display buffer,
  so texture revalidation used to stay on frame 0). The last-frame hash
  `a6fcc849341eba9b` and the 10-frame gate `02a922c9b2620fa1` are unchanged.
- **New `PS3RECOMP_WALKER_PROF2=1`** (default off): 22 nested terms around the
  fetch plan, conversion, pipeline key/analysis/create, texture slot/upload,
  constants, index build, backend binds, surface tracking and draw recording.
  `REPLAY_NULL_HASH=0` disables the null-backend digest for timing runs.
- **Plan to <= 8 ms:** texture page-dirty revalidation (-6 to -9), readback
  cuts (-3 to -4.5), Vulkan recording (-2 to -3), fetch/pipeline memo
  (-2.2 to -3.2), cheaper parse/other (-1.5 to -2.5), GPU vertex conversion
  (-1 to -1.5), then a parse/record split (-3 to -5). Each item's exactness
  is the meter/pitch replay hash plus a per-item test.
- **Not run:** `perf` (absent; `perf_event_paranoid=2`) and a second live
  window. The live phases are from the post-capture A/B window (2 x 20 s).

## 2026-11-03 (c690 profile): 165 us/job, 14.4 ms per worker, and no repeat output

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Three meter launches
(one killed by the run harness, two usable); details, tables and the ranked
plan: [`docs/c690-profile.md`](c690-profile.md).

- **Job cost:** c690 job wall p50/p90/p99 = 121/248/980 us, mean 164.6 us,
  437 jobs/frame. Cumulative 72 ms/frame across five lanes = **14.4 ms
  per worker**; the "35-40 ms" figure is the frame/union span on long frames,
  not each lane.
- **Hot spots:** `0xFDA0` tree traversal/culling 19 %, `0xD4A8` skin 8.1 %,
  `0x6D68` SNORM unpack 7.1 %, `0xAF98` normalize 6.2 %, `0x7C18` pack 5.7 %
  (already native). Compute is 90.5 % of running time; dispatch 5.2 %, MFC/DMA
  1.9 %, coherency 1.4 %.
- **Five lanes are the game's SPURS config:** `MeshJobs0/1` use
  `maxContention=5` and priority lane 5 = 0. Overriding lane 6 wedged the
  guest in run 3; rejected.
- **Caching is dead:** 40.3 % of sampled jobs re-read identical input
  (25.9 % across frames), but only **0.07 %** produce identical output.
- **Ceiling probe:** +50 us/job (+4.4 ms/worker) cost 1.95 fps (CI 0.63-3.28)
  and +2.2 ms walker SPU wait. Frame time follows c690 with ~0.55 elasticity;
  below the 19.5 ms main cycle further c690 cuts stop helping p50.
- **Instrumentation kept (all default-off):** `PS3RECOMP_C690_HASH=<stride>`
  per-job input/output hashes, `C690_SPIN_US` live cost probe,
  `SPURS_LANE6` live lane-5 override.
- **Not run:** `perf` (absent; `perf_event_paranoid=2`, no system change) and
  the fresh psample window (watcher picked the `timeout` wrapper). Per-function
  shares come from the existing gdb profiles `prof_spu2/3.prof`.

## 2026-11-03 (frame timeline): the meter's waits are work, not late wake-ups

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. One windowed meter run
(`ab_live_timeline_20261102_233854`), X once, WCB=2, PIN=1, CUBE_RT=1,
`PRESENT_NEW_ONLY=1`, PACE=1, x11, the four cheap-win live switches on.
Analysis and instrumentation only; no optimisation. Details, inventory,
tables and PNGs: [`docs/frame-timeline.md`](frame-timeline.md).

- **New `PS3RECOMP_TIMELINE=<path>|1`**: the existing per-thread trace rings,
  written as a binary record stream (`PSTL`, 32-byte records, thread-name and
  event chunks) instead of Chrome JSON. Trigger file + `PS3RECOMP_TIMELINE_SECS`
  window, as `PS3RECOMP_TRACE` already had. New events: frame begin/end, the
  ref/flip lwcond waits with their awaited value, walker pass / FIFO put /
  idle wait, ref set/publish, SPU PUT, c690 job, WCB done, draw submit,
  present queue/submit/fence/vkQueuePresent/shown, GCM user handler, and a
  `wait.wake_lag` instant = wake minus the newest causal event. With the
  variable unset every hook is one load/branch and no state changes.
- **Overhead, same launch:** 45 s recorded vs the 30-80 s after the sink shut
  down. Guest flip-to-flip p50 +0.03 ms, p90 +0.29, p99 +0.71, mean
  **+0.32 ms/frame**. 6,815,181 events / 218 MB / 0 dropped / 41 threads.
- **Frame time, 300 consecutive meter frames:** p50 **33.85 ms**, p90 51.3,
  p99 59.6 (full 45 s: 33.90 / 51.0 / 58.6). Walker drain is 26.4 ms/frame
  (78 %); the walker idle wait is 3.17 ms and its wake lag **0.00 ms**. Main
  is 12.5 ms work + 20.5 ms ref wait (the same walker work from the PPU
  side), ref wake lag **0.18 ms/frame** (0.50 on the 37 frames where it is
  the critical chain).
- **Short/long is the c690/mesh hand-off.** 256/300 frames are walker-drain
  critical: 35.7 ms frames, 28.7 ms drain, 24.7 ms c690 lane time, 3.4 ms
  idle. 37/300 have an empty FIFO (11.4 ms drain, 0.4 ms idle, 2.3 ms c690):
  main's 19.5 ms ref cycle is the frame. Transitions are 35 each way;
  lag-1 autocorrelation 0.02. The present side is already even (shown
  intervals p50 37.6, <8 ms 0.15 %).
- **Ranked wake-up lags:** ref lwcond 0.18 ms/frame (event-driven buys 0 at
  p50/p99, best case -0.18 ms mean), walker idle 0.00 (already an auto-reset
  event), present fence 3.30 ms but that is GPU blit execution, DisplayFlip
  flip-done 0.40 ms off the guest chain, SPU channels 0. c690's end-to-first
  PUT is p50 3 µs and the walker's kick-to-run lag ~10 µs.
- **Loading 100 % cores:** static deduction, not sampled (the planned
  `top -H` helper produced no file). The only no-OS-wait loops are lifted SPU
  poll loops (`GETLLAR`/128-byte GET, no LLAR back-off); with PIN=1 the walker
  is cpu0 and main cpu1, so CPU 10/11 are two unpinned `spurs pm` workers.

## 2026-11-03 (cheap wins): X skips the pregame, four live cuts, and the AVX2/FAST_LS build

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Meter, X once, WCB=2,
PIN=1, CUBE_RT=1, `PRESENT_NEW_ONLY=1`, PACE=1, x11. Details, tables and PNGs:
[`docs/cheap-wins.md`](cheap-wins.md).

- **Nav: boot-to-meter 245 s -> 153-157 s** with `SKIP_CINE=1` (X only on a
  frame that matches no menu, is not black, and is not the pitch camera; the
  exact pitch-menu match still triggers the one meter X). The first version
  pressed X on the loose pitch-camera match and that X was consumed by the
  batter-view transition; the meter then needed a second X. The final flow
  waits for the exact menu, which is the safe trigger.
- **Live bundle (SPU_MFC_LOOKUP_CACHE, GCM_NOP_SKIP, GCM_REF_IMMEDIATE,
  PS3RECOMP_RSX_TEX_PIN): 28.54 -> 29.90 fps** (N=6, 10 s, 2 s dropped; paired
  -1.37 fps, 95 % CI -2.66..-0.07), walker wait 7.75 -> 6.78 ms. The run's A/A
  check is not at the A/A level (first A segment 26.4 fps warm-up); excluding
  it the gain is +0.94 fps. `SPU_MFC_LOOKUP_CACHE` alone was inside noise.
- **Build A/B (base vs FAST_LS+AVX2+VMX on): 22.35 vs 28.35 and 26.76 vs
  28.68 fps.** c690 job wall 65.5 -> 48.5 ms and 60.8 -> 47.7 ms; walker SPU
  wait 11.6 -> 7.2 and 10.9 -> 7.8 ms. `-mavx2`/`-mfma` were already implied
  by `-march=native`; `-ffp-contract=off` was not, and FAST_LS is the main
  driver. `-march=x86-64-v3` is a no-op under native.
- **Correctness:** AVX2 exactness 446.8 M lanes bit-identical, FAST_LS
  helpers 72/72, VMX op oracle + 200 k-vector accessor fuzz clean, replay
  hash `416abee2c5bff47d` (gate `67f03d06377de89b`) identical with the
  texture pin off/on. Meter frames unchanged.
- `play_windowed.sh` (both copies) enables the four live switches;
  `build-play` is reconfigured to the winning build options.

## 2026-11-03 (exposure + readback cuts): the meter's exposure readback is fresh and flat; the cuts do not change it

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Meter, X once, WCB=2,
PIN=1, CUBE_RT=1, `PRESENT_NEW_ONLY=1`, PACE=1, x11, two runs of 240 s each.
Details, tables and images: [`docs/exposure-and-readback-cuts.md`](exposure-and-readback-cuts.md).

- **The uncommitted RECT/BATCH/SKIP cuts are committed and gated** (live keys,
  default off; off path is the old path). Fixed one gating bug: `guest_dirty`
  was cleared in `guest_read` even with the cuts off. Added the missing
  walker-profiling/capture stubs to the two GCM fixtures; all 29 video fixtures
  pass. `exp/live-ab` pushed to `fork`.
- **Static:** `Lighting_InitStadium` (`0x4f0380`) creates `ToneMapSample`
  (32x32, 6 mip levels, RGBA8, `0x14008`), `QuickAmbientSky` (32x32, 1 level,
  `0x14008`) and `HarmonicsSample` (32x32, 1 level, `0x10008`).
  `Lighting_RenderExposureRT` (`0x4f48c8`) renders the temp camera and
  `ToneMapMipGen`; `Lighting_UpdateFrame` (`0x4f2198`) writes the per-material
  lighting block every frame.
- **Continuous, not once at start:** the new `PS3RECOMP_LIGHT_PROBE=1` (default
  off) logged the `ToneMapSample` 32x32/2x2/1x1 chain read 4,198 times per
  level in 255 s (16.5/s, ~every second frame) at the meter.
  `QuickAmbientSky`/`HarmonicsSample` were only read in boot/transition bursts.
- **No stale/partial reads:** every copy was at the surface's current serial
  (`fresh=1`, `covers=1`, no ticket mismatch). 1x1 exposure **78.7 +/- 0.3**
  (A, cuts off) vs **78.7 +/- 0.2** (B, RECT+BATCH+SKIP), drift **-0.1** vs
  **0.0** over 4 min; thumbnail luma **101.2** in both. The reported live-game
  lighting drift did not reproduce on the meter; no fix was made and the third
  run was not used. PNGs: `recomp-work/visuals/exposure/`.

## 2026-11-03 (frame pacing + cube fallback): the present pairs are two flips handed over back to back

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Meter, X once, WCB=2,
PIN=1, CUBE_RT=1, `PRESENT_NEW_ONLY=1`, x11. Details, tables and images:
[`docs/frame-pacing.md`](frame-pacing.md).

- **Per-flip log:** `PS3RECOMP_FLIP_LOG=1` prints `req/fin/pres/show` for every
  flip on one microsecond clock (`show_gap` is the display interval). For this
  title `req == fin` (statically linked libgcm, the drain decodes the
  `0xFEAD` word; no HLE markers), so the measurable split is
  `fin -> pres` (walker -> window handoff) and `pres -> show` (present thread).
- **Where the pairs are:** with `PRESENT_NEW_ONLY=1` and PACE off, 32 % of
  present intervals are < 8 ms and 60 % > 40 ms at a 34.5 ms mean. The
  per-flip log already shows two frames at `pres` 1-4 ms apart: the guest
  runs up to 3 flips ahead, the drain catches up and hands two frames over
  back to back. It is not re-presenting (`flips not shown: 0`) and not FIFO
  queueing (`pres -> show` p50 4 ms).
- **Fix:** `PS3RECOMP_VK_PACE=1` (live key, default off) holds each queued
  present in the present thread until its slot in an arrival-cadence
  schedule. Interval EWMA is measured on *new frame arrivals*, not on our own
  presents (the first attempt fed back and ratcheted B to 6 fps). The
  schedule is capped at one interval of wait and abandoned when presents are
  already queued; a late frame is presented at once. The blit is submitted
  when the frame ends, so only `vkQueuePresentKHR` is delayed.
- **A/B (one launch, 6 x 10 s per arm, 2 s dropped):** guest fps A 29.30
  +/- 0.86 vs B 29.07 +/- 0.76, paired +0.22 (95 % CI -0.35..+0.79), A/A
  PASS. Present intervals from the per-flip log: p50 **46.46 -> 38.43 ms**,
  p90 **58.30 -> 51.60**, std **24.28 -> 14.90**, < 8 ms **32.2 -> 0.0 %**,
  > 40 ms **61.8 -> 45.2 %**. MangoHud CSV agrees (p50 45.71 -> 39.04 ms,
  std 23.80 -> 15.96, < 8 ms 28.8 -> 3.2 %).
- **Cube probes:** an incomplete probe now binds the last complete cube image
  for that guest address (`cube_target_last` hook, address-keyed backend
  cache); the dummy stays only when no complete image ever existed. Unit
  test covers a face recreated out of format and a never-complete probe.
  Pregame in the same run: 20 addresses (368 frames) were logged incomplete
  after their first complete frame and are now served the last image; the
  1,435 remaining `cube` census misses are never-complete probes. No 2 s
  thumbnail pair within a scene shows a brightness flip.
- **Defaults:** `ab_live.sh` exports `PS3RECOMP_VK_PRESENT_NEW_ONLY=1`;
  `play_windowed.sh` (both copies) sets `PS3RECOMP_VK_PACE=1`.

## 2026-11-03 (readback attribution): the meter's WCB bill is guest transfers and faults, not texture binds

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Meter, X once, WCB=2,
PIN=1, CUBE_RT=1, x11. Details, tables and images:
[`docs/readback-cost.md`](readback-cost.md).

- **Attribution added:** `PS3RECOMP_RSX_WCB_ATTR=1` (default off) counts every
  WCB=2 copy per surface, per trigger (`texbind`, `guest`, `other`) and per
  copy/wait ms, in 60-frame windows. The trigger is set around the
  `sink_bind_textures` depth/reinterpret calls; the module records the
  guest-thread wait per region.
- **Meter (N=2, 35 steady windows):** 1.32 copies/frame, 4.60 ms/frame copy,
  1.57 ms/frame guest wait (~6.2 ms, the WCB=2-vs-WCB=0 gap). Trigger split
  **0.00 texbind / 0.66 guest / 0.66 other**. The `other` third is a 512x512
  RGBA target copied whole once per ~3 frames (3.13 ms/frame) through the
  guest's NV3089 blit (`cellGcmSys.c:1168`); the `guest` third is a
  32x32+2x2+1x1 mip chain read by PPU/SPU faults (1.47 ms copy + 1.57 ms wait
  for 4 KB, i.e. submission stalls).
- **The bind path exists only off the meter.** Menus bind a 2560x720 Z24S8
  depth target as A8R8G8B8 and the engine stores 2.5 MB to guest memory
  (~3.3 ms) because Vulkan has no `depth_as_color` backend hook (Metal does,
  `rsx_metal_backend.m:3250`). At the meter that bind does not occur.
- **Consequence:** serving texture binds from the GPU target
  (`PS3RECOMP_RSX_RT_TEX_GPU`) cannot move the meter; there are no
  bind-triggered readbacks to serve. The A/B was stopped rather than run as a
  no-op. If wanted for menus, the concrete work is the Vulkan
  `depth_as_color` hook; the engine already prefers it.
- **Real levers:** resolve only the NV3089 rectangle instead of the whole
  512x512 target; submit the tiny mip resolves together instead of one
  `submit_and_wait` each; skip the copy when the target's colour serial has
  not moved since the last read.

## 2026-11-03 (cube render targets + frame depth): players lit, one render frame in flight by design

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Meter, X once, WCB=2,
PIN=1, x11. Details, tables and PNGs:
[`docs/cube-rt-and-frames-in-flight.md`](cube-rt-and-frames-in-flight.md).

- **Part A (static):** `FUN_000655cc` waits for the **previous** frame's ref
  (`DAT_01245058[DAT_011203d4 & 3]`, where `DAT_011203d4` is the last frame id
  set by `FUN_00065760`), and that token is the `SET_REFERENCE` (method 0x0050)
  appended at the **end** of frame N-1. The guest therefore allows exactly one
  render frame in flight. `Gfx_FrameEnd`'s `DAT_01120410 + 1` gate allows three
  *display* flips; it is a separate `submitted - completed` queue and is not the
  measured 19-21 ms block. Our ref publication is not late: the token is queued
  when the walker executes 0x0050 and published at drain end / by the poll
  publisher; a dropped token would time out at ~9 s, not 19 ms. No fix
  implemented; the lever is faster walker drain, not the fence.
- **Part B:** the 8x8 cube probes are six separate 8x8 colour targets at
  `base + face * 0x200` (`0x059F3000`, `0x059F3C00`), written by one fragment
  program (`0xbd8ae188ddb5b0b8`, pipes 48/88/260) in a burst, not every frame.
  `PS3RECOMP_RSX_CUBE_RT=1` (default off, live key) copies the six faces into
  one six-layer cube image on the GPU, cached by face handles + colour serials;
  incomplete cubes keep the dummy and count a new `cube` census reason.
- **Runs:** A/B on the meter (N=6): off **30.06 +/- 0.53 fps**, on
  **30.09 +/- 0.58**, paired **+0.03** (CI -0.19..+0.25). Census at the meter:
  off 25,800-29,796 misses/10 s, on **0** after the switch. With
  `VK_DUMMY_BLACK` unset, **0 magenta pixels** at the meter and no visible
  pink; players are brighter (batter's blue jersey, catcher's red, umpire's
  grey), the field/crowd gain environment shading.
- **Harness:** `ab_live.sh` gained `DUMMY_BLACK`, `IGNORE_MAGENTA`,
  `CAPTURE_PREFIX`, `FINAL_SHOT`; `nav3.py`/`poll_pitch.py`/`same.py` mask
  magenta pixels out of the luma diff, so the magenta arm navigates on the first
  attempt.
- **Tests:** `test_rsx_draw_engine` (six faces, rewrite serial, incomplete,
  stacked) and `test_rsx_tex_miss` (new reason) pass; both are in
  `tools/test_video_fixtures.py`.

## 2026-11-02 (dummy census): the dark players are cube surfaces read as 2-D; MangoHud now counts flips

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. WCB=2, PIN=1, x11,
`PS3RECOMP_WALKER_RESYNC_FIX=1`, new `PS3RECOMP_TEX_MISS_LOG=1`. Details,
run table and PNGs: [`docs/textures-dummy-census.md`](textures-dummy-census.md).

- **New census** (`libs/video/rsx_tex_miss.{c,h}` + unit
  `test_rsx_tex_miss`): every dummy fallback by reason (format, surface,
  address, size, other), guest address, format byte, size, pitch, mips, cube,
  fragment hash and pipeline; per-frame summary + cumulative top-10. The only
  dummy sites in `libs/video` are the vtable `vk_cb_bind_texture` and the
  engine descriptor (`vk_eng_sample_view` / `vk_eng_write_descriptors_to`).
- **The jerseys are not a format/address/size miss.** At the meter the engine
  resolved every texture: `format=0 address=0 size=0`; **201,000 misses in the
  1029-frame meter window, mean 195/frame, max 582**, all the render-target
  reason. They are 8x8, 1-mip, `cubemap=1` textures whose guest address is also
  a tracked colour target (e.g. `0x059f3000`); the engine binds the 2-D
  surface/view to a `TextureCube` unit and the backend reads the dummy. One
  fragment program accounts for ~217/frame. Fix is a six-face cube-surface
  path, not contained, design in the doc.
- **Team-select logo is not a dummy:** black and magenta team frames are
  identical (mean diff 0.0); `RESYNC_FIX=1` still does not bring the logo back
  (park-resync draw loss, not texture resolution).
- **Magenta arm blocked by the harness:** with `VK_DUMMY_BLACK` unset the nav
  references (recorded with black dummies) lose the pitch twice, so no magenta
  meter frame; the saved b-roll pitch frame is ~24% pure magenta (217,220 px),
  confirming the dummy surfaces are the crowd/player materials.
- **`PS3RECOMP_VK_PRESENT_NEW_ONLY=1` (task 5):** gated on the new
  `cellGcm_flip_frame_count()` (SetFlipCommand / FIFO marker, not
  SetPrepareFlip), so re-presents of the frame on screen are dropped. Start
  screen: **`[flip]` 42.66 fps, MangoHud 42.57 rows/s, shown 426/10 s**, 0
  drops, pump gap 6-7 ms. `play_windowed.sh` now sets it.

## 2026-11-02 (meter pacing): guest work, not a 30 fps cap; the tick is not the missing tick

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Pitch meter, X once,
10 s settle, 6 x 10 s per arm, PIN on, 45 s trace window armed at the first
A/B switch. Details and PNGs: [`docs/meter-pacing.md`](meter-pacing.md).

- **Static:** the title never calls `cellGcmSetFlipMode` (import present, no
  call site), does not import `cellGcmGetVBlankCount` /
  `cellGcmSetVBlankFrequency`, and its vblank handler (`0x69288`) only updates
  a counter and a deadline that nothing reads. `Gfx_FrameEnd` allows the main
  to run **3 flips ahead**; `Gfx_DisplayFlip` flips continuously. There is no
  two-vblank wait, flip interval, or per-scene pacing switch. The title,
  pitch-wait and meter use the same path.
- **WCB=0, both `EXP_NO_DRAW=1`:** 16 ms **31.32 +/- 1.06 fps** vs 8 ms
  **28.05 +/- 0.43**, paired **+3.27** (CI +2.30..+4.23). WCB=2: 16 ms
  **28.89 +/- 1.77** vs 8 ms **26.85 +/- 0.56**, paired **+2.04** (CI
  +0.47..+3.62). **Halving the tick is slower, not faster.**
- **Per-frame trace (WCB=0):** main running p50 **14.7 ms**, blocked **19.6 ms**
  in `FUN_000655cc`; of the 18.9 ms ref wait only **0.97 ms is walker drain**
  and **16.8 ms is the walker waiting for a producer** (954 `gcm.spu_put`
  instants in a sample 13.8 ms wait). With WCB=2 the same wait is **16.8 ms
  walker drain** and 4.3 ms producer. Removing draws moves the wait to the
  guest mesh producer; it does not shorten the frame.
- **Vblank ticks/flip:** WCB=0 16 ms **2.05** vs 8 ms **4.48**; WCB=2 2.32
  vs 4.68, while the wall frame stays 32-38 ms. The meter's "two ticks" is
  arithmetic, not tick quantization or a title setting.
- **Verdict:** (1) guest work - main's own ~15 ms (p50) plus the buffer-ref
  handoff set by the SPU mesh producer - not (2) a deliberate 30 fps target
  and not (3) our flip/tick path. Keep draw fast-path benchmarks on the
  pitch-wait/gameplay, where the guest asks for flips sooner.
- **Tooling:** new `gcm.flip_vb` trace instant (request, vblank), `vc=` on the
  `[flip]` line, and `tools/analyse_meter_pacing.py` (streams the full SPU
  trace, splits arms from the `.marks` file, reports per-frame main
  blocked/running and the walker split inside the ref wait).

## 2026-11-02 (meter baseline): the stable meter is guest-cadence bound; NO_DRAW does not help it

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. X once at the pitch,
fixed 10 s settle, 10 s segments, PIN on, x11, 2 s scene thumbnails + contact
sheets. Details and PNGs: [`docs/meter-scene-baseline.md`](meter-scene-baseline.md).

- **Harness:** `ab_live.sh`/`measure_fast.sh` now press X exactly once and
  verify the meter from the frame dump against `ref/pitch_meter*.png`
  (crop 370,95,395,120; WCB-on and WCB-off references); auto-settle replaced
  by a fixed 10 s wait measured from the post-X walker plateau. Each run
  saves `scene_<epoch-ms>.png` every 2 s and a `scene_sheet.png` with the
  measured A/B bands.
- **A/A, WCB=2, N=8:** A **29.34 +/- 0.64** vs B **29.93 +/- 0.89 fps**,
  paired **-0.59 +/- 1.09** (CI -1.67..+0.50), segment std 0.77/1.07. Cleaner
  than the old pitch A/A (~1.6 fps CI); point estimate 0.59 fps and CI covers
  0.
- **Ceiling, WCB=0, N=6:** baseline **33.55 fps** (walker 19.17, wait 10.76)
  vs `EXP_NO_DRAW=1` **30.92 fps** (walker **3.78**, wait **28.60**), paired
  **+2.63** (CI +1.78..+3.48). **NO_DRAW is slower here.** The meter asks for
  a flip about every 30-32 ms (two 16 ms ticks); removing 15.4 ms of draw work
  leaves the frame at two ticks. The old pitch-wait 41 fps NO_DRAW ceiling
  does not reproduce on the stable meter.
- **WALKER_FAST=1 is slower on the meter:** A **29.18** vs B **27.21 fps**,
  paired **1.98** (CI +1.01..+2.94); walker 27.36 -> 30.71 ms, fetch 4.07 ->
  6.57, tex 10.16 -> 11.39. Do not enable it as a cut.
- **Per-phase (WCB=2 baseline):** walker 26.73 ms; engine 25.21 (tex 9.80,
  draw 4.75, fetch 4.01, other 4.54, pipe 1.91, bind 0.21), parse 1.50;
  source split main 10.20 ms/988 packets, mesh 11.57 ms/1393 packets. WCB=0
  cuts tex to 4.29 and main to 4.25 (mesh 12.12), so the WCB readback path is
  ~6 ms in the normal config.
- **Draw fast-path order (ms/frame, headroom not fps):** mesh 11.57, tex
  9.80, main 10.20, draw 4.75, fetch 4.01, pipe 1.91, parse 1.50. On the meter
  the fps payoff is 0 until the guest cadence/tick changes; test
  `EXP_TICK_US=8000` with NO_DRAW on the meter next.
- **Launch check:** current `play_windowed.sh` (Xwayland default) launched
  `build-play` to the title at **42.3-42.6 fps**, window `shown=63-68`, 0
  no-slot drops, no address-0/frame_clock crash (74 s). The older native-Wayland
  launcher reproduced the known present freeze, not a crash.

## 2026-11-02 (tick + resync): the 16 ms tick caps only NO_DRAW; FAST_WAKE still wedges

Branch `exp/live-ab` (`ps3recomp-play`), `build-play`. Pitch meter, 6 x 10 s
per arm, PIN on. Details and PNGs: [`docs/tick-and-resync.md`](tick-and-resync.md).

- **A/B band labelling:** the report was right, the PNG was read as game fps.
  MangoHud frametime is the *host present* interval; the bands also included
  the 2 s the report drops (the previous arm's tail). Bands now shade the
  measured window, print the band's guest `[flip]` fps, the plot handles
  MangoHud's nanosecond `elapsed` column, and a live-ctl/arm self-check passes
  on all re-rendered runs. `ab_live.sh` defaults WCB=2, warns when arm B names
  a key arm A does not (the control file never reverts a key), and has
  `BOOT_ARM` so `EXP_NO_DRAW` is not live at boot.
- **Jump-park resync fix** (`PS3RECOMP_WALKER_RESYNC_FIX=1`, default off):
  instead of teleporting `get` to `put` and losing the draws, rewinds it to
  the first unread offset below `put` and drains the range through the normal
  walker. Offline replay gate unchanged (`0f91a96c79a7aadf`); a park-reuse
  fixture shows the skipped probe executed with the fix, dropped without.
- **FAST_WAKE still not viable:** with the fix the arm is **~9 fps** (walker
  55 ms/frame vs 17 baseline) and wedges on the 3rd B (guest 314.5 s). The fix
  fires once, walks `0xC03C..0xC190`, and flips do not resume; 4d skipped.
- **Tick ceiling, NO_DRAW (WCB off):** A 16 ms **40.89 fps** vs B 8 ms
  **44.25 fps**, but B is bimodal: 54.0/60.4/55.8 in three segments,
  28.7/30.8/35.9 in the others. **The 16 ms tick is a real cap** (B exceeds
  41.5 by 45 %) but 8 ms is not free (doubled vblank/flip callbacks).
- **Tick, real drawing (WCB=2):** A **33.83** vs B **33.71 fps**, paired
  **0.12 fps** (CI -5.96..+6.20), A/A-level. With draws the walker is the
  limiter, not the tick.
- **Game speed:** the tick scales `cellGcmGetVBlankCount`, the vblank handler
  argument and the flip rate, so flip/vblank-driven logic speeds up (`sys_timer`
  is unaffected). Today's 16.000 ms is 62.5 Hz, already 4.3 % fast vs 59.94.
  A proper fix needs a 59.94 Hz vblank tick, flip completion at the marker (or
  a render-ahead mode), and a drain/fence cadence off the tick; >60 fps with
  unchanged per-flip simulation needs per-title delta-time work.

## 2026-11-02 (live A/B + ceilings): walker draw work is the limit; FAST_WAKE wedges

Branch `exp/live-ab` (`ps3recomp-play`, off `play/multiscene`), `build-play`.
One windowed run per arm pair, control file `PS3RECOMP_CTL` toggled every
10 s, 6 segments/arm, WALKER_PROF + MangoHud. Details:
[`docs/live-ab-and-ceilings.md`](live-ab-and-ceilings.md).

- **Harness:** live toggles for `PS3RECOMP_WALKER_FAST`, `PS3RECOMP_WALKER_WAKE`,
  `EXP_NO_DRAW`, `EXP_FAST_WAKE`; opt-in `PS3RECOMP_PIN=1` (walker cpu0,
  ppu main cpu1, others 2-5,8-11) with the mapping printed; `ab_live.sh`
  auto-settles until the pitch walker mean is <=20 ms for 8 s.
- **The pitch is not stationary for 1-3 min:** asset streaming swings the
  walker 14-45 ms/frame. A 10 s wait caught 16-38 fps segments. A/A after
  settle: PIN off diff **-0.31 fps** (CI -5.56..+4.95, std 4.2/4.9); PIN on
  **-0.11 fps** (CI -1.73..+1.51, std 2.2/1.7). Pinning cuts the spread ~2.5x;
  the best CI resolves ~1.6 fps, not 0.5.
- **NO_DRAW ceiling:** baseline **30.26 fps** (walker 24.94 ms, wait 10.76) vs
  **41.22 fps** (walker **2.15 ms**, wait 22.11), paired **-10.96 fps**
  (CI -17.69..-4.23); every segment faster, NO_DRAW std 0.6 vs 6.5. **Walker
  draw work is the frame limiter; the PPU side then caps at ~41.5 fps.**
  Draw source is mixed: PPU main ring 3.8-34.6 ms/frame, SPU mesh rings
  4.4-11.1 ms/frame. A native fast path should cover both, mesh first.
- **FAST_WAKE:** wedged the pitch twice at the same 5th B segment (guest
  ~300.8 s): jump-park resync, frame commands dropped, flips stop. The busy
  poll re-reads an unpatched park; the 4 ms block was giving the PPU time.
  Start-screen A/B: no gain (46.5 vs 46.5 fps); the 16 ms tick is the pacer
  (draw frames wait 14-19 ms, idle frames 28-33 ms; walker own 0.1-0.6 ms).
- **Pipeline cache:** none in `rsx_vulkan_backend.c`; a whole run is only
  ~340-430 creates, **30-41 ms total** (p50 29-36 us, max 0.4-6.9 ms). Not the
  scene-change stutter.
- **Timing:** flip completion is gated by the 16 ms tick (`main.cpp:263`);
  >60 fps needs a tick/timing change, not a faster walker alone.
- **Wayland:** native-Wayland FIFO froze again (showed 0 for the run); the
  runs used `SDL_VIDEODRIVER=x11` (Xwayland), the documented fallback.
- Frametime PNGs: `recomp-work/ab_live_aa_pin0_settled_20261102_140417_ft.png`,
  `..._aa_pin1_settled_20261102_141235_ft.png`,
  `..._ceil_nodraw_20261102_142204_ft.png`,
  `..._start_fastwake_20261102_143955_ft.png` (plus the invalid early runs,
  listed in the doc).

## 2026-11-02 (visuals): WCB ondemand fixes crowd/faces/field; gamma TXD gap and team logo explained

Windowed pitch A/B on `play/multiscene`, `build-play`, FIFO present, MangoHud
CSV, 10 s `[flip]` windows, one run per arm plus two ondemand repeats.

- **Game fps:** WCB off **35.94**; ondemand (`PS3RECOMP_RSX_WCB=2`) **30.87 /
  31.88 / 30.60** (mean **31.12**); async (`=3`) **28.71**. WCB costs about
  4.8 fps, async is slower than ondemand here.
- **Ondemand counters:** ~2.0-2.5 demand copies/frame, 292-395 KB, 0.6-0.7
  waits/frame (avg 1.5-2.1 ms, max ~28 ms), 3.4-4.3 ms copy, 0 timeouts,
  ~50-55 armed. Async also had a single **642 ms** read wait.
- **Visuals:** the dark crowd, dark/uniformless players, neon-yellow field and
  the rainbow/oil-slick catcher mask in the WCB-off frame all read correctly
  with ondemand or async (coloured crowd, skin tones, blue/white uniforms,
  natural green field). `HeadFaceCacheFile` was backed up and deleted before
  the WCB arms and regenerated correctly. The `[RSX fp] opcode 25` warnings
  remain in the WCB frames but did not cause the chrome faces there.
- **Team-select rotating logo:** pixel-identical with WCB off/2/3, so not a
  readback issue. The FIFO walker's jump-park resync (`cellGcmSys.c:2701`)
  still drops draws in the skipped range (`:1651-1745`); the 2026-10-27 note
  that the logo appears with an effectively infinite park grace points at the
  same cause. Proposed: raise `GCM_PARK_GRACE_MS` as a test, or replay draws
  on a resync. Not implemented.
- **Gamma/sign-expand:** `TEX/TXP/TXB/TXL` already decode both; the logged
  opcode 25 is `TXD`, where gamma fails the whole program (`rsx_fp_decompiler.c:876`)
  and sign-expand reads unexpanded (`:1078`). Because WCB removed the observed
  artefacts, the `PS3RECOMP_FP_GAMMA=1` work was left for an approved change.
- **Crowd patch:** the recomp applies no equivalent of
  `mlb-show-rpcs3-fixes/patches/MLB16_crowd_fix.yml` (one branch at
  `0x009bd544`); crowd placement still needs it.
- Details and images: [`docs/visuals-wcb-gamma.md`](visuals-wcb-gamma.md),
  `recomp-work/visuals/compare.png`.

## 2026-11-02 (window freeze): native-Wayland MAILBOX present stalls; FIFO by default

The player's window froze for 15-50 s at a time while the game kept running;
moving the mouse unfroze it. The `PS3RECOMP_VK_PRESENT_STATS=1` lines prove it
is presentation: the pump ran normally (`max pump gap` 4-15 ms) while every
frame was dropped **no-slot** (`blits 0, shown 0, dropped ~65/s`). The whole
session was **219 frozen s / 457 s**, 15,059 no-slot drops, only 87 not-ready,
and the longest `vkQueuePresentKHR` was **49.2 s**.

- **Mechanism:** a 3-image swapchain exposes only 2 present slots
  (`rsx_vulkan_backend.c:1982`). A slot is busy until `vkQueuePresentKHR`
  returns. On native Wayland + NVIDIA MAILBOX the present thread can block
  waiting for the compositor's buffer release; after two stuck presents every
  later flip is dropped at `:2210-2211` *before* the acquire. `no-slot` is not
  an acquire timeout, so `PS3RECOMP_VK_ACQUIRE_TIMEOUT_MS` cannot help.
- **A/B (windowed, MangoHud, no input):** MAILBOX froze hard in 2 of 4
  play-style runs (138 s and 82 s frozen; 5-8k no-slot) and in the player's
  session. Wayland FIFO: 0 s and 1 s frozen, 1-2 no-slot over 147-149 s.
  Xwayland (which negotiated FIFO) 0 s / 4 no-slot. The measure_fast pitch
  harness did not reproduce the menu/attract stall (0 frozen in two runs);
  pitch fps was **35.44 flips/s** there.
- **Fix:** `tools/play_windowed.sh` defaults `PS3RECOMP_VK_PRESENT_MODE=fifo`
  (override with `mailbox`) and sets `VK_DUMMY_BLACK=1` (missing textures are
  black, not magenta). No ps3recomp code change was needed.
- **Tool bug:** `tools/frametime_plot.py` averaged per-row fps and reported
  333.83 fps for a log whose mean frametime was 35.25 ms; avg is now
  frames/total time (28.37 fps) and a self-check enforces it. MangoHud's fps
  counts window presents (72.8 rows/s at one pitch) while game fps is
  `[flip]` (35.44); documented in `docs/multiscene-play.md`.
- Details: [`docs/window-freeze-fix.md`](window-freeze-fix.md).

## 2026-11-02 (overlap): the ref is published 3 us after the walker reaches it; the wait is walker time-to-fence

One traced pitch run (`measure_fast.sh`, `VK_DRAW_BATCH=1 VK_FRAME_PIPELINE=1`): **35.01 fps** on the
10 s counter; trace window 12.05 s / 416 flips / 510 fences / 396 PPU ref waits. New trace-only
`gcm.ref_set` / `gcm.ref_pub` events (`ps3recomp-probe` `mlb16-work`, off by default, pushed to `fork`)
measure the publication path directly.

- **Publication is not late.** Walker reaches `NV406E_SET_REFERENCE` -> ref register written:
  **p50 3 µs, p90 4 µs, max 20 µs** over 510 fences. The write is a plain guest store at
  `cellGcmSys.c:3194` (end of the drain pass) or the read-driven poll (`:1909`); no Vulkan submit,
  GPU fence, flip or `VK_FRAME_PIPELINE` dependency. Labels/semaphores are written at dispatch.
- **The fence is the last word before the park**, so end-of-pass publication is ~0 ms after dispatch.
  "Publish at dispatch" is a no-op for this title.
- **The wait is the walker reaching the fence.** Of the mean **20.7 ms** wait, **19.7 ms** is before
  the walker reaches the target; set->pub is 0.003 ms and pub->PPU read is 0.97 ms. In 396/396
  episodes the target was not yet written when the wait began. The walker is in a drain pass 52.6 %
  of that pre-publication time (p50 72 %) and starved the rest.
- **Guest side:** one global monotonic ref counter (`DAT_01246044`), one main pushbuffer
  (`0x01246390`, `0x280000`, usable `0x27FC00`) and one 16 KB mesh/frame-tail ring (`0x01244E08`) in
  the 64 KB cellGcm FIFO; `FUN_000655cc` waits for `ctrl->ref == DAT_01245058[prev & 3]` on lwcond
  `0x01245090` (mutex `0x01245068`, 9000 us) as a **one-frame-in-flight** throttle.
- **Overlap is already present:** main builds frame N while the walker drains N-1; only the submission
  tail is serial. There is no extra append work to overlap; deeper runtime rings cannot skip the
  guest's depth-1 wait, and forcing it is the geometry-corruption path (the old `(id+1)&3`).
- Options: (A) publish at dispatch, **0-0.02 ms** (do not pursue); (B) cut walker time-to-fence,
  0-2.6 ms (existing `PS3RECOMP_WALKER_FAST`); (C) shorten the starved half, 0-2 ms; (D) deeper
  pipeline, guest change only, high risk; (E) env A/B `VK_FRAME_PIPELINE`/`VK_DRAW_BATCH` to size the
  GPU part of the drain. Details: [`docs/overlap-analysis.md`](overlap-analysis.md).

## 2026-11-02 (walker cuts in-game): 38.52 fps off vs 37.34 fps on, no gain resolved

Two `measure_fast.sh` runs on `feat/walker-cuts` (`55192504`, the usual
`build-o2` config), untraced, one 10 s pitch window each: `PS3RECOMP_WALKER_FAST`
unset **38.52 fps** (390 flips), `=1` **37.34 fps** (380 flips). Both reached the
same pitch wait, ran the full window, and logged no crash, `[replay-engine]`,
trace, `[walker-prof]` or new `pipeline ... FAILED` line. B is 1.18 fps (3.1 %)
below A, inside the within-run spread (1 s buckets stddev 1.6-1.7, range
35.9-40.8) and the same-build round-to-round spread (32.22 vs 34.63 earlier), so
one run per arm does not resolve whether the cuts help in-game; the offline
-2.6 ms engine cut did not show up as fps here. Frames are equivalent: **3.2**
grey levels over the full 640x360 image and **2.3** in the pitch box
(380,100)-(500,200) (`same.py`, as `walker-wake.md`), with the difference in the
animated crowd and player pose; no missing, black or constant-colour draws. Run
A (fast off) is 38.52, so the earlier 34.6 no-trace baseline vs 37.07 profiled
run is a build/session difference, not the `measure6` method and not the cuts.
Details: [`docs/walker-cuts.md`](walker-cuts.md).

## 2026-11-02 (walker cuts 1+3): fetch -1.63 ms, pipeline -1.09 ms per frame offline

Branch `feat/walker-cuts` (`ps3recomp-probe`, off `feat/walker-prof`, not merged).
`PS3RECOMP_WALKER_FAST=1` (default off) adds cut 1's fetch plan/remap memo and
cut 3's pipeline input memo. `replay_rsx_engine` now also reports a per-frame
digest of everything the null backend receives, so the replayed frame can be
compared with the toggle on and off.

Offline A/B on `recomp-work/walker_frame` (5 repeats per setting, 20 measured
frames each; medians and min-max over repeats):

| phase | off | on | delta |
|---|---:|---:|---:|
| fetch | 4.200 ms (4.174-4.240) | 2.572 ms (2.559-2.592) | **-38.8 %** |
| pipe  | 2.067 ms (2.058-2.076) | 0.981 ms (0.972-0.986) | **-52.6 %** |
| engine | 14.978 ms (14.870-15.087) | 12.377 ms (12.293-12.500) | **-17.4 %** |

Output gate identical in all ten runs (`0f91a96c79a7aadf`, 20-frame chain) and
`replay_rsx` still verifies at `0x85fc2ff5a1655c26`. The memo holds no guest
vertex bytes (payloads are re-fetched), keys the fetch plan on the reference set
and VTXFMT state, and keys the pipeline on program bytes plus the
pipeline-relevant registers, so changed index/program data misses. Unit tests
cover hits, reference-source and program-byte misses, and the pipeline key.
Also fixes a latent replay-only staleness: `state_restore` now bumps the texture
write generations it bypassed. Cut 2 (texture) is not implemented; Vulkan
recording is untouched. Details and the one-run in-game check:
[`docs/walker-cuts.md`](walker-cuts.md).

## 2026-11-02 (walker profile): fetch 3.6, texture 3.2, pipeline 1.9 ms per frame

Branch `feat/walker-prof` (`ps3recomp-probe`, off `mlb16-work`, not merged). New
`PS3RECOMP_WALKER_PROF=1` splits the real `sink_end` path per frame, and
`PS3RECOMP_WALKER_CAPTURE=<prefix>` images one pitch frame (FIFO words +
dispatcher state + every guest page the draws read, 107 MB). `replay_rsx_engine`
replays that capture through the real draw engine with a null backend; the
existing dispatcher-state hash still verifies at `0x85fc2ff5a1655c26`.

One pitch window (`measure6` flow, both switches on, 537 reports / 15.9 s,
37.07 fps): walker **17.19 ms/frame** at 500 draws. Split: parse 1.21, fetch
**3.56** (7.1 µs/draw), texture bind+hash **3.23** (6.5 µs/draw), pipeline
**1.86** (3.7 µs/draw), bind 0.19, Vulkan draw 4.78 (9.6 µs/draw), other 1.82.
Profiler overhead is inside noise (offline A/B 14.06–14.09 vs 13.95–14.91 ms per
14 ms drain). Offline replay (668 draws, 0 read misses) is 14.0–14.9 ms and
matches fetch/pipeline/draw per draw, but its warm texture phase is 0.2 µs/draw
against 6.5 in-game because one static frame has no texture updates — it is not
an A/B for the texture cut.

Top cuts (not implemented): fetch/remap caching **1.4–2.1 ms**, texture
page-dirty generations **1.9–2.6 ms**, pipeline-key/program memo **0.6–0.9 ms**;
together 3.9–5.6 ms off the 17.2 ms drain (~37 → ~42–44 fps if the producer wait
does not absorb it). Details and the capture format: `docs/walker-prof.md`.

## 2026-11-02 (critical path): the frame is walker drain (17.3 ms) + walker producer wait (11.8 ms)

One traced pitch run (34.58 fps / 10 s; steady window 361 frames, 10.47 s, 34.47 fps; trace window 411 frames,
34.08 fps). New `ppu.usleep` trace span (short `sys_timer_usleep`, guest LR) plus a fix to close the `ppu.gcm_finish`
poll run at the next block (`ps3recomp-probe` `mlb16-work`, trace-only, off by default). Analysis:
`tools/analyse_ppu_path.py` (new) and `tools/analyse_walker_wait.py`.

- **Frame = walker drain + walker put_wait**: 17.25 + 11.77 = **29.02 ms mean** (p50 33.8; distribution bimodal and
  the 10 s counter is 28.9 ms). `ppu main` runs 9.3 ms (already overlapped) and blocks **19.7 ms** in one place:
  `FUN_000655cc` (0x000655cc) waits on the ref cond (`0x1245090`) for the reused buffer. During that wait the walker
  drains 11.6 ms and starves 8.1 ms.
- **Only block on main**: the ref wait. `Mesh_WaitPrevJobList`/`FUN_0006f998` (mesh job list), the flip throttle
  (`0x1245100`, 17 ms) and the post-kick ref spin are **not taken at the pitch** — main has zero usleep spans.
  DisplayFlip blocks 9.3 ms on ref + 19.6 ms on flip-done; it is a pacer, not the limiter.
- **Walker split** (same shape as the attribution run): `u` PPU/park **6.16** ms, `e2` resume **2.80**, c690 `a+d`
  **2.71**, queueing **0.10**. 412 c690 jobs/frame, 41 % lane busy. The PPU and walker wait on each other for ~8 ms
  per frame: main waits for the ref, the walker waits for the PPU's own append (`u_park` 4.1 + `u_caught` 2.0).
- **Levers**: (1) cut walker drain CPU ~5 ms → ~42 fps (low–medium risk); (2) deeper ring / pipeline the PPU append,
  hides up to ~8 ms → ~45–48 fps (high risk, the `(id+1)&3` reorder is not enough); (3) c690 earlier/offload,
  −2.7 ms → ~38 fps (medium/high). 1+2 is the ~50–60 fps path; lever 3 alone is not. Full thread map, addresses,
  overlap/ordering analysis and caveats: [`docs/ppu-critical-path.md`](ppu-critical-path.md).

## 2026-11-02 (wake): waking the walker on the slot PUT removes e2 but not the frame time

Branch `feat/walker-wake` (`ps3recomp-probe`, off `mlb16-work`). The present thread waits on its kick event with a
4 ms timeout (`templates/project/main.cpp:220`, `cellGcmSys.c:1922`); the SPU PUT that fills the parked slot did not
signal it, so PUT->resume was p50 527 µs / p90 2.15 ms. `gcm_note_spu_put` now signals that event when the PUT range
contains the parked `get` (opt-in `PS3RECOMP_WALKER_WAKE=1`, default off; unit test
`tools/test_gcm_walker_wake.py`).

- **e2 removed:** PUT->resume **p50 8 µs / p90 11 µs** in a 398-frame, 12.98 s pitch window. But total wait stayed
  **11.64 -> 11.28 ms/frame** and fps fell **33.29 -> 30.67** traced (**33.44 -> 30.95** on the 10 s counter; the
  no-trace run with the watcher was 32.22 vs the 34.63 baseline). The wait moved to "job not yet pushed"
  (1.82 -> 3.65 ms/frame) and passes/frame rose 6.2 -> 35.4: the walker is caught up with the producer, so waking
  per slot just batches less and costs more walker CPU. Off by default.
- **`u` writer identified:** the park is patched by the PPU, not an SPU DMA. `Gfx_FrameEnd` (guest 0x00066530)
  writes the JUMP into the mesh ring at **0x00066ae0**; the put register is written by `FUN_00065760` (guest
  0x00065760) at **0x00065af8**. `u_park` waits are p50 11.7 ms and end near a flip/kick: the walker is early, and
  the real fix is to pipeline the PPU producer (`Mesh_WaitPrevJobList`, 0x0006ed24) with it, not just wake on the
  write. Numbers, tables and the proposed `u` fix: [`docs/walker-wake.md`](walker-wake.md).

## 2026-11-02 (attribution): the walker's 10 ms wait is mostly PPU/cadence, not c690 throughput

One pitch run (`measure_fast.sh`, 33.44 fps / 10 s; trace window 393 frames, 11.80 s, 33.29 fps; instrumentation
costs −3.4 %). New `PS3RECOMP_TRACE` events (`c690.job`, `spurs.kick`, `gcm.spu_put`, `rsx.park`,
`spu.overlay_load`, `spurs.init`) plus a trigger window attribute every `rsx.put_wait` to the SPU PUT that
overwrites the parked slot and the c690 job that made it.

- 2,444 waits = **11.64 ms/frame**. Only **~2.8 ms/frame is c690 production** (job admitted 1.82, running 0.96).
  **~5.7 ms/frame has no SPU PUT at the park** (20.5 % caught up with `put`, 28.7 % park patched by a non-SPU
  writer), and **~3.0 ms/frame is the walker not running again after the PUT** (slot filled p50 68 µs after the
  wait starts; resume p50 527 µs / p90 2.15 ms later).
- Workers are not saturated: c690 **42.1 %** of lane time over 6 configured lanes, 58.8 % busy, lane 5 idle
  (0 c690). Queueing with an idle lane 0.8 % of wait, all-busy 0.1 %. c690 wall ~75–77 ms/frame cumulative
  (matches 73.5 ms at 34.6 fps).
- **Implication:** the path to 60 is scheduling/overlap — wake the walker on the slot PUT, pipeline the PPU
  producer with the walker, and shave walker CPU (~37–39 fps for wake-on-PUT; ~44–46 for producer overlap;
  both + the no-trace 14–15 ms walker → ~60). Design B alone measures ~38 fps, not 55–60, because it removes
  only the c690 part. Full attribution, levers, caveats: [`docs/walker-spu-wait.md`](walker-spu-wait.md),
  analysis `tools/analyse_walker_wait.py`.

## 2026-11-01 (measure-and-decide): c690 offload verdict — no-go for A; B is the only 60 path

One pitch run (34.63 fps, 10 s window, `PS3RECOMP_C690_STATS=1 VK_DRAWPROF=1`). c690: **429 jobs/frame**, job
wall **73.5 ms/frame cumulative** (p50 115 µs, p90 239 µs), opcode `0x0A` 30.6 % / `0x2A` 7.9 % (every job) /
culls 5.0 %. Fence round trip **41 µs p50 / 74 µs p99** (locked clocks). Design A per-job win 44.5 µs mean but
−16.5 µs at p50/p99 → **~37–40 fps; no-go**. Design B (whole job on GPU, indirect draws, no CPU wait) → **~55–60
fps but 2–4 months**. Next lever: tree/leaf lifter codegen (~34 % of the SPU wall). Full numbers + leaf
cross-check: [`docs/c690-offload-decision.md`](c690-offload-decision.md).

## 2026-11-01 (correction): 0x2A is an assembler; 0x6D68 is a direction unpack

- `0x10F78` (opcode 0x2A, selector 0) is a **vertex-format assembler**: it gathers four already-final planar
  arrays (UV0 `0x21C30`, COLOR0 `0x20FB0`, POSITION `0x20330`, NORMAL `0x1F6B0`) into 36-byte records at `0x2F870`,
  with one float→u8 color pack (`0x7D40`, `cfltu 149`). It does **not** unpack/transform/normalize.
- `0x6D68` is a **12/12/11 SNORM direction/tangent unpack** (4th field 0), not position. The real per-vertex chain
  is: unpack `0x6D68` → 3x4 transform `0xA798` (matrix LS `0x29540`) → normalize `0xAF98` (frsqest+fi) →
  snorm pack `0x7C18`/`0x8160` → planar arrays → `0x2A` assemble.
- The captures are from **different job instances**, so they cannot be concatenated byte-exactly; a co-temporal
  capture (or whole-job replay) is needed to validate the full pipeline.

## 2026-11-01 (capture): pitch pipeline is multi-command; per-call offload not viable

- Captured the pitch frame's `0x10F78` (0x2A handler), `0x10B88`, `0xA798`, `0xAF98`, `0x6D68` in one run
  (multi-address `SPU_FN_CAPTURE`). `0x10F78` replays offline with zero DMA/mailbox.
- **The 0x2A handler is not the position path**: it is the stream-2 attribute gather/convert (`0x7840` family),
  writing `0x2F870`/`0x23400`. Position unpack is the separate leaf `0x6D68` (bit-exact, `0xb98ec2c0561b2bbf`).
- **Fence-latency implication**: `0x6D68` is ~0.5 µs/call; a per-call GPU dispatch+fence (~10–20 µs) is a loss.
  The offload must be **job-level** (all streams, one dispatch + one fence per job), which requires the full
  per-vertex pipeline in one shader.
- Report + constants: `docs/pitch_0x2A_pipeline.md`, `tools/replay/pitch_pipeline_constants.h`.

## 2026-11-01 (tooling decision): no Ghidra SPU import, no emulator FP

- Ghidra 12.1.2 has no SPU processor; GhidraSPU builds for it, but the MCP is bound to the EBOOT and cannot switch
  programs, and the c690 module is already lifted to readable, bit-exact C. **Do not import the SPU program.**
- anergistic (offline SPU emulator) has a flat EA model + MFC DMA but is **integer-only** (no `fma`/`cflts`/`frest`),
  so it cannot run the geometry math without ~15 added FP instructions. **Do not invest in emulator FP.**
- Use the lifted C + `tools/spu_disasm.py` for the thin low-LS runtime. Runtime per-format constants need
  function-entry captures, not decompilation.

## Next steps (2026-11-01)

Per-call offload is not viable (fence latency >> the ~0.5 us kernels). The offload must be **job-level**, and the
pipeline is a chain of separate commands. To build a job-level shader we need the full chain from one job:

1. **Co-temporal capture (recommended, one run).** Add a probe that captures the producing chain in a single job:
   `0x6D68` -> `0xA798` -> `0xAF98`, the `0x0A` pack scale/offset (`sr9`/`sr10`), and the `0x29540` matrix for the
   same job. Then build/validate the job-level shader offline against that one snapshot.
2. **Whole-job replay (offline, no run).** Lift the low-LS SPURS runtime (`0x1690/0x22B0/0x1B08/0x1AE0`) and add
   an EA/DMA model to the replay harness, then replay a job from the entry.

Then: build the job-level shader for all streams, validate offline, integrate at the DMA boundary (one dispatch +
one fence per job), and run the single go/no-go measurement. Function map: `docs/c690_function_map.md`.

## 2026-11-01 (decision): commit to the whole-pipeline GPU offload

Cheap levers are closed (per-function native 1.00×/1.03×; cull bypass loses). The only path to 60 fps is moving the
c690 **per-vertex math** (unpack `0x6D68`, skin `0xD2B8/0xD4A8/0xC98C`, normalize `0xAF98+B238`, pack `0x7C18/0x8160`)
to GPU compute, keeping culling/tree/index/ring on the SPU. First step is a 1–2 week go/no-go spike on one format
(bit-exact vs the offline harness, then measure the fence/sync cost). Full plan, data flow, risks, and milestones:
[`docs/gpu-offload-plan.md`](gpu-offload-plan.md).

## 2026-11-01 (late, 3): per-function native + cull bypass are both closed

- **Native kernels bit-exact but at parity:** `0x6D68` = **1.00×** (563 vs 568 ns/call); `0xAF98`+`0xB238` = **1.03×**
  (216 vs 223 ns/call). The lift already emits the same `vpshufb/vpsllvd/vfmadd132ps` mix. Per-function SIMD is not
  a performance lever.
- **Cull bypass loses in every variant:** all three = 33.6 fps; only `0x2C` = 34.85; baseline 36.6. The culls remove
  triangles before the per-vertex math, so they are a net win.
- **Conclusion:** cheap levers exhausted. 60 fps needs the whole-pipeline GPU offload (2–4 months) or accept ~36–40 fps.

## 2026-11-01 (late, 2): offline capture/replay harness working

`SPU_FN_CAPTURE=<lsa>[:dir]` (ps3recomp `a3a5564f`) dumps the full LS + 128 GPRs + pc at a function entry. First
capture/replay: `0x6D68` at the pitch → `tools/replay/` loads it, runs the lifted body, prints a deterministic
checksum (`0xfc2e9569dc2feb11`, loop 24) and **~563 ns/call**. This is the offline loop for native kernels — no
game boot or match load. Next: native `0x6D68` diffed against the checksum, then `0xAF98`+`0xB238` as one unit.

## 2026-11-01 (late): cull-bypass A/B (negative) + research triage

- **Cull bypass is a perf loss.** Porting the RPCS3 `MLB16_triangle_fix` idea (`bi $lr` on ICE handlers
  `0x2C`/`0x26`/`0x25`, LS `0x12DC0`/`0x13518`/`0x13520`) via an early `SPU_RET(ctx)` measured **33.57 fps vs 36.6**
  (diff 6.5). The cull is an early-out before the expensive per-vertex math; removing it costs more downstream
  than it saves. Keep it off; it stays a *visual* option for high-resolution pin-holes only.
- **Research doc:** its c690 command table / dispatch tables / patch offsets are corroborated by our own
  `mlb16-revival-server/docs/ice-mesh.md`. The valuable new prior art is **WKC2's capture/replay harness**
  (`WKC_JOB_CAPTURE` / `WKC_JOB_REPLAY`) and its serial-ring-commit rule. Its RPCS3 config flags are not
  applicable to a native recomp, and its "native SIMD → 6–7.5 ms" estimate is optimistic given our neutral
  `0x7C18` kernel and the cull result. RPCS3 calibration: 12700K ~55–60 fps minor-league / ~38–48 major.

## 2026-11-01 (night): Stage 0 c690 capture implemented — first real data flow

`SPU_JOB_LS_DUMP=<prefix>` (ps3recomp `f47deb52`, off by default) captures at the c690 entry (LS `0x135B8`):
the local store, the descriptor words, any guest EAs it references, and the job's first 400 DMAs. The job runs
under **image 6 (the SPURS policy)**, not image 204. Findings:

- The c690 code is DMA'd into LS `0x4000` from guest `0x105EE00` (variant `0x106D380`).
- The descriptor (`r3` = LS `0x1330`) is format/callback data; +0xC is a guest EA (`0x01243E00`) pointing at a
  **16×16-byte attribute-format table**.
- **Data flow:** inputs are per-mesh attribute streams from `0x0173xxxxx`–`0x0180xxxxx` (`0x180`–`0x500` blocks),
  params/matrices at `0x172F2FF0`, and a `0x2000` block from RSX memory `0x05800C000`; outputs are PUT back to
  the **same** `0x0173xxxxx` EAs (in-place transform), plus `0x011B00000` (`0x2400`), `0x146A800`, and
  `0x05800C000`.
- `tools/spu_profile.sh` is the repeatable gdb sampler (the `psample` attribution is unreliable).

Next: a larger/filtered DMA capture to separate palette/weight GETs from vertex-attribute GETs, and map the
attribute-format table. See `~/Documents/work_notes.md` "Session 7 Update".

## 2026-11-01 (evening, later): current cost table + the honest path to 60 fps

| Subsystem / Operation | Old figure | Current | Notes |
|---|---|---|---|
| Frame | ~31 ms | **~27.2–27.8 ms (36.0–36.8 fps)** | trace median 33.8 ms because flips burst/stall |
| RSX FIFO walker drain | ~31 ms | **24.65 ms** | single-threaded; includes ~7–10 ms waiting on the SPU |
| Walker *own* CPU | ~26 ms (~35 µs/draw) | **~14–15 ms** | ~35 µs/draw × ~440 draws; already under 16.6 ms |
| SPU lifted job | ~44 ms aggregate | **25.5 ms wall/worker × 5 (~127 ms CPU)** | 5 busy, 6th underused; spread over ~15 kernels |
| PPU `Gfx_FrameEnd` wait | ~30 ms | **18.2 ms** | halved by the `0xA8A0` lift |
| Present | ~9 ms sync | **offloaded to present thread** (windowed) | not on the benchmark critical path |
| GPU | — | **not the limiter** | headless replay 0.59 ms/frame |

120-sample gdb profile (140 c690 samples): `0xFDA0` tree/leaf 28, `0xD4A8` output/skin 15, `0x6D68` unpack 12,
`0x8160` pack 10, `0xAF98` normalize 10, `0x7C18` pack 8, `0xD2B8` skin 8, `0xFCB8` leaf 7. Buckets: **~50%
per-vertex math, ~34% tree/leaf, ~15% index/table/ring.**

**Why 60 fps is plausible but not a pile of 2 ms wins.** The `5M → 0.3M` win was *interpreted* SPU steps — a thin
unlifted-code layer, not the job (32.5 → 36.6 fps). The "30 ms → 0.5 ms" memory is the `replay_rsx --verify`
artifact, not a speedup. Micro-opts are single-digit %. The real path is one structural move: the walker's own CPU is
already ~14–15 ms (fits under 16.6 ms), and the SPU is ~50% per-vertex math (~12.7 ms) + ~34% tree/leaf (~8.7 ms) +
~15% index/ring (~3.8 ms). Move the per-vertex math to the GPU → SPU wall ~13 ms → the walker's ~10 ms SPU-wait
collapses → frame ~16–17 ms → **55–60 fps plausible**. If only some of the ~40 format variants are covered or sync
serializes, the walker caps it at **~40 fps**.

## 2026-11-01 (evening): reliable SPU profile + GPU-offload reality check

A gdb stack-sampling run (25 rounds, pitch screen, 36.8 fps binary) replaces the unreliable `psample`
per-thread attribution. The c690 job wall (~25.5 ms/frame across 5 workers) is **spread over many
format-dispatched kernels**, not concentrated in skinning:

- top `0xFDA0` (recursive tree/leaf) ~17% of job samples; then `0x7C18`, `0xFCB8`, `0x6D68`, `0xAF98`,
  `0x6860`, `0xE0A8`, `0xC98C`, `0x76B8`, `0x10700`.
- `0x31B0` is **not** c690: it is below the c690 LS base `0x4000`, inside the SPURS policy window.
- dispatch overhead is now tiny (`spu_lifted_lookup_full` 4, `spu_indirect_branch_slow` 3) after the `0xA8A0` fix.

Consequences for the plan:
- **Offloading only skinning (`0xD2B8`, ~10–20% of the job) saves ~2.5–5 ms → ~34–38 fps, not 60.**
  The whole per-vertex math pipeline (unpack/transform/normalize/pack, ~35–55%) is the necessary GPU step,
  and even then the walker (~24.65 ms) must be cut to <16.6 ms or it caps the frame at ~40 fps.
- `vtx_unpack.comp` is downstream of c690, so it cannot reduce c690's cost by itself.
- palette/weight addresses are runtime values; a first step is `SPU_LS_DUMP` + per-function timers, not code.

The walker remap cleanup (tightest table per draw) was behavior-preserving but **neutral** (36.80 vs 36.6).
See `~/Documents/work_notes.md` "Session 5 Update" for the full scoping.

## 2026-11-01 playtest (windowed, PS4 controller) — issues to fix

Run with `ab-tools/play.sh` on the 36.6 fps build. The game plays, but it is neither smooth nor visually correct yet.

**Rendering / visual**
- **Missing shadows, dark/black crowd, blank or chrome/pink players, missing jerseys.** The render-target readback
  (`PS3RECOMP_RSX_WCB`) was left off by default for benchmark speed; that is the path that writes rendered colour
  (and the crowd/face caches) back to guest memory. `play.sh` now sets `PS3RECOMP_RSX_WCB=1`; retest before
  concluding which of these it fixes.
- **Crowd LOD popping:** people appear only when the camera zooms in; segments of the crowd are missing.
- **Picture-in-picture view has a black background.**
- **Depth-of-field duplicates** (the RPCS3-fixes artifacts) are present.
- Several issues the `mlb-show-rpcs3-fixes` repo patches are present here and not yet ported.

**Presentation / window**
- **The window only updates when the mouse moves.** With the mouse still, the image runs for ~1 s then freezes while
  the game keeps running (controller rumble continues). The headless benchmark does not show this, so it is the
  windowed present/event path (Wayland/compositor frame callbacks or the SDL event pump), not the renderer. Try
  `SDL_VIDEODRIVER=x11` to test the XWayland path.
- Cutscenes and player entrances drop to low fps; load times are long.

**Input**
- **Controller presses are not always registered immediately**; menu items sometimes need several presses. Likely
  `PAD_POLL_INTERVAL_MS` / pad report pacing, not the renderer.

## 2026-11-01 (later): Target 4 — a missing lift entry was costing ~5 M interpreted SPU steps/s

The c690 job module's `spu_lifted` in the working tree was **stale**: it predated the `0xa8a0` entry already present in
`tools/lift_spu.sh`. The game reaches LS `0xA8A0` through a function pointer inside the lifted `0xA818` body, so with
no registered entry it fell to the SPU interpreter. `SPU_INTERP_STATS` showed ~5 M interpreted steps/s, with
`img=0xCC ls=0x0A8A0` as the top entry (~58k calls/s).

Re-running the canonical `tools/lift_spu.sh` (same flags; `0xa8a0` is already in `LBP_JOBMOD_EXTRA_FUNCS`) splits
`0xA818` into `0xA818` + `0xA8A0`. Pitch-screen result:

| | before | after |
|---|---:|---:|
| pitch-wait fps | ~32.5 (32.17–33.15) | **36.13 / 36.59 / 36.67** |
| interpreted SPU steps | ~5 M/s | **~0.1–0.3 M/s** |
| pitch visual diff | 12–18 | 14.2 |

The AVX2 `0x7C18` kernel was re-applied to the new lift and measured neutral (36.59 vs 36.67), so it stays wired.
`replay_rsx` still verifies `0x85fc2ff5a1655c26`.

**Lesson:** after changing `LBP_JOBMOD_EXTRA_FUNCS`, re-run `tools/lift_spu.sh`; `spu_lifted` is generated and not in
git, and a stale copy silently reintroduces the interpreter path.

**`0x108EC` — tried, then reverted.** The next missing c690 pointer target, `0x108EC` (between lifted `0x10758` and
`0x1095C`), was added to the lift. It cut interpreted steps further, **~0.3 → ~0.1 M/s**, but pitch-wait fps did not
move (**36.06 vs 36.1–36.7**, within noise), and the entry still hit the interpreter (`img=0xCC ls=0x108EC`) through a
residency/SMC path even though the function is now lifted. Adding it to the *global* `LBP_JOBMOD_EXTRA_FUNCS` also
pulled two other modules' `code_end` into their data tails (`7ac07dde` 0→411 unsupported, `31097a84` 0→195), so it
would need a per-module extras mechanism. Reverted to the `0xA8A0`-only build. The remaining interpreter
(~0.1–0.3 M/s) is the policy module's self-modifying-code entry at `0xA68` plus these residency paths; it is no
longer a meaningful frame-time lever.

## 2026-11-01: frame re-measured; the walker + SPU pipeline are the wall; Target 3 fence-skip fails again

Current `mlb16-work` tree, pitch screen: **32.2 fps** (1 s buckets 32.1 ± 2.0); `replay_rsx` still verifies
`0x85fc2ff5a1655c26`. The AVX2 c690 `0x7C18` kernel is wired and active (the linked binary calls
`native_spu_func_00007C18`), but it is not the main cost.

- **`replay_rsx --verify` is a correctness gate, not a benchmark.** The 36 ms "frame" is the per-call hashing; without
  `--verify` the same 646-draw frame is **0.59 ms** (~1,700 fps). The in-game walker is ~45× slower per draw because it
  also fetches/converts guest vertices, checks textures and records Vulkan commands.
- **Timeline (`PS3RECOMP_TRACE`, median 33.9 ms frame):** walker drain ~27 ms; PPU main sits in the `Gfx_FrameEnd`
  fence spin ~34 ms; 5–6 SPURS workers run c690 concurrently. The walker also has ~10.5 ms/frame of gaps waiting for
  the SPU to publish buffers (`rsx.put_wait`).
- **Walker CPU ~40 µs/draw** (38.5–49, `VK_EB_STATS`) over ~438 draws/frame: ~29% Vulkan driver command recording,
  ~18% `rsx_vertex_remap_build`, ~22% texture change checks, ~6% `sink_end`.
- **SPU profile (1 kHz per-thread `perf_event_open`):** the c690 job's hot functions are `0xD540`, `0x10028`, `0x6D68`,
  `0xAF98`, `0x8160`; `0x7C18` (the vectorized one) is a small share, and the others are local-store-bound.
- **Target 3 re-attempt, done correctly:** `0x011203D4` was left untouched; at the `Gfx_FrameEnd` call site (lr
  `0x6579C`) the wait used `(id + 1) & 3` instead of `id & 3`, behind `MLB16_PIPELINE=1`. It still fails: **185 flips
  in 689 s** and never reaches the pitch screen, versus ~9,300 flips for the same build without it. The fence also
  guards guest buffers the walker is still reading, so the PPU cannot run ahead of the drain. Reverted; both PPU
  recomp files are byte-identical to `.orig`.
- **Committed** the three infrastructure tweaks and the AVX2 kernel on `mlb16-work` (`ba20312c`). The
  `recomp-work/spu_lifted` wiring is still outside git; a copy is at `spu_recomp.c.avx2_wired`.
- **Walker split (`VK_DRAWPROF=1`):** of ~40 µs/draw, only ~10 µs is Vulkan recording (vertex dispatch 3.1, render
  pass begin 1.9, descriptors 1.4, copy 1.3, barriers 0.8, state 0.4, binds+draw 0.6, pass end 0.6); the rest is
  texture checks (~9 µs), vertex remap (~7 µs) and bookkeeping.
- **Walker experiments:** backing off the full texture re-hash for stable textures = 32.5 fps (no change, reverted);
  `PS3RECOMP_ENG_VTX_REMAP=0` = 28.4 fps (worse, dedup stays on); three frame-pipeline sets instead of two = 32.96 fps
  (1 s-bucket 34.1), a small within-noise gain, kept opt-in (`060bd105`).
- **Conclusion:** walker CPU is not the marginal limiter; the frame loses ~10.5 ms/frame waiting for the SPU to
  publish command buffers. The c690 SPU job is the next lever.

Next: cut the c690 job's time (the remaining hot functions `0xD540`, `0x10028`, `0x6D68`, `0xAF98`, `0x8160`) or
offload skinning to the GPU. The walker is close to its practical limit.


## 2026-11-01: Descriptor-Driven GPU Vertex Compute Unpack & Presentation Thread Complete

- **Descriptor-Driven GPU Vertex Offload (Macro Target 2 Completed, `feat/gpu-vertex-offload` commit `1f6dc0cd`):**
  - Integrated `vtx_unpack.comp` compute shader directly into `rsx_vulkan_backend.c` and `rsx_draw_engine.c`.
  - Upgraded from rigid layouts to a **Descriptor-Driven Compute Unpack** architecture:
    - *Modes 0, 1, 2 (Fast Paths)*: Zero-overhead unrolled unpack paths for 24-byte 3D meshes and 16/20-byte 2D UI quads.
    - *Mode 3 (Generic Descriptor Unpack)*: Receives up to 8 packed `AttrDesc` structs in push constants. Uses funnel shifts (`load_unaligned_be32`, `load_unaligned_be16`, `load_u8`) to natively unpack 2-byte aligned 22-byte streams (e.g., Short3 Pos + CMP Normal + CMP Tangent + Short2 UV0 + Short2 UV1) without alignment faults. Supports all 7 RSX attribute formats (Float, Half, UNorm8, UInt8, SNorm16, SInt16, CMP32).
    - *Dynamic Index Remapping*: Normalized non-contiguous vertex references (`elem = ((vertex_id + base_index) & 0xFFFFF) - stable_first`), streaming index remap buffers to compute threads to fetch and unpack sequential hardware vertex buffers.
  - Live Game Benchmark Results (`measure6.sh`):
    - Evaluated 2,278,559 draw calls.
    - **Total draws offloaded to GPU surged to 98.28%** (2,239,363 draws vs 320,595 previously).
    - **CPU software fallbacks collapsed from 85.0% to 1.72%** (39,196 draws).
    - Pitch-wait FPS rose to **24.19 fps** (1-sec mean 24.39 fps, instantaneous mean 24.43 fps ±2.62 fps), with instantaneous peaks reaching **32.15 fps** (vs 22.64 fps prior run, +1.55 fps / +6.8% uplift).
  - Deterministic & Visual Verification:
    - `replay_rsx kauffman_pitch.bin --frames 100 --verify`: 100% deterministic (State hash `0x85fc2ff5a1655c26`).
    - `same.py` pitch-wait reference match: diff 13.4 ($\le 25.0$). Full-screen end frame match: diff 2.6 across full 1280x720 viewport (sub-pixel match, zero mesh tears, zero spikes).
  - Architectural Diagnostic & Bottleneck Analysis:
    - Headless `replay_rsx` executes the entire 646-draw Kauffman frame in **0.59 ms (~1,700 fps)** without per-call hashing, proving the RSX walker itself is no longer the bottleneck.
    - The live game remains paced at ~24 FPS because the walker is starved at self-JUMP "parks" waiting for the **SPU Edge Geometry pipeline (`jobmod_c690c2f7b09a`)** which spends ~35–40 ms decompressing and skinning vertices on the CPU, and the **PPU main thread serializing in `Gfx_FrameEnd`**.
    - Target 2 permanently eliminated the walker's 26 ms CPU vertex anchor (which made 60 FPS previously impossible). The remaining gap to 60 FPS is now isolated to Macro Target 3 (PPU/RSX overlap) and SPU skinning loop optimization.

- **`mlb16-next` verified and fast-forwarded to `mlb16-work`:**
  - `PS3RECOMP_ENG_VTX_DIRECT`: converted draw vertices straight into the mapped Vulkan vertex ring buffer, bypassing intermediate host scratch copies.
  - Faster constant/NOP walker dispatch for repetitive WWS Edge state headers.
  - Benchmarked at pitch screen: `cur` 23.80 fps vs `next` 24.16 fps (+0.36 fps uplift).
  - Visual verification: `bodies.sh` captured 4 high-detail camera angles. Python/PIL pixel analysis confirmed identical rendering (no missing geometry, spikes, or flickering). Fast-forwarded.
- **Clang PGO automated end-to-end (`pgo_build.sh`):**
  - Profile counter pollution resolved: `pgo_profile_list.txt` filtered strictly to `src:*ps3recomp-probe/*.c` and `*.cpp`, eliminating counters from lifted SPU/PPU translation units (`0` in `spu_workloads.c.o`).
  - Clean training run: match load to pitch screen reached in 276.1 s (+15.5% overhead vs uninstrumented, zero hangs).
  - 9-run alternating A/B (load < 1.5, GPU clocks locked): GCC baseline 23.37 fps, Clang plain 23.66 fps, Clang PGO **24.73 fps (+5.82% / +1.36 fps, $\sigma = \pm 0.09\text{ fps}$)**.
  - Automated entire pipeline into `recomp-work/pgo_build.sh`.
- **Deterministic RSX FIFO Replay Tool (`tools/replay_rsx.cpp`, `tools/replay_rsx_core.c/.h`):**
  - Standalone C++17 CLI tool that accepts dumped RSX command streams (`.bin`) and replays them through the walker and register-file draw engine headlessly.
  - Reports p50, p95, min, max, equivalent FPS, draws/sec, and 64-bit deterministic state verification.
  - Synthetic test fixture `sample_fifo.bin` replays 50 frames at 1,720 equivalent fps with 0% measurement noise. Added CTest target `replay_rsx_sample_fifo`.
- **Vulkan Render-Target Readback (WCB) & Face Cache Purge:**
  - Implemented `rsx_draw_engine_flush_dirty_to_guest` in `libs/video/rsx_draw_engine.c`. Offscreen render targets (dynamic player faces, crowd atlases) marked dirty during rendering and blitted via `vkCmdCopyImageToBuffer` into guest memory before sync points (`NV406E_SET_REFERENCE`, `SEMAPHORE_RELEASE`, flips). Display scanout buffers protected.
  - Unit test `test_color_readback_guest.c` passing 13/13 checks.
  - Added `--purge-cache` and `PURGE_FACE_CACHE=1` to `tools/run.sh` to safely auto-delete corrupt, black `HeadFaceCacheFile` assets on startup.
- **Upstream Pull Request Campaign (11 PRs submitted to `sp00nznet/ps3recomp`):**
  - Converted local fork fixes into 11 clean-room PRs (#640–#650) addressing community issues #551 (MLB 15 match load), #464 (Puppeteer SPU taskset crash), and #345.
- **Real-Game FIFO Dump (`PS3RECOMP_DUMP_FIFO`) & Big-Endian Fix:**
  - Fixed big-endian dumper bug in `libs/video/cellGcmSys.c`: swapped words to big-endian (`__builtin_bswap32`) and padded unmapped payload dwords (`0u`).
  - Captured clean 100-frame trace at Kauffman Stadium pitch window (`kauffman_pitch.bin`, 40,466 words, 158.07 KB).
  - Verified 100% deterministic replay: 646 draws, 50 clears, 2,338 dispatcher calls per frame, state hash `0x85fc2ff5a1655c26`. Pure headless frame replay runs in 0.59 ms (~1,700 fps).
- **Decoupled Vulkan Presentation Thread (Macro Target 1, Completed):**
  - Moved `vkQueuePresentKHR` off the walker thread to a dedicated background worker thread in `libs/video/rsx_vulkan_backend.c`.
  - Resolved 12 FPS swapchain sync-coupling by requesting 2 physical queues (`queueCount = 2`): Queue 0 for walker submissions, Queue 1 for presentation (`s_vk.present_queue`). `vk_queue_present` calls `pvkQueuePresentKHR` directly on Queue 1 without acquiring `s_vk.queue_mu`.
  - Added `VK_PRESENT_MODE_MAILBOX_KHR` negotiation with fallback to FIFO and non-blocking slot acquire.
  - Exported `__GL_SYNC_TO_VBLANK=0` to eliminate driver sleeps.
  - Diagnosed performance behavior:
    - Synchronous WCB readbacks (`PS3RECOMP_RSX_WCB=1`) + GPU downclocking to P5 (810 MHz) had caused an ~11.2 FPS dip due to PCIe readback stalls (`g.be->submit_and_wait`).
    - With `PS3RECOMP_RSX_WCB=0`: Headless baseline instantly restored to **23.98 fps** (1-sec mean 24.84 fps, peak 34.25 fps).
    - Windowed live run (`PS3RECOMP_VK_WINDOW=1`): **23.34 fps** (instantaneous mean 23.52 fps ±2.17 fps), completely eliminating the 12.00 FPS clamp with only a 0.64 fps (2.6%) delta from headless.

---

## 2026-10-29: the match-load race found; a faster FIFO walker; clang PGO +5.8%

Measured by 3-run alternating A/Bs at the pitch screen. Today's absolute numbers are ~23 fps, not 26: the machine had
more running alongside (a remote-desktop session alone cost ~15-20%), so compare builds within a session only. The 55 s
"hold" number swings by ±3 fps between runs of one build, so decisions use the 15 s pitch-screen window.

- **The match-load freeze behind the PGO stalls:** the RSX drain gives up on a self-JUMP "park" after a grace period
  and follows `put` instead. MLB 16 parks in command buffers its SPU jobs are still writing, so when a slower runtime
  (an instrumented build, a busy host) took longer than the 50 ms grace, the drain skipped those commands and a GPU
  label with them, and the game waited forever. Confirmed by setting the grace to 5 ms on a normal build: same freeze.
  The default is now 1000 ms (God of War HD needed more than 50 ms too); zero resyncs and no fps change in A/B.
- **The instrumented builds' crawl** had a second cause: the profile list `src:*ps3recomp-probe*` also matched
  `spu_helpers.h`, so every SPU helper inlined into the lifted SPU code was counted (`spu_v2u` alone 50 billion
  times). Listing only `src:*ps3recomp-probe/*.c` and `src:*ps3recomp-probe/*.cpp` fixed it: training reaches the
  pitch screen ~10% slower than a normal build.
- **Clang PGO (runtime only):** 24.73 fps vs 23.37 for the gcc build and 23.66 for plain clang, best in all 3 runs
  (24.69, 24.84, 24.67).
- **FIFO walker:** with the draw engine on, every method word went through the per-word diagnostic loop. Runs of 3D
  methods now skip it, and plain register writes are stored without the full dispatcher: 19.7 → 11.1 ns per word in a
  replay benchmark with identical handler calls and register state. In the game it was within noise at the pitch
  screen; screenshots match.
- **Real `sys_lwcond`** (host condition variables, timeouts, a 10 ms fallback wake) behind `PS3_LWCOND_REAL=1`:
  +2.7% pitch, -7% hold over 4 runs, so neutral; left off.
- **A vertex cache across frames was measured and dropped:** validating the inputs costs about as much as converting
  them (≤1.2× best case in a benchmark). The cost that matters is converting and then copying ~55 MB a frame into the
  GPU ring; writing straight into the ring is next.

## 2026-10-28 (night): the frame traced; the walker is the bottleneck; +27% from pipelined GPU batches

All ps3recomp changes are on `mlb16-work` (MatRanc/ps3recomp).

- **Merged, measured at the pitch screen** (3 alternating runs each):
  - inFAMOUS 2's SPU DMA fast path, inline MFC channel ops and register-built insertion masks: +2.7%;
  - the lifter writes back only registers written since the last sync (register traffic in the hot job down 45%):
    within noise;
  - a ring filled exactly to its last word is no longer taken for an overrun (found by review);
  - `--const-bi` no longer turns resolver-only targets into gotos (found by review; MLB's lift is unchanged);
  - fixes from two reviews: memory containers reserve their size, SPURS slot checks and `maxContention` clamp, AIO
    write prefault and shared AIO ids, the GCM interrupt thread's stack, the template's SPU options, and the
    `run_tests.sh` and video fixture tests wired up. Within noise. One SPURS idle change was left out (-0.3 fps).
- **A frame tracer.** `PS3RECOMP_TRACE=<file.json>` writes a Chrome/Perfetto timeline: SPU job runs and waits,
  lock-line retries, the walker's drains and put waits, PPU lwmutex/lwcond/semaphore/event waits, GCM finish and
  flip waits. Tracing on costs nothing measurable at the pitch screen (20.6 fps either way).
- **What the trace showed.** The walker's drain is one ~36 ms pass over a few hundred bytes of the main command
  stream: it follows the command buffers the SPUs build. The SPUs out-run it, fill their rings and wait. Per thread
  the walker used ~55-68% of a core; the rest was GPU waits (`VK_EB_STATS=1`).
- **`VK_FRAME_PIPELINE=1`** (an existing ps3recomp mode) submits a full batch of draws without waiting for the GPU:
  20.8 -> 26.1 fps at the pitch screen, the pitch screen pixel-identical. Now on in `tools/run.sh`.
- **Native Edge kernels: dropped.** With the benchmark fixed (setup was inside the timed loop), an 8-wide AVX2
  version of job function 0x7C18 is bit-exact and 1.00x the lift. Only 2 of the job's 9 hot functions could be
  widened at all.

## 2026-10-28 (afternoon): a faster frame dump; striped lock-line locks measured; the hot job module identified

- **The frame dump was costing the walker ~20% of its time.** The test harness's PPM dump (every 10 frames) wrote 3
  bytes per `fwrite` call. It now packs the frame into one buffer and writes it once, to a temp file renamed into
  place. Pitch screen: 18.8 -> 20.3 fps (3 runs each). This only affects our measurements, not play, but the walker
  finishes last every frame, so the dump was inflating every number above (ps3recomp `c93af77`).
- **Striped lock-line locks (inFAMOUS 2, MIT): neutral.** Five SPUs contend on the one global lock-line lock in the
  job manager's atomic job grab, about 4% of SPU busy time. The 64-stripe version from inFAMOUS 2 measured 18.8 vs
  18.5 fps, within noise. It is kept on branch `port-lockline-stripes`, unmerged.
- **SPU time, split by sampling:** ~72% in lifted job code and ~28% in the runtime (lock waits, indirect-branch
  resolution, the drain loop, `mfc_for`). One module, `c690c2f7b09a`, is ~70% of the lifted part. A read-only
  analysis identifies it as a variant of Sony's Edge Geometry pipeline (vertex decompression, matrix-palette
  skinning, culling, quantization into the RSX mesh rings, and draw packets with fenced DMA).
  - A native replacement could at best halve the SPU phase. The walker would then set the pace.
  - The plan is one function at a time, each checked bit for bit against the lifted code offline, before
    committing to the whole module.

## 2026-10-28 (midday): the 5.9 s stalls fixed; SPU loops kept in registers; 17-18 fps at the pitch screen

**The 5.9 s stalls: our command-buffer `end` was one word too far.** The runtime set the context's `end` to the
first byte after the 64 KB buffer, IO 0x10000. The SDK sets it to the buffer's last word, the slot the wrap JUMP goes
in. MLB writes its own JUMP-to-begin at `end`, so it landed in whatever the game keeps right after the buffer. When
that word was overwritten, the walker walked on into data, read vertex-program microcode as a CALL, and skipped the
frame's fence. Gfx_FrameEnd then waited out its own 5.9 s timeout. `end` is now `begin + size - 4`
(ps3recomp `3695e97`).
- **Before:** 8 bad CALLs per run, and every one was followed by a ~6 s freeze.
- **After:** none in 3 runs, and no freezes during play. The few remaining multi-second gaps are scene loads: menus,
  the walk into the game, a new SPU workload being created.
- The pitch-screen benchmark is no longer bimodal: 15.2-17.3 fps across runs (before: 9 or 14-21).
- `GCM_RINGDBG=1` logs a walker that steps past the end of the ring.

**Where a frame goes now (ftrace at the pitch screen, ~58 ms frames).** The game publishes the FIFO `put` once per
frame, in Gfx_FrameEnd, and then waits for that frame's reference value. A frame is therefore:
1. the main thread runs about 12 ms, with the SPUs and the walker idle;
2. the SPU job lists (~44 ms on 4-5 SPUs) and the walker's drain (~46 ms) run together;
3. the walker finishes about 6 ms after the SPUs, which releases the main thread.

The game's own sync allows no more overlap than that, so the lever is speed: SPU job time first, then the walker.
- The interpreter is not the problem: 0.2-0.4 M interpreted SPU steps a second, a few ms of SPU time.
- One job module (`c690c2f7b09a`) holds most of the SPU compute.

**Two SPU code-quality fixes** (ps3recomp `a16e306`, `58587e9`):
- **`--ib-goto` (lifter).** The hottest loop (0x7C18) loops back through `binz $39, $50`, where `$50` holds its own
  entry address. The lifter treated that as leaving the function. Every iteration wrote back ~30 registers,
  tail-called itself and reloaded ~60. A computed branch whose target is a known entry inside the same function is
  now a guarded goto, and the loop keeps its registers. About +3%.
- **`shufb` without byte swaps (x86).** Registers keep each word byte-reversed, so the swaps fold into the index
  (`^ 3`), and a 16-entry lookup handles the special selectors. About 9 instructions instead of 18, and 9% fewer
  instructions in a hot function. About +4%.

**Result:** pitch-screen fps averages 17.8 over 3 runs (16.9 with the stall fix alone; 12-14 typical before it).

Next: the SPU job code is still about 10 x86 instructions per SPU instruction, and a third of that is register
spills. After that comes the walker (~40 us per draw).

## 2026-10-28 (morning): inFAMOUS 2's SPU and RSX work ported; a repeatable benchmark; a bimodal frame rate

Ported from the inFAMOUS 2 port (MIT, credited in each commit), all on branch `mlb16-work`:
- **SPU lifter:** `--local-regs` (registers in C locals) and `--const-bi`, plus our `--ib-known-targets`.
- **SPU helpers:** SIMD forms for the integer helpers that were still scalar on x86.
- **SPU runtime:**
  - a fast branch resolver for job modules (`SPU_IB_FAST=0` turns it off);
  - the channel-read store-forwarding fix;
  - cheaper DMA parameter writes.
- **RSX draw path:**
  - per-texture-unit write counters with cached decode (`PS3RECOMP_ENG_TEXWGEN=0`);
  - a texture-binding memo (`PS3RECOMP_ENG_TEXMEMO=0`);
  - vertex-shader constants by range (`PS3RECOMP_ENG_VPC_FULL=1`).
- **Frame pipelining (`VK_FRAME_PIPELINE=1`):** two batch sets; neither a present nor a full batch waits for the GPU.
- **CMP32 vertex fetch:** decodes from the stable source instead of resolving each element.
- **Interpreter stats** now name the job-code branch targets the lift lacks. Several were added to the jobmod lift
  (the entry list `tools/lift_spu.sh` passes).

**A repeatable benchmark at last (mostly).** Take the home team (the menu driver moves the controller right at team
select, then handles pitcher selection and the lineup), and the game waits at the first pitch selection with a fixed
camera: the full 3D scene, with the job lists still running. Fps is measured over 15 s from when that screen appears.

**Results:**
- None of the ports above shows a clear gain on its own.
- The walker's GPU waits fell from ~600 to ~100-170 ms per 2 s with pipelining, but they were off the critical path.
- All combined: 10.85, 18.24 and 12.99 fps, against 13.96, 8.85 and 8.72 for the previous build (and 13.7-14.9
  earlier). 18.24 is the best run yet.

**Both builds are bimodal.** Some runs settle around 9 fps and others around 14-18, which swamps every change
measured this morning. It is not thread placement: four more runs (11.4, 12.1, 12.2, 17.9 fps) had the same
spread of hot threads over cores and the same job lists on three or four SPUs at once.

**Found it: 5.9-second stalls from a FIFO parse slip.** Traced runs show the median frame is ~43 ms (~23 fps) in
slow and fast runs alike. The slow runs contain multi-second hitches:
- The walker reaches `0x709D302A` and treats it as a CALL to unmapped IO. It is the 12-word vertex-program
  microcode after a SET_TRANSFORM_PROGRAM header (`0x00300B80`) one word earlier.
- The walker got there misaligned: it decoded `0x01000014` as a 64-word method header. Before that, its
  control-flow log shows a park, a jump to 0xFFB8, and `0x00000009` at IO 0x15064 taken as a jump to IO 0x8.
- The same two words (CALL `0x709D302A`, JUMP `0x30464649`) appear 6-8 times in every run.
- After the bad branch the walker resyncs, so that frame's reference write is never seen. Gfx_FrameEnd waits for it
  in a loop that gives up after 0x15B3 = 5,555 `Cond_Wait(9 ms)` calls. With our 1 ms lwcond poll that is 5,555 ×
  ~1.06 ms ≈ 5.9 s.

Whether a run's measured window catches some of these is the bimodal frame rate. Fixing the parse slip (likely the
walker reading a ring slot before the SPU job has written it) is next. It is probably also the "hang-ups" seen
while watching.

**Where a frame goes now** (trace): five SPUs run the job lists for ~50 ms, then the RSX walker drains for ~35-40 ms
with the SPUs idle. The game waits at frame end for the RSX to finish the previous frame, so the two phases don't
overlap.

## 2026-10-28 (early morning): job lists on several SPUs, ~6 → ~12-13.6 fps

With batching on, the frame trace (SPURS workers now record busy spans) showed one SPU busy nonstop for ~180 ms of a
~208 ms frame while the others idled. The WWS job manager's two main workloads ran on one SPU at a time, by turns.

**The cause was a missing API, not the scheduler's speed.** MLB calls `cellSpursRequestIdleSpu(spurs, wid, 7)` for
every workload. The SPURS kernel lets an SPU take a workload while its contention is below ready count + that idle
count; WWS uses it to spread one job list over several SPUs, each grabbing the next job. Our runtime's version was a
stub that kept nothing, so with a ready count of 1 every list got exactly one SPU.

The inFAMOUS 2 port's notes and SPURS kernel had the answer (it is the same job manager). Now implemented on branch
`mlb16-work`: the job lists run on three or four SPUs at once.

| | fps in play (30-210 s after the first pitch) |
|---|---|
| Before (batching on) | 5.4-8.0 over six runs |
| With idle SPUs honoured | 11.93, 13.59 |

**Also learned:** vertex decode is not where the RSX walker's time goes. SIMD decode is 2× faster in isolation and
bit-exact, but the walker's CPU per draw didn't move (60.4 vs 60.5 µs), and the walker sleeps most of the frame.

**Next:** the inFAMOUS 2 port's other SPU work, ported where MLB needs it:
- the lifter keeping SPU registers in locals (`--local-regs`), `--merge-chunks` and `--const-bi`;
- SIMD forms of the integer SPU helpers;
- cheaper DMA fences.

## 2026-10-28 (overnight): the frame is a waiting chain; draw batching +20%

**Correction to the entry below.** The CPU-vs-CPU game plays out differently every run, so it wasn't the same
at-bat each time, and fps swings ±20% with what's on screen. Changes are now judged by alternating A/B runs: a
closed-loop menu driver that recognises each screen, fps over 30-210 s after the first pitch, one game at a time.

**Where the time goes**, now measured on the running game:
- Sampled with gdb, plus the runtime's `PS3_FTRACE` timeline (SPURS workers now record busy spans).
- **Nothing is CPU-bound.** The process uses about 18% of the CPU. One SPU worker is ~60% busy, the RSX walker about
  half, and the main thread ~10%.
- **The frame is a chain of waits:**
  - The main thread waits (Gfx_FrameEnd) for the RSX walker to finish the previous frame.
  - The walker spent ~45% of its time waiting for the GPU after each draw.
  - The SPU mesh jobs wait on the walker for ring space, which shows as bursts of ~8 ms on, 8 ms off.
- **RPCS3 on the same PC** holds about 43 fps in the same scene, spreading SPU work over six threads.
- The SPU compute per frame is similar between the two, so faster SPU code (a lifter flag `--ib-known-targets`
  that turns the hot loops' computed branches into direct ones) gave no measurable gain.

**Draw batching.** `VK_DRAW_BATCH=1`, ps3recomp's opt-in mode written for God of War: Ascension, records 100-200
draws per GPU submission instead of waiting after each one:

| | fps (runs) |
|---|---|
| Batching off | 5.41, 5.53 (4.50, 5.00, 6.24 before the GPU clock lock) |
| Batching on | 6.25, 7.96 (6.01, 6.17 before) |

It is now on in `tools/run.sh`.

**The GPU sat in its idle clock** (210-270 MHz on a 3060 Ti). The bursty submit-and-wait pattern never ramps it up,
and PowerMizer "prefer maximum performance" didn't stick. `nvidia-smi -lgc 1400,1980` (root; reset with `-rgc`) pins
it for testing.

**Smaller fixes** on branch `mlb16-work`:
- Vertex fetch skips duplicate-removal hashing for sequential draws and fills attributes that have no array.
- `VK_EB_STATS=1` prints why GPU batches end.

**Next:**
- Asynchronous GPU submission, so the walker records while the GPU executes. The first step publishes RSX labels only
  after their GPU work completes; today the game can see a label before the work behind it ran.
- Then SIMD vertex decode and a cache for static geometry; the walker re-decodes every vertex every draw.
- Then why the SPU rings wait on the walker.

## 2026-10-27 (end of day): SPU speed, 3.4 → 6.4 fps

Measured as the average flip rate over 60 s of play (same at-bat each time, `GCM_FLIPCOUNT=1`).

| Change | fps |
|---|---|
| Start (exact SPU float, scalar local-store access) | 3.4 |
| `PS3RECOMP_SPU_FAST_FLOAT=ON` | 4.6 |
| + `PS3RECOMP_SPU_FAST_LS=ON` | 6.4 |

Both are CMake options on branch `mlb16-work` that affect only the lifted SPU sources (configure with
`-DPS3RECOMP_SPU_FAST_FLOAT=ON -DPS3RECOMP_SPU_FAST_LS=ON`):

- **Fast float:** ps3recomp's x86 path emulates SPU single precision bit-exactly: each op in f64x4 with lane checks
  (~30 instructions), plus a scalar fix-up for tiny/out-of-range lanes that the vertex code hits often. The option
  uses native SSE/FMA3, keeps the SPU's denormal flush, and differs only in rounding (nearest vs truncate) and
  overflow (infinity vs saturate). It is within 1 ulp on 800k random lanes and ~6× faster per fma, like an
  emulator's default "approximate" SPU float mode.
- **Fast local store:** each 16-byte local-store access was four byte-swapped scalar moves plus a diagnostics-flag test
  and cold call. Now it's one vector load/store and a `pshufb`, with no diagnostics in lifted code. The hottest mesh
  block went from 827 to 478 x86 instructions.

Where the time goes now: one SPU worker at ~86% running the mesh jobs (the game runs that list on one SPU at a time,
as on hardware), a second at ~50%, the RSX walker thread at ~40% (CPU vertex conversion).

**Next lever, register caching (prototype in `tools/spu_regcache.py`):** the lifted code keeps SPU registers in
`ctx->gpr[]`, and every local-store store goes through a char pointer that may alias it, so the compiler reloads
registers after each store. The prototype rewrites each lifted function to use C locals (load at entry, write back
before exits/calls/channel ops, reload after calls).
- Applied to all 639 functions of the mesh job module, the job went wrong (an SPU return to LS `0x22`). The likely
  culprit is the interrupt path, which snapshots/restores `ctx->gpr` and can unwind host frames.
- Applied only to the 402 functions without calls or channel ops, the game plays correctly but no faster (6.0 fps).
  The hot loop (`0x7c18`) contains a call, so it wasn't covered.
- Next: make calls inside hot blocks safe for caching (or move the caching into the lifter itself, with interrupt
  points as explicit write-back sites), then measure again.

Also open: render-target writeback ("Write Color Buffers") for the dark players and crowd sprites, the RSX walker's CPU
vertex conversion, and the PPU side (same `ctx->gpr` pattern).

## 2026-10-27 (later): magenta fixed; where it stands against RPCS3

- **Magenta surfaces:** a RenderDoc capture of our own Vulkan output (layer mode,
  `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` with `SDL_VIDEODRIVER=x11`, since the layer has no Wayland support) showed the
  player shader sampling texture unit 5, which the game leaves disabled. Our backend binds a 1×1 magenta placeholder
  to such units, and the shader adds it as a tint. `VK_DUMMY_BLACK=1` (branch `mlb16-work`, set in `tools/run.sh`)
  makes the placeholder transparent black. Dirt, crowd, field and walls now look right.
- **Players and crowd sprites are dark/blank:** the same problem as RPCS3 without "Write Color Buffers". MLB 16 builds
  some textures by rendering them and reading the render target back from the CPU/SPU (the generated faces in
  `HeadFaceCacheFile`, the crowd sprite sheets). Our renderer doesn't write render targets back to guest memory, so
  those reads see black. Next to fix. Then delete `HeadFaceCacheFile`, which was built blank.
- **Speed: about 3.3 fps in a game** (flip counter, no RenderDoc). The two busiest threads are SPU workers running the
  mesh jobs (`jobmod_c690c2f7b09a`, skinning/geometry); the PPU main thread is at ~30% and the GPU is nearly idle.
  So the lifted SPU code is the bottleneck: one real SPU runs these jobs well within a frame, our translation is many
  times slower.
- **How this differs from an emulator, honestly:** the CPU side (PPU and SPU) is compiled ahead of time, but the GPU
  side is still an emulated RSX (command walker, translated shaders, texture formats), the same job RPCS3 does. And
  ps3recomp's RSX layer is much younger than RPCS3's, so graphics bugs RPCS3 fixed years ago show up here. The
  speed advantage can only appear once the SPU code generation is fast. Right now it's the opposite: RPCS3's SPU JIT
  (LLVM, with years of tuning) beats ps3recomp's straightforward SPU-to-C translation by a wide margin.

## 2026-10-27 (late night): in the game, pitches being thrown

**The match-load deadlock is fixed and the game plays:** the dugout, the pre-game intro, Kauffman Stadium, Volquez
pitching to Curtis Granderson, with the count and the score bug updating. Geometry is correct. Many surfaces
(uniforms, skin, infield dirt, part of the crowd) come out solid magenta, a renderer problem still to look at.

The cause was ps3recomp's SPURS scheduling, in three parts:

1. **Max contention dropped.** `_cellSpursWorkloadAttributeInitialize` takes `maxContention` as its ninth argument,
   on the guest stack, so the generic adapter never saw it and stored 1. The mesh lists ask for 5 and the DXT list
   for 4. Fixed with a context handler that reads the stack (branch `spurs-workload-maxcontention`).
2. **The ready count was consumed.** The scheduler decremented a workload's ready count each time it gave it an SPU.
   In the SDK the ready count is a level: the kernel runs up to `min(ready, maxContention)` SPUs and the workload
   lowers it itself. The WWS policy module does exactly that, setting it to 0 when its list is empty and back to 1
   when it sees more work. Consumed, a kick could be used up by a dispatch that found no jobs yet, and nothing
   dispatched the list again. `SPURS_READY_LEVEL=1` keeps the level.
3. **Idle probes ran workloads nobody had kicked.** To cover for (2), the scheduler periodically dispatched idle
   workloads. One such probe ran mesh jobs that the command-building threads had already queued, *before* the PPU
   reset the mesh rings and kicked the list. The reset then threw away the ring space those jobs had used, the next
   job was given the same space again, and it waited forever for the GPU to free it. `SPURS_NO_IDLE_PROBE=1`
   schedules only kicked workloads, as real SPURS does.

(2) and (3) are opt-in switches on branch `mlb16-work` for now, and `tools/run.sh` sets them. They change SPURS
scheduling for every title, so they need testing on other ps3recomp games before going upstream.

How it was found: a temporary override that forced the stuck wait through after 2 s got the game into the match
(with garbled geometry), which proved the wait was the only blocker. Then gdb watchpoints on the ring allocation
pointers and on the ready-count bytes showed the order of events.

## 2026-10-27 (night): how the mesh rings work, and where the deadlock really is

The stall at the pitching matchup isn't the game waiting for a pick (X on the matchup, START on the lineups):
flips stop and the main thread never leaves the job-list wait, so pad input is never read. Pressing X there changes
nothing.

How MLB 16 feeds geometry to the GPU (worked out from walker traces, gdb watchpoints and Ghidra):

- **Five mesh rings** of 2 MB each, holding **vertex/index data, not commands**: EA `0x11900000`, `0x11b00000`,
  `0x11d00000`, `0x11f00000` and `0x12100000` (IO `0x00900000`–`0x012fffff`), with RSX labels 72–76
  (`0x480`–`0x4c0`). Their state lives in the render object at `0x1246680`: `+0x200` holds
  `{base, end, alloc pointer, label address}` per ring (one 128-byte line).
- **Per-mesh slots in the command stream.** The PPU leaves a 0x90-byte slot per mesh job in its big command ring
  (IO `0x013xxxxx`–`0x014xxxxx`): a self-JUMP plus `0x40880100` (a non-incrementing NOP skipping 34 words). The job
  fills it by DMA when it finishes: vertex/index array offsets into its ring, the draw, and a release of its ring's
  label with the segment's end address. The GPU stops at the first unfilled slot, so it consumes ring data as jobs
  complete.
- **Frame setup** (`0x70694` → `0x6ed24` → `0x6f6b0`): wait for the command-building worker threads (`0x70894`),
  wait for the previous mesh job list (`0x9fb898`, list "MeshJobs0"), then set each ring's start for the new frame
  and kick the next list. Frame end is `0x66530` (links the frame into the libgcm ring, flips, paces on the
  frame counters).
- **Each ring has one SPU writer.** Watchpoints on the alloc pointers show ring 72 only written by one SPU worker,
  ring 75 by another, ring 76 by a third. Plus the PPU's frame setup. No lost updates.

The deadlock: the GPU parks at the first unfilled slot. That slot's job waits for ring space (`jobmod_c690c2f7b09a`,
`0xe4d0`–`0xe5c0`): it compares the ring's label (the GPU's read position) against the range it wants and retries
forever. In every run the waiting job wants a range the GPU hasn't released yet, often right after wrapping its ring.
Open question: why it wraps so early. In one run, ring 76's writer wrapped at `0x12137d00`, 0x37D00 bytes into a 2 MB
ring. The frame setup also once set ring 76's start to `0x12105180` rather than the base. Next: trace the per-frame
start/end values `0x6f6b0` hands out (`+0x18`, `+0x58`, and the single-ring branch at `+0x280`/`+0x290`) and how the
job derives its ring end. An RPCS3 memory dump of the same structure at the same screen would settle what "normal"
looks like.

Side findings:
- The walker's 50 ms park recovery is no longer needed for this game; running with it effectively off
  (`GCM_PARK_GRACE_MS=100000000`) still boots, and parks are always patched within 4–47 ms (`[parkfix]` log).
- `HeadFaceCacheFile` is the generated-face cache from the RPCS3 white-faces fix. Faces built natively before RSX
  render-target readback works may be cached blank; delete it once a match loads.
- Temporary diagnostics on branch `mlb16-work` (not for upstream), all under `MLB_SEMA_TRACE=1`: label releases with
  stream position, CALL/JUMP into the rings, park patches, and SPU PUTs into the command ring (`[ringput]`).

## 2026-10-27 (evening): Bink spin fixed, match load reaches the pitching matchup

- **`cellFsFtruncate` never worked:** it resolved to `libs/filesystem/cellFs.c`, which looks fds up in its own table,
  one that the port's `cellFsOpen` (`runtime/ppu/ppu_fs.cpp`) never fills, so every call returned EBADF.
  - The game sizes `HeadFaceCacheFile` (in the install folder) to 314,584,064 bytes before a match, and
    `ArenaLogoCache` to 9 MB. Implemented next to `cellFsOpen` (branch `cellfs-ftruncate`).
  - With it, the match load gets past the starting lineups to the **pitching matchup** screen.
- **The Bink SPU spin (99% of a core) is fixed:**
  - The task retried `cellSpursEventFlagWait` forever, because each call failed at once with `0x80410911` (null
    pointer): its event flag address was 0.
  - The task DMAs its setup block from r4, the taskset argument. `_cellSpursTasksetAttributeInitialize` dropped
    `argTaskset` and `cellSpursCreateTasksetWithAttribute` created the taskset with args 0.
  - Carrying it through (branch `spurs-taskset-args`) gives r4 = `0x01476E80`; the Bink thread now sleeps in its
    wait like on hardware. That's a core back.
- **The match-load stall is a real deadlock**, not a slow first cache build: with the SPU watchdog at 600 s it never
  moves. Three parties wait on each other:
  - **PPU:** the main thread spins in `0x9fb898`, the WWS job manager's wait for job list "MeshJobs0". Two SPU lanes'
    progress counters are stuck.
  - **SPU:** two mesh jobs (`jobmod_c690c2f7b09a`, loop at `0xe510`–`0xe550`) wait for ring space. Each DMA-reads an
    RSX label holding the GPU's read position in its ring (labels 72, 75 and 76; rings at EA `0x11900000`,
    `0x11f00000` and `0x12100000`). It retries while that position is inside the region it wants to overwrite.
  - **GPU:** our command walker has drained everything up to a self-JUMP "park" at IO `0x0138ef10`. The GPU waits
    there for the PPU to write more; the PPU waits for the jobs.
  - The only label release in that frame is label 75 = `0x11f02080`, inside the region its job wants. So that ring's
    previous output was never marked as read by the GPU.
- **Walker oddities found on the way** (not yet shown to cause the deadlock):
  - The game extends its big ring by leaving a self-JUMP at the write head. After 50 ms (`GCM_PARK_GRACE_MS`) the
    walker gives up on the park and jumps to `put`; a real RSX waits.
    - With the grace effectively infinite the game still boots and gets to the same stall.
    - The selected team's **logo now shows on team select**. It was probably in commands the early jump skipped.
  - At about 2.5 minutes in, the walker reads data as commands at IO `0x2d4e80`. The frame `CALL`s a vertex-program
    upload at `0x2d4ef0`; the walker was 0x70 bytes before it.
    - It reads an upload argument (`0x709d302a`) as a CALL to an unmapped address, and drops the rest of that pass.
    - Possibly a block rewritten while the walker, which drains on 4 ms ticks, was still behind it.
  - New-style CALL targets are masked with the old JUMP mask (`0x1ffffffc`) and new-style JUMPs (`w & 3 == 1`)
    aren't decoded at all. This game seems to use only old-style JUMPs and new-style CALLs into low IO, so neither
    has bitten here yet.
- **Ghidra:** the recompilation addresses are now named in the project (`Gfx_DisplayFlip`, `Gfx_FlipHandler`,
  `Mutex_LockOwned`/`Mutex_UnlockOwned`, `Cond_Wait`, `File_Duplicate`, `File_Open`, `File_AioCallback`,
  `MemPool_Acquire`, `MemPool_SharedMemContAnim_LazyInit`, `MemContainer_Carve`).
- **Next:** find which commands should advance labels 72/75/76 and why the walker never reaches them: trace every
  `0x1d6c`/`0x1d74` release value per frame, and the `CALL`s into the mesh rings (IO `0x0090xxxx`, `0x00f0xxxx`,
  `0x0110xxxx`).

## 2026-10-27 (afternoon): driving it headless; match load gets to the lineups

- **Scripted input:** `PAD_FILE=<file>` presses whatever button mask is written to the file (`0x4000` CROSS,
  `0x0008` START, `0x0040` DOWN; `"0x0040 3"` holds it for 3 reports). With a frame dump every 10 frames, the menus
  can be driven without a person: title → START → "Online disabled" OK → Play Now → Offline Rosters → team select →
  stadium select (a still image of Kauffman) → game options → starting lineups.
- **`-O2 -march=native` build** (`recomp-work/build-o2`, 16.5 min at `-j4`): same menu frame rate as `-O1`, 421
  frames in 20 s each (about 21 fps). The menus are paced at one frame per three 60 Hz vblanks, so they don't show CPU
  speed.
- **"3D TV detected":** `cellVideoOutGetResolutionAvailability` reported the 3D modes (ids `0x80`+) available, so
  the game asked to enable 3D and, on yes, rendered over-under. Fixed on branch `cellvideoout-no-3d` (PR draft 5).
- **The match-load NULL call** came from the game's memory containers:
  - `0x14fc4` carves an N MB container out of the default one (id `0xFFFFFFFF` at `0x0111ffc4`): get its size,
    destroy it, recreate it smaller, create its own.
  - ps3recomp's `sys_memory_container_get_size` rejected the default id, took its arguments in the wrong order and
    wrote `{total, used}` instead of `{total, available}`. `sys_memory_allocate_from_container` (syscall 350) wasn't
    registered.
  - So the "SharedMemContAnim" pool at `0x0134a464` (built lazily in `0x99c45c`) never got memory, and `0x8c5508`
    called a method on a NULL buffer.
  - Fixed on branch `sys-memory-containers` (PR draft 4). Both pools now allocate (25 MB in container 1, 7 MB in
    container 3), and the NULL call is gone.
- **Next stall:** about 30 s into the lineups screen, an SPU job on the job manager (local store `0xe510`) re-reads
  RSX label 76 (`0x200004c0`; labels start at `0x20000000`) until the runtime halts it. Something that should write
  that label, probably a semaphore release in the GPU command stream, isn't writing it.

- **Label 76, first look:** a temporary trace in our runtime branch (`MLB_SEMA_TRACE=1`, branch `mlb16-work`) shows
  the GPU command stream *does* release it. The game cycles through five labels, `0x480`, `0x490`, `0x4a0`, `0x4b0`
  and `0x4c0`, with `NV4097_SET_SEMAPHORE_OFFSET` (`0x1d6c`) and texture-read releases (`0x1d74`), on subchannel 0,
  with no semaphore context DMA set (so they land in the label window). The waiting job is
  `jobmod_c690c2f7b09a` (a geometry ring producer: DMA-reads the label at `0xe510`, compares it against its ring
  bounds at `0xe550`).
  - Next: compare the values released against what the job waits for. Candidates: byte order (`0x1d74` is written
    verbatim, `0x1d70` byte-swapped), the walker running behind the job's own output, or releases in
    SPU-built command blocks.
  - Driving there takes about 4 minutes with the `PAD_FILE` script above.

## 2026-10-27 (evening, playtest): menus playable, match load crawls

Played with a PS4 controller through SDL, in a window:
- **Menus work.** Experience level, main menu and moving between screens, at about 20 fps (109 new frames in about
  5 s, read from the frame counter at `0x011203d8`).
- **Button presses sometimes need two taps.** `PAD_ALWAYS_CHANGE=1` (every read returns a full report) didn't fix it.
  The controller is sampled every 4 ms, so short taps aren't the cause.
- **Menu boxes flicker** (only the boxes, not the whole screen): to capture with the renderer's pass dumps.
- **SELECT (Quick Menu) froze it.** This is online code, which also needed our server in RPCS3. Skipped for now;
  offline first.
- **Loading an exhibition match crawled.** The crowd job workload (`CrowdJobList`) started and stadium data loaded,
  then reads dropped to about 17 KB/s with no new frames. The main thread was busy in game code (`0x5499a0`,
  `0x643200`…) while the job module `255e2961` kept dropping into the SPU interpreter.
  - A gdb breakpoint on `spu_interp_run_until` caught 7,293 interpreter entries in 25 s, mostly at `0xd7a0`,
    `0xd9d0`, `0xd810` and `0xd878`: functions reached only through function pointers, so the lift never had them as
    entry points.
  - The 28 addresses are now passed to `lift_jobmods.py` (`LBP_JOBMOD_EXTRA_FUNCS`, in `tools/lift_spu.sh`). The
    job modules gained functions, e.g. `7ac07dde` went from 241 to 630. Test pending.
- **CPU use:** the Bink SPU program spins at 99% of a core in its wait loop even with no movie, and a job manager
  thread runs at about 84%.
- **Match load still stalls with the entry points added** (interpreter entries are now only the job manager's own
  self-modifying code at `0xa68`). At the start of each match load the main thread calls a NULL object:
  - `0x8c5508` writes the game's "FE data block" (`HDDBlk`) to the HDD cache. It takes a buffer from a 3-slot pool
    at `0x0134a464` via `0x98c72c`, which hands one out only if the slot's current owner says it's done (virtual
    `+0xc`).
  - All three were busy, so no buffer came back. The caller doesn't check, and calls a virtual method on 0 (call at
    `0x8c5638`, logged as `[null-call] near lr=0x8C563C`).
  - Next: read the pool's owners (slots `+0x18`, `+0x24`, `+0x30`) at that moment to see what's still holding them.
- `cellFsAioWrite`'s completion callback (`0x79c2c`) only forwards to the request's handler, which the game sets up
  before submitting, so completing synchronously looks safe here.

- The build now lives in `recomp-work/build` (lifted source in `recomp-work/recompiled`), not the session's scratch
  folder.

## 2026-10-27 (later still): first menu

- **The autosave-screen stall** was the game's file copy into its `/dev_hdd1` cache (`0x776d8`, a "Duplicate"
  routine called from the file open at `0x78ca4`). It writes 64 KB chunks with `cellFsAioWrite` and polls a status
  word set by the completion callback (`0x79c2c`). `cellFsAioWrite` (`0x4CEF342E`) wasn't implemented in ps3recomp,
  so nothing was written and the callback never ran. Implemented on our branch `cellfs-aio-write` (upstream PR
  candidate 3), modeled on its `cellFsAioRead`.
- **Result:** about 500 async writes during boot, then the game's first interactive screen, **"Choose your
  experience level" (Veteran / Novice)**, drawn correctly through Vulkan with the team background behind it. This
  is the first-time setup a new save sees before the main menu.
- The Bink thread still spins at 99% of a core; whether the intro movie plays isn't known yet.
- **New unresolved imports, reached for the first time:** `0xB20F87B3`, `0xCE4374F6`, `0xDBF22BD1`, plus
  `cellHttpInitCookie` and `cellSpursAttributeEnableSystemWorkload` from before.

## 2026-10-27 (late night): flip loop fixed; stuck on the autosave notice

- **The 100,000 flips a second** came from the flip handler running twice per flip. `cellGcmSetFlipImmediate` calls
  it at request time (a compatibility "early edge" in ps3recomp) and the pump calls it again at completion. MLB's
  handler advances the displayed-frame counter (`0x011203dc`) each time, so it overtook the rendered-frame counter
  (`0x011203d8`). `DisplayFlip` (`0x692dc`) then saw a frame ready forever and flipped non-stop. Upstream's
  `GCM_FLIPCB_ONTICK=1` drops the early call. With it, about 35 flips a second, and the two counters stay a few
  frames apart.
- **Where it stops now:** the screen stays on the autosave notice (cover art) from about 20 s on, flipping buffer 0
  only, while the game keeps loading in the background. It opens `MLB_INTRO.BIK` early, probably to preload.
  - The main thread polls an event flag with an ~11.6 ms timeout (`0x4c7ae0` → `0x78ca4` → `0x785d8` → `0x776d8`).
  - The Bink SPU program (`BinkSPU.spu.elf`, task created with a SPURS event flag, `direction=2`, SPU to PPU) spins
    in its wait-for-work loop (`0x3870` → `0x3438` → `0x3448`) at 99% of a core.
  - Next: check ps3recomp's SPURS task event flag and queue path between the two (`docs/spurs-task-signal-wake.md`
    upstream).
- **Upstream PR branches** are in the fork (`MatRanc/ps3recomp`: `template-vulkan-backend`, `gcm-handler-thread`),
  with the draft descriptions in `PR_DRAFTS.md` on its `pr-drafts` branch. Nothing is opened yet.

## 2026-10-27 (night): boot freeze fixed, display stops updating

**The freeze was a leaked lock, not Bink.** The game usually hung on its first screens, at random, before ever
opening the intro movie; one early run got through by luck. Stacks of the stuck process (`eu-stack`, function names
are PS3 addresses) and its guest memory (`gdb`, reading `vm_base + address`) showed:
- `DisplayFlip` (thread entry `0x692dc`) flips each frame, then waits on a condition for the flip handler
  (`0x691b0`, registered with `cellGcmSetFlipHandler`) to clear a "flip pending" flag at `0x011203e0`.
- MLB's lock wrappers (`0x9a2da8` lock, `0x9a2e78` unlock) record the owner's thread ID at `struct+8` and only
  unlock when the caller's ID matches.
- ps3recomp delivers vblank/flip handlers at whatever firmware call a guest thread happens to make, and the callback
  inherits that thread's ID (`ppu_guest_call`). The flip handler ran nested inside `DisplayFlip`'s own call, while
  `DisplayFlip` held the lock at `0x012450d8`. It re-entered the lock (same ID), and its unlock cleared the owner
  record. `DisplayFlip`'s own unlock then saw an owner of 0 and was skipped, so the lock leaked (owner 4,
  recursion 1). The display engine and a sound thread blocked on it forever, and with the handler stuck inside the
  pump, no more vblank/flip callbacks were delivered.
- On a PS3 these handlers run on GCM's own interrupt thread.

**Fix** (our ps3recomp branch `gcm-handler-thread`, upstream PR candidate): `GCM_HANDLER_THREAD=1` delivers the
handlers only on a dedicated host thread with its own identity. With it, 3 of 3 runs get past the boot screens,
open `MLB_INTRO.BIK`, and go on loading user data, crowd sound banks, profiles and the logo cache.

**Next problem:** the presented picture stays on the autosave notice (cover art) from about 20 s on, while the game
keeps running. `DisplayFlip` now calls `cellGcmSetFlipImmediate(0)` about 100,000 times a second, so its flip loop no
longer paces itself. The movie may be playing unseen.

Also unresolved, not blocking yet: `0x4cef342e`, `cellSpursAttributeEnableSystemWorkload` and `cellHttpInitCookie`.

## 2026-10-27 (evening): first picture through Vulkan

- **Packages:** Vulkan needs `vulkan-headers`, `glslang-devel` and `spirv-tools-devel` (Fedora). Build with
  `-DPS3RECOMP_RSX_VULKAN=ON`. With glslang found, CMake reports "guest programs available": the game's own RSX
  shaders are translated (HLSL, then SPIR-V).
- **Template gap (upstream fix candidate):** `templates/project/main.cpp` picks D3D12, Metal or the headless
  renderer and never Vulkan, even in a Vulkan build. The runtime's own host tool (`runtime/host/host_posix.c`) does
  pick it. Adding a `PS3RECOMP_RSX_VULKAN` case with `rsx_vulkan_backend_init/present/pump_messages` fixes it.
- **Run:** `PS3RECOMP_VK_WINDOW=1 PS3RECOMP_VK_GUEST_PROGRAMS=1`. The renderer picks the RTX 3060 Ti, opens a
  1280×720 Wayland window, and the game draws its **legal and licensing screen correctly**: text, logos and the
  animated red and blue background. `PS3RECOMP_VK_DUMP=<file>.ppm PS3RECOMP_VK_DUMP_EVERY=60` saves frames.
- **Next blocker:** it freezes right after opening the intro movie, `PS3/UI/Movies/intro/MLB_INTRO.BIK`. Bink's
  SPU program is lifted, but playback hangs.
- `PATCH_GD.WAD` fails to open from the data install folder (`BCUS01089_R`); it may be found in the update folder
  instead, not checked yet.

## 2026-10-27 (later): SPU job manager running, data install done

- **Policy module:** the WWS job manager's SPURS policy module is 12,032 raw bytes at `0x105bb80` (file offset
  `0x104bb80`) that SPURS loads at local store `0xA00`. Wrapped as an SPU ELF at that base
  (`wrap_spu_elf.py --base 0xA00`) and lifted with `build_spu_workloads.py`, its text segment registers under the
  fingerprint the runtime looks up (`0x75BFE68AE96D7577`).
  - The runtime's `SPU_DUMP_MISS=<dir>` setting wrote the missing programs out exactly as fingerprinted, which is
    how both were found.
  - The same job manager runs LittleBigPlanet, whose fixes are already upstream.
- **Job modules:** `lift_jobmods.py --engine-elf` finds and lifts the 7 job modules linked into the EBOOT. One has
  308 undecoded words, the same count as the MP3 decoder program, so they're probably its data tables.
- **Build:** with the 13 SPU programs (`-DSPU_DIR=`), the rebuild takes 38 s. All of this is in `tools/lift_spu.sh`.
- **Results:**
  - All three job workloads resolve and run.
  - The game's 9.5 GB first-boot data install completes in about 50 s.
  - The game then creates more workloads and its seven sound streaming threads. Job loads stall and resume through
    SPU DMA lists and interrupts, and the job manager's self-modifying code is handed to the SPU interpreter.
- **Open problems:**
  - The job manager sometimes spins at local store `0x2534`, repeating one DMA transfer until the runtime's
    watchdog halts that SPU (256 identical transfers). LBP's notes say this policy module gets code patched into it
    by SPURS at load time (holes at `0xAEE`–`0x11B0`, `executeStage` at `0x14f4`); `SPURS_PM_DUMP` checks for that.
    - Checked with `SPURS_PM_DUMP=1`: the same holes as LBP's (1,728 of 1,730 bytes zero, `executeStage` empty).
      No game code writes the main-memory copy in 60 s. Upstream has no fix for LBP either.
    - That may be expected: the job manager writes code into its own local store (the runtime logs these as
      `spu-smc` and runs them through the SPU interpreter). That would fill the holes on the SPU side, where a probe
      of main memory can't see them.
    - The spin re-reads the same 128 bytes of the workload's data (data + `0x80`) 256 times, which looks like
      polling an empty job queue rather than a crash. The game keeps re-running those workloads afterwards. Still to
      confirm.
  - One guest write to NULL+0x48 from `0x7b988`.
  - The headless null renderer shows nothing yet; the Vulkan backend is next.

## 2026-10-27: first boot

**Lifting the PPU code** (`tools/probe.sh`):
- `ppu_loader.py` finds 15,790 functions from the OPD table and 433 firmware imports across 32 libraries.
- `ppu_lifter.py` lifts 43,501 functions to about 6.5M lines of C++ in 11 chunks, in about 1 minute.
- **Pass `--code-end 0xf2c634`.** Without it the lifter treats the read-only data after the code (1.5 MB, up to
  `0x10a43e8`) as code: 1.93M undecodable words and 954 MB of output. Ghidra's section table shows the last
  executable section ends at `0xf2c634`.
- With it, 6,847 words stay undecoded. They are float constants (1000.0, 32767.0, 10000.0…) and zero padding
  between functions, not unsupported instructions.

**Building** (ps3recomp's `templates/project`, clang, `-j6`):
- No compile errors. 10 min 55 s wall time, about 1.5 GB of RAM per chunk.
- Result: a 205 MB native Linux executable.
- The runtime itself needs `SDL2-devel` (Fedora: `sudo dnf install SDL2-devel`) and builds in seconds.

**Running** (headless null renderer, no SPU programs lifted yet):
- The game's own code runs natively. It sets up memory, SPURS, GCM and sysutil, starts its threads ("Job Manager
  Event Handler", "IMFE PPU Thread"…), draws its first frames and flips buffers, creates its trophy context, finds
  a PS4 controller through SDL, and reads `MLBPS3_SYS.WAD` from the disc.
- First stop: the game's "not enough system storage (5,120 MB)" dialog. The runtime resolves `/dev_hdd0/game` under
  the same root as the disc, which was a read-only ISO mount. Fix: a writable root folder holding a `PS3_GAME`
  link to the disc and `game/BCUS01089` (the v01.10 update).
- With that, the game offers its optional data install, the runtime auto-answers yes, and the game starts
  installing ("Installing Game Data… File 1/6").

**Missing so far:**
- The WWS job manager's SPURS policy module (12,032 bytes at `0x105bb80`) isn't lifted, so the game's SPU job
  workloads (`Wws_Job`: `DXTJob`, `MeshJobs0`, `MeshJobs1`) never run. ps3recomp's `lift_jobmods.py` targets this
  job format (`c0dec0de` modules).
- The Bink SPU program (110,560 bytes) isn't lifted yet, so video won't play.
- Two unresolved firmware imports showed up early in the boot.

**SPU programs:**
- The EBOOT embeds 5 SPU ELF images: two WWS job checks, an MP3 decoder, surround audio and Bink.
- It also holds 8 `c0dec0de` job modules (4 inside those ELFs), including the ICE mesh program.
- RPCS3 showed 25+ SPU programs in use, so the rest come from the game's data files.
- All 5 ELF images lift with `spu_lifter.py --auto-functions` with no unsupported instructions.

**Firmware coverage** (approximate, from ps3recomp's handler table vs our 433 imports):
- 165 have a real implementation, 71 a thin one, 46 return a constant, 32 run inside the runtime itself, and 119
  are missing. Missing imports return CELL_OK without doing anything.
- Gaps for online play:
  - `cellHttp` is registered under made-up names, so its handlers never match the game's imports (37 of 48 missing).
  - `cellSsl`/`cellHttps` have no TLS.
  - NP auth, ID lookup and signaling are missing or stubbed.
- ps3recomp's own docs recommend Sony's `libsre` (lifted from your firmware) over its built-in SPURS, which has hung
  most SPURS-heavy titles.

## Reference

- **Upstream PRs** (submitted to `sp00nznet/ps3recomp`, all open): #640 templates Vulkan-on-Linux; #641 cellFsAioWrite;
  #642 cellVideoOut stereo; #643 cellSpurs taskset args; #644 sys_memory container size; #645 cellFsFtruncate;
  #646 cellSpurs maxContention; #647 GCM handler thread; #648 GCM wrap JUMP slot; #649 cellSpurs maxContention clamp +
  RequestIdleSpu; #650 PS3RECOMP_TRACE.
- **Clean-room rule:** ps3recomp/mlb16-recomp are MIT; RPCS3 is GPLv2. Never copy or port RPCS3 (or other GPLv2) code;
  write original code against ps3recomp's own structures. This keeps the project MIT-compliant and upstreamable.

## 2026-11-05: walker waits attribution (exp/walker-waits)

- Roundtable item 1: `PS3RECOMP_WALKER_WAITS=1` (default 0) times 14 blocking sites on the walker thread
  (queue submit, one-shot/readback fences, pipelined-set fences, present fences/locks, acquire, async shader wait,
  pipeline create, WCB lock, MT spins) and prints one `[walker-waits]` line per `[walker-prof]` report.
  Offline crowded replay: steady frame blocks ~0.9 of 42.6 ms, so the live parse gap needs one live run
  ([walker-waits.md](walker-waits.md)). Fixtures 34/34. Branch pushed to `fork` only.

## 2026-11-05: one-shot waits attribution + create batching (exp/oneshot)

- Live finding: walker in-drain blocking is almost all `oneshot` (15-40 ms/frame, 16-165 one-shots/frame). Added
  `[oneshot-by]` (per-caller, `flush:<why>` split) under `WALKER_WAITS=1`, and `PS3RECOMP_VK_BATCH_CREATES` (default 0,
  live key) that records texture/target/snapshot creates into the open batch instead of submit+wait. Replay: cold
  one-shots 815 -> 5, md5 unchanged, fixtures 34/34; live effect unmeasured ([oneshot-waits.md](oneshot-waits.md)).


## 2026-11-05: WWS native PM head/tail, Stage A contract + native ticket claim (exp/wws-native)

- Roundtable item 2, Stage A. `PS3RECOMP_WWS_NATIVE_PM` (env, default 0) routes the WWS ticket claim
  (`0x23A8 -> 0x23BC -> 0x2460/0x2498/0x24C0/0x250C/0x2518/0x2534`, GETLLAR/PUTLLC + LS caches) through
  `runtime/spu/wws_native_pm.h`; the rest of the head (`0x3020`/`0x3080`/`0x31B0`) and the job submit/exit tail
  (`0x12AB8 -> 0x4A5C...`, `0x2D10 -> 0x3630`) stay lifted. The generated PM is patched reproducibly by
  `tools/c690_ref/patch_wws_pm_native.py`.
- Differential test (`runtime/spu/tests/run_wws_native_pm_test.sh`): lifted vs native on identical 256 KiB LS +
  memory model, five cases (claim/index/tag/early-out/PUTLLC retry), LS + main memory + r3-r5 byte-identical;
  claim cost 156.7-164.1 -> 135.4-142.6 ns (1.12-1.21x).
- Kill criterion: the claim is <1 % of the 4.59 lane-ms head+tail, so the saving is ~0.02 lane-ms/frame. Stage A
  does not reach -50 %, Stage B (native runner) must not start, key stays 0, no live A/B. Contract and the full
  address map: [`wws-native-pm.md`](wws-native-pm.md).
- Offline: `check_spu_context_abi.sh` PASS (no `spu_context` change); `build-sync` builds; video fixtures 34/34 after
  a test-only weak `g_spu_put_hook`/`ps3_ms_now` stub (the `exp/live-ab` Bink DMA log path broke the fixtures on the
  base commit). Code to `fork` only.

## 2026-11-05: WWS head/tail sub-step ledger; kill criterion reached (exp/wws-native)

- New `PS3RECOMP_C690_STEPS=1` (default 0, measurement only) splits the c690 inter-job turnover into
  tail-submit / PM-exit+host / claim+alloc / staging+dispatch, with the synchronous DMA copies and GETLLAR/PUTLLC
  timed inside each step. One automated pitch-meter run on `build-sync` (37.9 fps, 81.8 jobs/lane/frame,
  429,533 short gaps over 28 s): total turnover 50.1 us/gap (p50 41) = 4.10 lane-ms per lane per burst.
  Tail submit 7.7 us (0.63 lane-ms), PM exit+host 22.3 (1.83), claim+alloc 2.2 (0.18), staging+dispatch 17.9 (1.46).
- All copy work together is ~0.8 lane-ms: code re-DMA 0.23, other GET 0.27, output PUT 0.27, atomics 0.30. The
  dominant time is lifted PM/job instruction execution (pm_exit 20.1 us, head_stage 11.7 us), so no safe local
  sub-step saves 1 ms of the burst; the only >1 ms item needs the native PM exit translation. No fix implemented,
  key stays 0. Details: [`wws-native-pm.md`](wws-native-pm.md) §9.

## 2026-11-05: SPU codegen v2 -- the 3.3x is float semantics, not spills (exp/spu-v2)

- Phase 1 (perf `cycles:u` + `perf annotate` per lifted symbol on the real 0xFDA0 stage corpus and the fncap21
  captures): on the 0xFDA0 stage 63 % of host time is SPU emulation (39 float-semantics/FTZ, 26 frsqest/fi/frest),
  12.7 % spills, 3.3 % local store, 3.7 % shufb, 0.2 % dispatch. Weighted over the measured hot kernels
  (four float-bound kernels, 29 % of c690 job time): emulation 66.6 %, register file/spills 18.8 %, LS 3.8 %,
  branch 0.4 %. Removing all spills buys at most 1.23x; the old 26-37 % spill figure was static and over-counted the interrupt write-back
  blocks. This explains the 0 % from `--local-regs`/`--precise-regs`/`--inline-direct`.
- Phase 2 implemented `--spu-ftz-hw` (lift flag, default off) + MXCSR FTZ|DAZ enter/leave in
  `spu_run_with_halt`: generated float helpers use plain loads, hardware FMA and only result sign-of-zero
  canonicalisation. 0xFDA0 stage 1.36x (268 -> 198 us) and 0x0C018/0xBED0 1.23-1.25x, all four corpus pairs and every
  fncap21 capture bit-identical; `run_tests.sh` 11/11. Kill gate is 1.5x, so the flag stays off, no live A/B and no
  title lift change. Details and the next design (lazy -0 canonicalisation, measured 1.54x upper bound):
  [`spu-codegen-v2.md`](spu-codegen-v2.md).

## 2026-11-05: SPU codegen v3 -- FTZ live-switchable and measured (exp/spu-v3)

- One binary now carries both SPU float variants. `tools/spu_ftz_twin.py` derives a `_ftz` twin for all 14 lifted
  modules (5 workloads, WWS PM, 7 jobmods, DXT) from the patched base source; the runtime registry keys entries by
  variant, `spu_run_with_halt` samples the new live key `PS3RECOMP_SPU_FTZ` once per run and sets/restores MXCSR
  FTZ|DAZ, and dispatch prefers variant 1 while the key is on. Default 0 keeps the old module; the play tree's weak
  `spu_register_function_var` makes the twins inert there.
- Exactness: 0xFDA0 stage 4/4, fncap21 9/9, DXT entries 6/6, WWS PM claim 5/5, new 0x6D68 capture 1/1, all
  LS+GPR bit-identical; `run_tests.sh` 11/11, local-regs/ib-known-targets/context-ABI pass, video fixtures 34/34.
- One pitch-meter launch, 3 alternating 10 s windows per arm: fps 34.60 (off) vs 35.09 (on), paired -0.49 +/- 2.94
  (CI covers 0) because the meter is walker/PPU-bound; c690 job wall time fell 204.5 -> 183.1 us/job (1.117x) and
  c690 lane time 3124 -> 2926 ms/s. Second (last) launch captured a real 0x6D68 entry; the other four unmeasured
  kernels did not dispatch in the meter window. No lazy-canonicalisation/scalarisation pass shipped.
  Details: [`spu-codegen-v3` in spu-codegen-v2.md](spu-codegen-v2.md).

# Team select: the ~1 s input lock, the missing 3D logo, and the Start press

Date: 2026-11-03. **Static work only**: no game was launched. Sources are the
EBOOT in Ghidra, `recomp-work/play_20261103_163747.log` with its MangoHud CSV
and 2 s thumbnails, the runtime sources in `ps3recomp-play`, and the prior
measurements in `tick-and-resync.md`, `textures-dummy-census.md`,
`visuals-wcb-gamma.md` and `hitch-census.md`. No GPL emulator source was read.

Run: `play_20261103_163747` (16:37:47-16:43:44, `build-play`, WCB=2, MangoHud,
2 s thumbnails). `PS3RECOMP_VK_PRESENT_NEW_ONLY=1` is on in
`play_windowed.sh`, so a producer stall appears as a long MangoHud frame (it
does at the Start press and at match load). The team-select frames therefore
really are steady; this is not the old "host presents at 70 fps while the guest
flips at 20" trap (`tick-and-resync.md`).

## TL;DR

| # | Question | Evidence | Most likely cause | Cheapest next probe | Fix proposed |
|---|---|---|---|---|---|
| 1 | ~1 s with no input after a d-pad team switch | No stall in the whole team-select window: 2,543 walker reports, max report gap **39 ms**, max walker wait **38 ms**, max walker **6.8 ms**; MangoHud n=2,375, max **41.2 ms**, p99 36.9 ms, 0.4 % > 38 ms; no `[fs]`, `[spurs]`, `[c690]`, `[mfc]` or `[LWCOND]` lines | The title's scripted frontend wheel transition/debounce, not a renderer or asset stall. The missing logo (2) removes the visual feedback, so the lock reads as a freeze | `PAD_ALWAYS_CHANGE=1` for one switch; `PS3RECOMP_AUDIO_DIAG=1` (or `PS3_NO_AUDIO=1`) on a second; capture the team frame with `GCM_PARK_GRACE_MS=600000` | No renderer change unless a probe implicates the pad contract; then report at ~60 Hz / mark fresh only on change. If an audio cue is implicated, make it fire-and-forget. Restore the logo (2) so the transition is visible |
| 2 | Floating 3D team logo never shows | No load failure: no `[fs]` open at a switch, no logo `open FAIL`, no texture/dummy miss; black and magenta dummies are pixel-identical; WCB does not change the frame; the only `open FAIL`s are CONFIG_MAIN.TXT, playlist.dat, User_LiveRoster, CustomCams, PATCH_GD.WAD (17 lines, all unrelated) | The FIFO walker's jump-park resync drops the command range that contains the logo draw. It is a draw-loss bug, not a texture or file bug | `GCM_PARK_GRACE_MS=600000 ./play_windowed.sh` (existing knob; the logo showed when the grace was effectively infinite) | Do not abandon a park whose skipped range contains draw/state commands: replay the range through the normal dispatcher (the walk-range fix) or wait for the producer patch; make it default after a team-select + in-game check |
| 3 | Start-menu lag on the Start press | 16:38:09-16:38:20: 325 walker reports, max report gap **887 ms**, max wait **886.5 ms**, max draws **297**; MangoHud 18-33 s max **882 ms**, p99 **856 ms**, 9.6 % > 38 ms; 27 pipeline/surface creations; 26 `[fs]` lines (preloads only); no SPU/c690 | Guest-side frontend construction (`FUN_00010230` -> `FUN_00087ca0`): rosters/stats, front-end textures, UI widgets, ArenaLogoCache; the walker is idle, so the host renderer is not the limiter. The "ONLINE DISABLED" modal is a separate flow gate | Time the press with an already-warm shader/pipeline cache and record `[walker-prof]`; if unchanged, the cost is guest PPU. Use the title's own debug prints (`ENTERING FRONTEND`, `FrontEnd Load Complete`) by enabling their print flag | No renderer fix for the bulk. Warm/persist the frontend shader/pipeline cache (small); profile the frontend PPU path. The offline modal is product behaviour, not a stutter |

## 0. Timeline (2 s thumbnails + log)

| Wall | Screen | Evidence |
|---|---|---|
| 16:37:47 | boot | log line 1 |
| 16:37:59-16:38:06 | intro movie, legal screen | `MLB_INTRO.BIK`; thumb 16:38:01/03/05 |
| 16:38:06-16:38:08 | frontend preload | `userteam.dat`, `DefaultProfiles`, `ArenaLogoCache` x3, `texture_cache.xml`, `CACHE_C8372833/FFC767D5/B2340B7E`, JPGDEC+PNGDEC modules (log 3319-3577) |
| 16:38:11.969 / 14.054 | "PRESS X" title | thumb `...891969`, `...894054` |
| 16:38:16.144 | "ONLINE DISABLED" modal | thumb `...896144`; `FUN_0024b204` |
| 16:38:20.359 | main menu | thumb `...900359`; `MUSIC.SKX` open at 16:38:20 (log 3978) |
| 16:38:43.340 | team select, Royals | thumb `...923340` |
| 16:38:53.717 | team select, Tigers | thumb `...933717` |
| 16:39:06.187 | team select, Athletics | thumb `...946187` |
| 16:39:20.758 | team select, Braves | thumb `...960758` |
| 16:39:33.430 | team select, Cubs | thumb `...973430` |
| 16:39:43-16:39:48 | Play Now / Stadium Select | thumbs `...983883`, `...988051` |
| 16:39:47-16:40:04 | match load | `CACHE_C8372833`, `ROM.WAD`, `GD.WAD`, 5,094 ms guest gap (log 13758, 15610) |

The team-select window is 16:38:41-16:39:43. There is no per-switch marker in
the log, so the switches are identified from the thumbnails, not from a log
event. The `[usercmd]` counter lines at 16:38:44/56, 16:39:08/20/33 are
periodic (one line per 256 commands, ~21/s) and are not pad events.

## 1. What a team switch does, and why ~1 s of input is lost

### 1.1 Static: the frontend and the wheel

* `FUN_00010230` (`EBOOT.elf:0x00010230`) is the frontend state entry. It calls
  `FUN_00087ca0` (`0x00087ca0`) and then the frontend loop `FUN_00089f18`.
* `FUN_00087ca0` builds the frontend: roster/stat load (`FUN_008b5124` ->
  `rosters.bin`, `FUN_008b4de0` -> `stats.bin`), front-end textures, the UI
  widget table, `FUN_006b8c30(..., "Miscellaneous\\TeamSelect_Wheel", ...)`,
  and `FUN_00800600(5,0x12,2,0xfa)` for the 2-D logo cache.
* The wheel is a frontend UI widget: constructor `FUN_000ecaf8`
  (`0x000ecaf8`), vtable `0x01035d20`. The script-facing methods include:
  * `FUN_000ecb70` (`0x000ecb70`) - read the current selection (team index
    0..0x1d).
  * `FUN_000ecdec` (`0x000ecdec`) - apply the selection: scroll the list
    (`FUN_006c4794`), write the selected value into the UI state arrays
    (`FUN_006a13e0`/`FUN_006a1444`), and fetch the team record pointer
    (`FUN_006764f4` -> `FUN_0068d928`). These are UI-state/table operations,
    not file or audio calls.
  * `FUN_000ecff4`, `FUN_000ed0c0`, and the wheel transitions
    `TS_WHEEL_ShowForViewedTeam` (`0x01126414`) /
    `TS_WHEEL_BranchOnSelectedTeam` (`0x0112642c`) through
    `FUN_00dd0b6c/8c/cf8/d18`.

So the switch itself is a UI-script state change plus a scroll and a table
lookup. It does **not** open a file, read a roster, start an SPU job, or wait on
a stream. The title has a separate `SelectTeamAudio*` path, but the calls in
this handler are not an audio API and audio is silent in this run. The
roster/stats and the 2-D logo cache are loaded once when the frontend is
entered, before team select.

### 1.2 Log: the switch window is steady

| Measurement (16:38:40-16:39:40) | Value |
|---|---:|
| walker reports | 2,543 |
| max `at=` gap between reports | **39 ms** |
| max walker wait | **38.0 ms** |
| max walker time | **6.8 ms** |
| draws per frame | ~107 (max 133) |
| MangoHud rows 53-110 s | 2,375 |
| MangoHud max / p99 / >38 ms | **41.2 ms / 36.9 ms / 0.4 %** |
| `[fs]`, `[spurs]`, `[c690]`, `[mfc]`, `[LWCOND]` in the window | **none** |

This matches the user's own observation ("frame times steady, <= 38 ms") and
rules out a guest render/asset stall: with `PRESENT_NEW_ONLY=1` a stalled
producer shows a long MangoHud frame (see the 882 ms Start frame and the 5,060
ms match-load frame). The title kept producing frames throughout the switch.

### 1.3 Runtime suspects, in order

1. **The pad report contract.** `cellPadGetData` reports a packet only when
   `s_data_fresh[port]` is set, and freshness is set for every port on each
   host poll; the poll is capped at 4 ms (`ps3recomp-play/libs/input/cellPad.c`
   `PAD_POLL_INTERVAL_MS` at :682, the freshness gate at :700 and :873-876).
   A real DualShock reports at ~60 Hz, so the title is seeing up to 250
   reports/s. `PAD_ALWAYS_CHANGE=1` restores the always-a-packet behaviour;
   if the lock changes with it, our report rate/gating is implicated.
   The SDL hotplug rescan is period **1 s** (`PAD_SDL_RESCAN_MS` :480) and runs
   while any of the four host ports is free (:536-560). With one controller on
   port 0 three ports are free, so `SDL_NumJoysticks()` runs every second.
   Normally that is cheap; the run shows no repeated "SDL controller
   connected" line, so it never re-opens the pad. It is nevertheless the only
   1 s-periodic call in the pad path and is worth a one-line log before the
   scan (not implemented here).
2. **The title's team-selection audio path.** The title has
   `SelectTeamAudioInit/WriteCatalog/SetAudio` strings and audio is silent in
   this run. We did not tie an audio call to the observed switch handler, but a
   synchronous cue (or a wait for a voice slot) in our backend would look like
   this. Probe: `PS3RECOMP_AUDIO_DIAG=1`, then a coarse A/B with
   `PS3_NO_AUDIO=1` (reports audio unavailable).
3. **The UI transition itself.** The wheel is scripted; a 1 s lockout while the
   wheel settles is plausible and would also exist on hardware. With the 3-D
   logo missing (section 2) there is no animation to explain the pause, which
   is why it reads as a freeze.

### 1.4 Probe and fix

* Probe A (pad): one team switch with `PAD_ALWAYS_CHANGE=1`, same build and
  scene. If the lock changes, fix the contract: deliver at ~60 Hz (PS3 report
  rate) and mark a report fresh only when the state changes, instead of
  setting all ports fresh every 4 ms.
* Probe B (audio): one switch with `PS3RECOMP_AUDIO_DIAG=1`; if a mix/device
  line coincides, repeat with `PS3_NO_AUDIO=1`. Fix: make the selection cue
  fire-and-forget and never block the menu thread (if it is the audio path).
* Proposed fix if neither probe moves it: treat the lock as title behaviour
  and do not paper over it with an input hack. Restore the logo (section 2) so
  the transition is visible; that is the user-facing fix.

## 2. The floating team logo

### 2.1 Asset path

* The 3-D logo is a **model**, not a texture: a UI opcode
  `FUN_000f56f4` (`0x000f56f4`), case 2 (the `0x000f5980` call site), returns
  `logos\%s` (`0x00f405e4`) from the team-id table at `0x01177574`, whose
  entries are the `logo3d_*` names at `0x01011c7c` (`logo3d_BAL`, `logo3d_BOS`,
  ... `logo3d_AAA_SL`; team id 0..0x89). The same name family is used in game:
  `UI_InGame3DTeamLogo` (`0x00fb0860`, referenced by `FUN_00510130`).
* The 2-D crest/logo cache is separate: `FUN_00800600` (`0x00800600`) opens
  `ArenaLogoCache` (`0x00fc0044`) and creates `UILogoDepth%d`
  (`0x00fc0054`) and `UILogoSlot_%d_%d` (`0x00fc0064`). This is the cache that
  is visible (2-D crests and the `AT` panel are present in the thumbnails).
* There is no loose `logo3d_*` file under `PS3_GAME/USRDIR`; the names are
  asset keys, and the WAD/front-end fd is already open. A team switch produces
  no `[fs] open` line at all (the next open after 16:38:20 is
  `CACHE_C8372833` at 16:39:47).

### 2.2 Does it fail to load?

No evidence of a load failure:

* No `[fs] open FAIL` names a logo (the 17 failures are CONFIG_MAIN.TXT,
  playlist.dat, User_LiveRoster, CustomCams and PATCH_GD.WAD).
* No dummy/texture-miss warning at team select; the census measured **zero**
  format/address/size misses on the team frame (`textures-dummy-census.md`).
* Black and magenta dummy builds are pixel-identical on the team frame
  (mean diff 0.0), and WCB off/ondemand/async are identical: it is not a
  texture or a readback (`visuals-wcb-gamma.md`).
* `PS3RECOMP_WALKER_RESYNC_FIX=1` alone did not bring the logo back in the 11-02
  census; with the park grace made effectively infinite the logo did show
  (`progress.md`, 10-27 evening).

The consistent cause is the **jump-park resync**: a self-JUMP park that stands
with an unchanged `put` past `GCM_PARK_GRACE_MS` (default 10,000 ms) makes the
walker jump `get` to `put`; the resync replays fences/labels but not the
draw/state commands in the skipped range (`ps3recomp-play/libs/video/cellGcmSys.c:1255-1266`,
`:1905-1929`; `tick-and-resync.md`). The 3-D logo draw is in that range.

### 2.3 Probe and fix

* Cheapest probe: one team-select screen with
  `GCM_PARK_GRACE_MS=600000 ./play_windowed.sh` (the env knob exists; no
  rebuild). If the logo appears, the draw loss is confirmed and the fix is the
  resync, not a texture path. Repeat with
  `PS3RECOMP_WALKER_RESYNC_FIX=1 GCM_PARK_GRACE_MS=600000` to see whether the
  walk-range path restores it when the grace is not infinite.
* Fix: never abandon a park whose skipped range contains method commands.
  Replay the range through the normal dispatcher (real `s_state`, fences,
  flip markers, draw sink) instead of teleporting; or wait for the producer to
  patch the park. The walk-range fix is the intended shape; make it default
  once a team-select + in-game 3-D-logo check passes.
* This is **not** the same cause as the input lock. They co-occur on the same
  screen: the missing logo makes the 1 s scripted transition look like a hang.

## 3. The Start press

### 3.1 What runs

`FUN_00010230` (`EBOOT.elf:0x00010230`) enters the frontend and calls
`FUN_00087ca0` (`0x00087ca0`), which:

* loads rosters (`FUN_008b5124`), stats (`FUN_008b4de0`), front-end textures;
* creates the UI widgets, including `Miscellaneous\TeamSelect_Wheel`;
* initialises `ArenaLogoCache` and the `UILogoSlot` atlas (`FUN_00800600`);
* then runs the frontend loop `FUN_00089f18`.

The log catches this window from 16:38:09 to 16:38:20 (before the main menu at
16:38:20.359):

| Measurement (16:38:05-16:38:21) | Value |
|---|---:|
| walker reports | 325 |
| max `at=` gap / max wait | **887 ms / 886.5 ms** |
| max walks / draws in a frame | 53.5 ms / **297** |
| MangoHud 18-33 s max / p99 / >38 ms | **882 ms / 855.9 ms / 9.6 %** |
| `[rsx engine]` pipeline/surface lines | 27 |
| `[fs]` lines | 26 (all frontend preloads) |
| `[spurs]` / `[c690]` / `[mfc]` | none |

The walker is idle for most of each frame (wait ~0.9 s, walker ~0.6 ms), while
the guest submits up to 297 draws and creates pipelines/surfaces. That shape is
guest-side frontend construction, not our draw execution or file I/O. The
"ONLINE DISABLED" modal at 16:38:16 is a separate flow gate:
`FUN_0024b204` (`0x0024b204`) shows it when an online mode is unavailable; it
waits for the user's OK. It is not a frame stall.

### 3.2 Probe and fix

* Cheapest probe: time the same press on a second run with the existing shader
  disk cache and `PS3RECOMP_VK_PIPELINE_CACHE=1` warm (no rebuild). If the
  0.5-0.9 s frames remain, the cost is guest PPU frontend work. The title
  already has debug prints `ENTERING FRONTEND` and `FrontEnd Load Complete`
  (`FUN_00052b5c`); enabling their print flag gives free phase boundaries.
* Fix: no renderer fix for the bulk while the walker is idle. Warm/persist the
  frontend pipelines (small, per `hitch-census.md`), and profile the frontend
  PPU path (UI layout/text and first-use texture decode are the likely cost).
  Bypassing the offline modal is a product-flow change, not a performance fix;
  do not do it as a lag workaround.

## 4. Numbers used here

* Run `play_20261103_163747`: 61,417 log lines, 12,139 MangoHud CSV rows,
  thumbnails every 2 s.
* Team select 16:38:40-16:39:40: 2,543 walker reports; max report gap 39 ms;
  max wait 38.0 ms; max walker 6.8 ms; ~107 draws/frame; MangoHud max 41.2 ms,
  p99 36.9 ms, 0.4 % > 38 ms.
* Start/frontend 16:38:05-16:38:21: 325 reports; max gap 887 ms; max wait
  886.5 ms; max draws 297; MangoHud max 882 ms, p99 855.9 ms.
* Match load: 5,094 ms between flip batches (16:39:59 -> 16:40:04); one
  walker report at 16:40:04 with 3,484 draws, walker 3,426.7 ms, wait 1,666.7
  ms; MangoHud max 5,059.6 ms.
* File opens: none between 16:38:20 (`MUSIC.SKX`) and 16:39:47
  (`CACHE_C8372833`), i.e. the whole team-select window.

## 5. Caveats

* `[fs]` opens are always logged, but reads are sampled (first per fd plus the
  first 50); the absence of read lines in the switch window is not by itself
  proof of no reads. The absence of opens is proof of no new files.
* The 1 s lock cannot be timestamped from the log (there is no pad-event log
  line). The thumbnails give 2 s resolution; a `PAD_TRACE`-style line (button
  mask + guest PC on change) is the probe that would settle the exact
  press/release timing.
* "Start press" here means the press-start/title -> frontend transition. If the
  user meant the in-game START/pause menu, the same method applies but the
  window must be identified from the thumbnails first; this run has no
  multi-second gap in play besides match load and the tiled transition frames.

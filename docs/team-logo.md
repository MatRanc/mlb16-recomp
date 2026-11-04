# Team-select 3D logo: capture trigger, static map, and the replay plan

Date: 2026-11-04. **No game was launched** (AGENTS.md: automated runs measure
only the pitch; the user drives team select). Sources: the EBOOT in Ghidra, the
runtime on `exp/live-ab` in `ps3recomp-play`, and the earlier notes in
`team-select-and-logos.md`, `boot-spu-hitches.md` (§3), `tick-and-resync.md`,
`textures-dummy-census.md`, `visuals-wcb-gamma.md`. No GPL emulator source was
read.

Branch `exp/live-ab`, build `recomp-work/build-play`. The old jump-park/resync
theory is **disproved** for this symptom: the 600 s-grace run had zero `FIFO
resync` lines and zero `gcm.resync` events (`boot-spu-hitches.md` §3), so this
document starts from "the logo draw is not submitted, or is submitted but not
visible", and prepares the capture that settles which.

## TL;DR

| # | Question | Answer |
|---|---|---|
| 1 | Can a FIFO dump be placed at a scene the user navigates to? | Not before this change: `PS3RECOMP_DUMP_FIFO` opened on the first drain and only `PS3RECOMP_DUMP_FIFO_SKIP` (a flip count known at launch) could reach a later screen. A live trigger was added (below), default off. |
| 2 | What is `FUN_000f56f4` case 2? | A string resolver: it `snprintf`s `"logos\\logo3d_<TEAM>"` and returns a UI string. It opens no file, binds no model, submits no draw/RT/SPU job. |
| 3 | What is `UI_InGame3DTeamLogo` at `0x00fb0860`? | It is the **string** `"UI_InGame3DTeamLogo"`, not a function. Its only code user, `.opd.FUN_00510130` (`0x00510130`), uses it to pick a camera transition (`"Camera_Trans_0%d"`). It does not render the logo. |
| 4 | What actually draws the floating logo? | A `ScriptUIElem`/`ScriptModel` (UI element) whose scene is rendered by `.opd.FUN_009d611c` → `FUN_009d5c08` → `FUN_009d53bc` → `FUN_006bd388`/`FUN_006bde60`; the element is composited as a 2D textured quad. On the type-5/7 path it first renders into `ScriptUIElem_RT` and copies that back into the element texture. |
| 5 | Most likely causes, in order | (1) element/model never submitted, (2) offscreen RT or RT→texture copy never composited, (3) draw submitted but killed by state/texture handling in our engine, (4) camera/visibility transform, (5) name/model-load hand-off. |

## 1. The capture the user runs

### 1.1 What was added

`libs/video/cellGcmSys.c` gained an **armed** mode for the existing FIFO dump:

| Variable | Meaning |
|---|---|
| `PS3RECOMP_DUMP_FIFO=<path>` | output file (unchanged). Unset/empty = completely inert. |
| `PS3RECOMP_DUMP_FIFO_FRAMES=<n>` | flips to record (default 100, unchanged). |
| `PS3RECOMP_DUMP_FIFO_SKIP=<n>` | flips to drop first (default 0, unchanged; legacy mode only). |
| `PS3RECOMP_DUMP_FIFO_ARM=1` | **new**: do not open at boot. Wait for a trigger. |
| `PS3RECOMP_DUMP_FIFO_TRIGGER=1` | **new**: live switch (in the `PS3RECOMP_CTL` file, name table `PS3_LIVE_DUMP_FIFO_TRIGGER`). A rising edge starts the dump at the next flip boundary. |

Without `ARM` the old boot-time behaviour is byte-for-byte unchanged. With
`ARM`, no file is created until the trigger, and the dump then holds exactly
`FRAMES` complete frames (the flip after the trigger closes the prior frame and
is not written). The trigger is level-detected, so a held `1` does not restart
the dump; write `0` (or remove the file) and raise it again for a second one.

The offline fixture `libs/video/tests/test_gcm_fifo_dump_trigger.c` compiles
`cellGcmSys.c` in and links the real `ps3_live_ctl.c`; it checks: armed waits,
no trigger = no file, control-file write starts at a flip boundary, exactly two
frames, held trigger does not re-arm, and the payload is big-endian method
words ending in `0xFEAD0000|buffer`. Run it with
`python3 tools/test_gcm_fifo_dump_trigger.py` (PASS, 8/8).

### 1.2 One-line launch for the user

Run from anywhere; the script uses absolute paths. `rm -f` first so a stale
control file cannot fire at boot.

```sh
rm -f /tmp/logo.ctl /tmp/logo_cap.trigger && cd /home/m/Documents/mlb16-online/recomp-work && PS3RECOMP_CTL=/tmp/logo.ctl PS3RECOMP_DUMP_FIFO=/tmp/logo_teamselect.bin PS3RECOMP_DUMP_FIFO_FRAMES=12 PS3RECOMP_DUMP_FIFO_ARM=1 PS3RECOMP_WALKER_CAPTURE=/tmp/logo_cap ../mlb16-recomp/tools/play_windowed.sh
```

Then, in the game:

1. Go to the team-select screen and let it settle for about 5 s (the floating
   3-D logo should be on screen; the 2-D crests and the `AT` panel are the
   reference).
2. Run this **one trigger line** in another terminal:

   ```sh
   rm -f /tmp/logo.ctl; echo 'PS3RECOMP_DUMP_FIFO_TRIGGER=1' > /tmp/logo.ctl; touch /tmp/logo_cap.trigger
   ```

   The first two parts start the 12-frame FIFO dump at the next flip
   (`/tmp/logo_teamselect.bin`). The `touch` is optional and captures one
   frame with guest memory (`/tmp/logo_cap.fifo/.state/.mem`) for the deeper
   per-draw pass in §5.3.
3. Wait ~1 s, close the window (or Ctrl-C). The log prints
   `[cellGcmSys] FIFO dump armed ...`, then `FIFO dump trigger seen`, then
   `FIFO dump closed: N words written`; `[walker-cap] frame complete` follows
   for the optional capture.

Do not use a `SKIP` frame count with this run; the trigger replaces it. The
standard `play_windowed.sh` defaults are unchanged. Nothing here changes the
scene the user drives or any renderer switch.

### 1.3 Capture format (what the next mission reads)

`PS3RECOMP_DUMP_FIFO` is **raw, big-endian 32-bit method words**, no header:
method headers (`type = w >> 29` normally 0 or 2) followed by their data
words, plus a `0xFEAD0000 | buffer` marker at each flip. JUMP/CALL/RET and
NOPs are not written; the stream is linear and branch-free. `tools/replay_rsx`
accepts it directly (raw fallback), and also accepts the optional `RSXF`
header form used by `PS3RECOMP_WALKER_CAPTURE` (`<prefix>.fifo`, native
little-endian 32-byte header + big-endian payload). The walker capture adds
`.state` (register/VP/constant snapshot) and `.mem` (guest pages actually read)
for `tools/replay_rsx_engine`.

## 2. What the static path actually does

### 2.1 `FUN_000f56f4` case 2: a string, not a draw

Decompiled at `0x000f56f4`, case `iVar4 == 2` (`0x000f58a8`):

```c
iVar4 = _opd_FUN_0001e1e0();                       /* returns 1 */
if (iVar4 != 0 && 1 < param_3 && arg_blob[1] != NULL) {
    iVar4 = *(int*)arg_blob[1];
    if (0 <= iVar4 && iVar4 < 0x8a)
        FUN_00a402f8(buf, 0xff, "logos\\%s",
                     (&PTR_s_logo3d_BAL_01177574)[iVar4]);
    FUN_009a0fb4(param_4, 0, buf);                  /* UI string result */
}
```

The format string is at `0x00f405e4`; the name table starts at `0x01177574`
(entries point at `0x01011c7c`: `logo3d_BAL`, `logo3d_BOS`, …). It is invoked
from `.opd.FUN_00297b60` (`0x002982d4`) through the frontend UI-script opcode
table. There is **no** model load, WAD read, SPU job, render target or draw in
this function; if the script table entry is unpatched, the result is an empty
string. Whether a `ScriptModel`/`ScriptUIElem` consumes that string is script
side, not in the EBOOT (uncertain, but it is the hand-off to check if the FIFO
has no model draw).

### 2.2 `UI_InGame3DTeamLogo`: a state name, not a renderer

`0x00fb0860` reads `"UI_InGame3DTeamLogo"` (64-byte read confirms the string
and the following `"Camera_Trans_0%d"`). Its only xref is `.opd.FUN_00510130`
(`0x00510130`, code use at `0x005101e4`), which compares the current UI state
name against `UI_InGameRandom*`, `UI_InGame3DTeamLogo`, then stores a
`"Camera_Trans_0%d"` transition name. A wrong/missing camera transition can
move a rendered logo off-screen, but this function submits nothing.

### 2.3 The visible logo is a `ScriptUIElem`

* `ScriptUIElem::Render` is `.opd.FUN_009d611c` (`0x009d611c`), a state machine
  (`vtable+0x28` read, `+0x2c` set). Its early-outs: global
  `DAT_013561d8 != 0` (`DisableUIElem`), `DAT_011409a8 == 0` (alloc failure),
  `DAT_011409a8+0x1f4 != 0`, `elem+0x10 == 0` (element texture null),
  `elem+0xc == 0` (element model object null), `((int*)elem+0xc)[3] == 0`.
* State 1 (`0x009d642c`) creates `DAT_0115e920 = FUN_004b8f04("ScriptUIElem_RT",
  w, h, tile, 2, 0x20, 0x14008)` **only for scene/texture node types 5 or 7**;
  scissor via `FUN_004b92cc`, then `FUN_009d5c08` (`0x009d5c08`) renders the
  scene into it.
* State 2 (`0x009d66bc`) is a second `FUN_009d5c08` pass; state 3
  (`0x009d6a3c`) draws the offscreen RT as a textured quad
  (`FUN_004c6ac0`/`FUN_00895128`); state 4 (`0x009d6b14`) copies the RT into
  the element texture with `FUN_009e2048`, unregisters `"UIElem_Thumbprint"`,
  frees the RT (`FUN_004b8678`) and finishes the 2D element draw.
* The normal visible path (types ≠ 5/7) renders with `FUN_009d5c08` into the
  element's own texture (`elem+0x10`) and always composites as a 2D quad
  (`FUN_009d4a78`, `FUN_006a0e48`/`FUN_006a1048`). So even the "3D" logo is an
  offscreen scene plus a 2D composite, not a direct framebuffer draw.
* Scene render: `FUN_009d5c08` logs `"RenderUI() Nm: %s An:%s"`, bails if
  `DAT_013561e0 != 0`, then calls `FUN_009d53bc` (`0x009d53bc`), which draws
  with `FUN_006bd388` (node mask `model+0x1ec & 4`) and `FUN_006bde60`
  (`& 8`) using either the element target (`elem+0x398`) or the default camera
  `&DAT_01120000`.
* Model load/bind: `ScriptModel` path `.opd.FUN_009cd1ac` (`0x009cd1ac`) →
  `FUN_009b5578` (`0x009b5578`, probes `"%s.%d"` chunks, materials under
  `$(PLATFORM)/Stadiums/MaterialDatabase/`) → `FUN_004c345c`/`FUN_004c1c28` →
  `FUN_004c220c` (`Model::Bind`). Element creation is `FUN_009d7b30`
  (`0x009d7b30`) → `FUN_009d4c68` (target texture lookup in the `"world"`
  scene; failure text `"Did not find target texture: %s"`) and
  `FUN_00edd244` (asset path via `FUN_006b8c30`/`FUN_006be8e8`).
* RT plumbing: `FUN_004b8f04` → `FUN_004b8ae4` → `FUN_004de4cc`; region
  `FUN_004b92cc` → `FUN_004db8e0`; pass begin/end `FUN_004b4d40` /
  `FUN_004b5268` (the latter ends with `gcm_SetDrawArrays(..., 8, ...)`).
  Guest tile/zcull wrappers are `FUN_0006dfb4` (SetTileInfo+BindTile),
  `FUN_0006e0a0` (UnbindTile), `FUN_0006e0c8` (BindZcull) and `FUN_0006e264`
  (UnbindZcull); the CPU-side RT→texture copy is `FUN_009e2048`.

### 2.4 FIFO signature of the 3D model

The game writes raw FIFO words itself (no `cellGcmSetDraw*` imports). The mesh
draw is emitted by `FUN_004d9084` (`0x004d9084`):

* arrays: `gcm_SetDrawArrays(ctx, 5, count, count)` (`0x004d9110`);
* indexed: `gcm_SetDrawIndexArray(ctx, 5, count, count, 0x10, off, 1)`
  (`0x004d912c`). Primitive `5` = triangles, `0x10` = 16-bit indices.

The corresponding FIFO methods are `0x1808` (BEGIN_END; data 5 = triangles,
0 = end), `0x1814` (vertex batch, first/count in the header), `0x1824` (index
batch), `0x1818` (inline vertex data), `0x181C`/`0x1820` (index offset/format),
`0x1714` (inline array), textures `0x1A00+unit*0x20` (`+0x04` format, `+0x0C`
control0 enable bit 31, `+0x18` NPOT size), surface `0x0210` colour offset /
`0x0214` zeta offset / `0x0208` format, depth `0x0A74`/`0x0A6C`/`0x0A70`,
blend `0x0310`/`0x0314`/`0x0318`/`0x0320`, cull `0x183C`/`0x1830`. The 2D UI
layer ends each pass with primitive `8` (quads, `FUN_004b5268` at
`0x004b52bc`), which is the easy discriminator from the 3D mesh.

## 3. Hypotheses, ranked, with the check each needs

The capture check refers to `/tmp/logo_teamselect.bin` (§1) unless stated.

| # | Hypothesis | Why plausible | Check in the capture | If confirmed |
|---:|---|---|---|---|
| 1 | **The element/model is never submitted** (null model, missing scene/target, `DisableUIElem`, an early-out in `FUN_009d611c`, or the `logo3d_*` name never reaches a `ScriptModel`) | The whole EBOOT path above can skip without any file error; a missing model is a silent early-out. | `tools/fifo_dump_scan.py /tmp/logo_teamselect.bin --draws --prim 5`: **no** triangle batches `0x1814`/`0x1824` and only primitive-8 quads; compare with the 2D crest draws. Also `grep -n 'did not find target\\|Model::Bind\\|logo3d'` in the run log if those debug strings are enabled. | Static: trace the script hand-off and `FUN_009b5578`/`FUN_004c220c`; add a log at element creation. |
| 2 | **Offscreen RT / RT→texture copy is not composited** (the element renders into `ScriptUIElem_RT` or its own texture, and the 2D quad samples stale/empty memory) | The visible logo is an offscreen scene plus a quad composite (`FUN_009d5c08`/`FUN_009d53bc`/`FUN_009d4a78`); `FUN_009e2048` copies tile-by-tile and returns silently on a format/size mismatch. | Triangle draws are present **and** `0x0210` (colour target) changes to an offscreen offset before them; the following primitive-8 quad binds that texture (`0x1A00`). Look for the `0x1A00`/`0x1A04` offset/format of the composite and whether it follows the RT offset. | Check our RT/tile/zcull and texture-resolve path; try `PS3RECOMP_RSX_WCB=0` / `=3` and `PS3RECOMP_ENG_TEXHASH_FULL=1` on the same scene. |
| 3 | **Draw submitted but invisible in our engine** (depth/stencil/cull/blend, texture format/swizzle, wrong pipeline, primitive dropped) | `rsx.draw_submit` only counts batches today, and the earlier frame is otherwise healthy. | Triangle batches are present in the dump; `replay_rsx` counts them. Around them, read `0x0208` format, `0x0A74`/`0x0A70`, `0x183C`, `0x0310`, and the `0x1A04` texture format. | Bisect with `PS3RECOMP_ENG_TEXMEMO=0`, `PS3RECOMP_ENG_TEXHASH_FULL=1`, `PS3RECOMP_ENG_DEPTH_VIA_GUEST=1`, `GCM_SEMA_ACQUIRE=1`; then add a per-draw drop reason if none moves it. |
| 4 | **Camera/visibility transform** (logo drawn off-screen, node mask, `UI_InGame3DTeamLogo` transition camera) | `FUN_009d53bc` falls back to the default camera if `elem+0x295` does not resolve, and `FUN_006bd388`/`FUN_006bde60` skip on node-mask bits. | Triangle draws are present but with viewport/scissor/transform constants unlike the neighbouring UI; `0x0A20`-range transform constants and the viewport/scissor methods (`0x0A0C`/`0x08C0`). | Compare matrices with a working 3-D element; check `DAT_013561d8`/node masks. |
| 5 | **Name/model hand-off** (runtime-patched selector table at `0x01126600`, or the script property never filled) | `FUN_000f56f4` case 2 emits the name through a table whose in-file entries look zero-keyed; if the table is not registered, the result stays empty. | Same as #1 (no model draw). The capture cannot see the selector; use the title's own debug prints or `Model::Bind` logging. | Static: check the table registration at runtime; log the `logos\\logo3d_*` result. |
| 6 | **Mesh/SPU job ordering** (geometry not ready when the draw executes) | The Gfx path uses guest threads, labels/semaphores and `cellSpurs`; our `ACQUIRE` is a no-op by default. | Semaphore methods `0x64/0x68/0x6C` and `GCM_SEMA_ACQUIRE` labels around the draw; SPU workload activity at the same time. | Try `GCM_SEMA_ACQUIRE=1 GCM_SEMA_ACQUIRE_MS=1000`; if it wedges, bound the wait. |

The earlier census already rules out a general texture/dummy/WCB fault on the
team frame (`textures-dummy-census.md`, `visuals-wcb-gamma.md`), which is why
#1/#2 are ahead of #3.

## 4. Switch reference (real names, defaults)

| Doc shorthand | Real variable | Default | What it really does |
|---|---|---|---|
| `ENG_TEXMEMO` | `PS3RECOMP_ENG_TEXMEMO` | **on** (only `=0` disables) | per-frame texture binding memo (`rsx_draw_engine.c:6914`) |
| `ENG_TEXHASH_FULL` | `PS3RECOMP_ENG_TEXHASH_FULL` | off | force a full texture hash every frame instead of every 8 (`:6905`, `:2426`) |
| `ENG_FSBIND_ALL` | `PS3RECOMP_ENG_FSBIND_ALL` | off | **logging only**: widens `PS3RECOMP_ENG_FSBIND_LOG` to full-screen draws; needs `PS3RECOMP_ENG_FSBIND_LOG=<n>` (`:5337-5357`) |
| `ENG_DEPTH_VIA_GUEST` | `PS3RECOMP_ENG_DEPTH_VIA_GUEST` | off | read depth targets through guest memory instead of the backend conversion (`:6907`, `:2295`) |
| `GCM_SEMA_ACQUIRE` | `GCM_SEMA_ACQUIRE` (no prefix) | 0 | make `ACQUIRE` block; bound with `GCM_SEMA_ACQUIRE_MS` (`cellGcmSys.c:3387-3412`) |
| WCB | `PS3RECOMP_RSX_WCB` | 2 | readback/resolve mode; `0` off, `3` async (`play_windowed.sh`) |
| cube RT | `PS3RECOMP_RSX_CUBE_RT` | on in `play_windowed.sh` | assemble cube render targets on the GPU (`rsx_draw_engine.c:2561`) |

`PS3RECOMP_ENG_FSBIND_ALL` alone produces no output; do not treat an empty log
as evidence. `GCM_SEMA_ACQUIRE=1` without a bound can wedge the drain, so use
`GCM_SEMA_ACQUIRE_MS` when trying it live.

## 5. Offline replay plan (next mission, on the user's capture)

### 5.1 Build the tools (no game launch)

```sh
cd /home/m/Documents/mlb16-online/recomp-work/build-play
ninja ps3recomp_sdk/replay_rsx ps3recomp_sdk/replay_rsx_engine
```

`replay_rsx` needed six newer draw-engine no-op stubs in
`tools/replay_rsx_core.c` to link on this branch; that is fixed. It is a
dispatch/register-file replay, not a renderer: it prints draws/clears/flips,
a whole-stream state hash and timing, and **does not emit a per-draw list**.

### 5.2 First question: are the logo's draws in the stream at all?

Use the scanner (works on raw `.bin` and `RSXF` `.fifo`; it splits at
`0xFEAD0000` markers and follows the walker's header count rule):

```sh
cd /home/m/Documents/mlb16-online
python3 ps3recomp-play/tools/fifo_dump_scan.py /tmp/logo_teamselect.bin --top 30
python3 ps3recomp-play/tools/fifo_dump_scan.py /tmp/logo_teamselect.bin --draws --prim 5
python3 ps3recomp-play/tools/fifo_dump_scan.py /tmp/logo_teamselect.bin --draws | head -200
```

Interpretation:

* Per frame, team select is roughly 55 draws in the earlier censuses; the 2-D
  UI is primitive 8. If **no primitive-5 batches** appear in any of the 12
  frames, the guest never submits the 3D model → hypothesis #1/#5. Follow the
  static path in §2.3 and log `Model::Bind`/element creation.
* If primitive-5 batches appear, note the `0x0210` colour target, `0x0208`
  format, `0x1A04` texture formats and depth/blend/cull state printed on each
  line. Compare the logo's batch with the neighbouring 2-D quads: an offscreen
  target followed by a quad that samples it is hypothesis #2; a main-target
  triangle with odd state is #3/#4.

Cross-check the same stream through the walker:

```sh
recomp-work/build-play/ps3recomp_sdk/replay_rsx /tmp/logo_teamselect.bin \
    --frames 3 --warmup 1 --verbose --verify
```

The `draws` count must agree with the scanner's `draw_batches`; a mismatch
means the scanner's header walk and the walker disagree (report it, do not
bisect yet). `--verify` should stay deterministic across passes.

### 5.3 If the draw is present: bisect the state

The raw dump cannot supply vertex/texture bytes. If you also ran the optional
`PS3RECOMP_WALKER_CAPTURE=/tmp/logo_cap`, replay that one frame through the
real engine with a null backend and ask for the per-draw text list:

```sh
mkdir -p /tmp/rsxpass
RSX_ENGINE_PASS_DUMP=/tmp/rsxpass RSX_ENGINE_PASS_DUMP_DRAWS=1 \
RSX_ENGINE_PASS_DUMP_MAX=1 \
recomp-work/build-play/ps3recomp_sdk/replay_rsx_engine /tmp/logo_cap \
    --frames 1 --warmup 0
sed -n '1,200p' /tmp/rsxpass/draws.txt
sed -n '1,120p' /tmp/rsxpass/passes.txt
```

`draws.txt` gives pass/draw, pipeline, colour mask, blend, depth and every
bound texture (unit, location, offset, format, width, height). Find the
triangle batch from §5.2 and compare its texture offset/format and state with
the 2-D draws. Then run the same replay with each switch, one at a time, and
diff the output hash/gate (`replay_rsx_engine` prints both):

```sh
PS3RECOMP_ENG_TEXMEMO=0            ... replay_rsx_engine /tmp/logo_cap --frames 1
PS3RECOMP_ENG_TEXHASH_FULL=1       ... replay_rsx_engine /tmp/logo_cap --frames 1
PS3RECOMP_ENG_DEPTH_VIA_GUEST=1    ... replay_rsx_engine /tmp/logo_cap --frames 1
GCM_SEMA_ACQUIRE=1 GCM_SEMA_ACQUIRE_MS=1000 ... replay_rsx_engine /tmp/logo_cap --frames 1
```

A changed `output hash` names the component; an unchanged one with a visibly
different `draws.txt` (e.g. a texture binding that disappears) names a binding
memo. Remember that with the null backend the PPM/PGM images are meaningless
(readback is zeroed); use the text lists and the digest only.

### 5.4 If the draw is absent

Do not keep replaying. Go back to the static path: does the `logo3d_*` string
reach a `ScriptModel` (script-side hand-off at `FUN_000f56f4` caller
`.opd.FUN_00297b60`), does `FUN_009b5578`/`FUN_004c220c` bind (`Model::Bind`
log), and does `FUN_009d7b30` create the element (target-texture lookup
`"Did not find target texture: %s"`)? A second user run with the title's own
debug prints is the cheapest confirmation; do not change the capture setup
mid-measurement.

## 6. Reproduction

```sh
# offline trigger unit test (no game)
cd /home/m/Documents/mlb16-online/ps3recomp-play
python3 tools/test_gcm_fifo_dump_trigger.py

# scanner sanity on an existing raw pitch dump (no game)
python3 tools/fifo_dump_scan.py \
  /home/m/Documents/mlb16-online/recomp-work/kauffman_pitch.bin --top 10

# replay tool sanity (no game)
cd /home/m/Documents/mlb16-online
recomp-work/build-play/ps3recomp_sdk/replay_rsx \
  recomp-work/kauffman_pitch.bin --frames 2 --warmup 0
```

## 7. Caveats

* The user's capture is the first evidence; nothing here claims the draw is
  present. The static map says where to look, not what the answer is.
* `FUN_000f56f4` case 2 is a string resolver, so "the logo3d path" is a script
  hand-off, not a renderer. If the selector table at `0x01126600` is not
  registered at runtime, the hand-off can fail with no file error; that is
  hypothesis #5 and needs a log/debug print, not a FIFO.
* The `ScriptUIElem_RT` type gate (node types 5/7) and the `UIElem_Thumbprint`
  path are for offscreen/thumbnail elements; the normal visible element renders
  into its own texture. Both end as a 2D quad, so "not composited" and "not
  submitted" look similar in a rendered frame and are separated only by the
  FIFO.
* The scanner is a heuristic over the same header/count rule the walker uses;
  it is not a full RSX decoder. If `replay_rsx` and the scanner disagree on
  draw count, trust the walker and report the fixture.
* Earlier docs' `ENG_FSBIND_ALL` shorthand is only a log widener; it cannot
  restore a draw.
* One user run only; the optional walker capture adds guest pages (capped at
  768 MiB) and one frame of capture time. If it is too slow on the user's
  machine, drop `PS3RECOMP_WALKER_CAPTURE` and the `touch` from §1.2; the raw
  FIFO dump is unaffected.

#!/usr/bin/env python3
"""MLB 16 launcher: toggles, paths, and shareable config. Windows and Linux.

Stdlib only (Python 3.9+, tkinter). Starts the recomp executable with the
environment that tools/play_windowed.sh sets, so a run from here matches the
measured setup. It does not mount discs, decrypt, or touch game data.

    python mlb16_launcher.py                 GUI
    python mlb16_launcher.py --print-config  print the current config as JSON
    python mlb16_launcher.py --config f.json run headless with a saved config
    python mlb16_launcher.py --show-env      print the env the game would get

Config format (also what "Copy config" puts on the clipboard):
    {"format": "mlb16-launcher-1", "paths": {...}, "toggles": {...}}
Anything missing from a pasted config falls back to the defaults below.
"""

import argparse
import json
import os
import platform
import subprocess
import sys
from pathlib import Path

FORMAT = "mlb16-launcher-1"
IS_WINDOWS = platform.system() == "Windows"

# ---- paths -------------------------------------------------------------------
# Relative to this file: tools/launcher/ -> mlb16-recomp -> the work folder.
HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DEFAULT_PATHS = {
    # folder with vfs/, shader cache and the build directories
    "work_dir": str(REPO.parent / "recomp-work"),
    # decrypted EBOOT made from YOUR copy of the game (never shipped)
    "eboot": str(REPO.parent / "EBOOT.elf"),
    # build directory inside work_dir that contains the executable
    "build_dir": "build-depth",
    # executable name without extension; ".exe" is added on Windows
    "exe_name": "MyGameRecomp",
}

# ---- toggles -----------------------------------------------------------------
# (key, default, group, label, env var, kind). kind: bool -> "1"/"0";
# choice -> the value as given. Defaults match tools/play_windowed.sh.
TOGGLES = [
    # Window and presentation
    ("present_mode", "fifo", "Window", "Present mode (fifo / mailbox)", "PS3RECOMP_VK_PRESENT_MODE", "choice:fifo,mailbox"),
    ("present_new_only", True, "Window", "Present only new frames", "PS3RECOMP_VK_PRESENT_NEW_ONLY", "bool"),
    ("present_pace", True, "Window", "Even out present cadence (pacer)", "PS3RECOMP_VK_PACE", "bool"),
    ("dummy_black", True, "Window", "Black dummy for unbound textures", "VK_DUMMY_BLACK", "bool"),
    ("fullscreen", False, "Window", "Fullscreen", "PS3RECOMP_VK_FULLSCREEN", "bool"),
    # Performance (measured wins)
    ("wcb", "2", "Performance", "Readback WCB (0 off / 2 ondemand / 3 async)", "PS3RECOMP_RSX_WCB", "choice:0,2,3"),
    ("cube_rt", True, "Performance", "GPU cube render targets", "PS3RECOMP_RSX_CUBE_RT", "bool"),
    ("tex_pin", True, "Performance", "Texture pin (never evict this frame)", "PS3RECOMP_RSX_TEX_PIN", "bool"),
    ("tex_dirty", True, "Performance", "Texture dirty tracking (no hashing)", "PS3RECOMP_TEX_DIRTY", "bool"),
    ("tick_in_drain", True, "Performance", "Complete flips inside drain", "PS3RECOMP_TICK_IN_DRAIN", "bool"),
    ("walker_wake", True, "Performance", "Walker wake fix", "PS3RECOMP_WALKER_WAKE", "bool"),
    ("remap_flat", True, "Performance", "Flat remap (walker)", "PS3RECOMP_REMAP_FLAT", "bool"),
    ("remap_fast", True, "Performance", "Remap recent-reference memo", "PS3RECOMP_REMAP_FAST", "bool"),
    ("pipe_memo", True, "Performance", "Pipeline answer memo", "PS3RECOMP_PIPE_MEMO", "bool"),
    ("eng_prog_memo", True, "Performance", "Engine program memo", "PS3RECOMP_ENG_PROG_MEMO", "bool"),
    ("vk_draw_state", True, "Performance", "Vulkan draw state cache", "PS3RECOMP_VK_DRAW_STATE", "bool"),
    ("vk_vtx_pre", True, "Performance", "Vulkan vertex prepass", "PS3RECOMP_VK_VTX_PRE", "bool"),
    ("vk_pass_keep", True, "Performance", "Keep render pass open", "PS3RECOMP_VK_PASS_KEEP", "bool"),
    ("wcb_prefetch", True, "Performance", "WCB prefetch", "PS3RECOMP_RSX_WCB_PREFETCH", "bool"),
    ("wcb_stream", True, "Performance", "WCB stream", "PS3RECOMP_RSX_WCB_STREAM", "bool"),
    ("pin", False, "Performance", "Thread affinity pinning (measured SLOWER)", "PS3RECOMP_PIN", "bool"),
    # SPU
    ("spu_mfc_cache", True, "SPU", "MFC lookup cache", "SPU_MFC_LOOKUP_CACHE", "bool"),
    ("spu_ftz", True, "SPU", "SPU flush-to-zero twin", "PS3RECOMP_SPU_FTZ", "bool"),
    ("spu_float_approx", True, "SPU", "SPU float approximation", "PS3RECOMP_SPU_FLOAT_APPROX", "bool"),
    ("spurs_pm_fast", True, "SPU", "SPURS fast reschedule (64)", None, "fixed:SPURS_PM_FAST_RESCHED=64"),
    ("spurs_native", True, "SPU", "Native c690 dispatch (6)", None, "fixed:SPURS_PM_NATIVE_DISPATCH=6"),
    ("nop_skip", True, "SPU", "Skip NV4097 NO_OPERATION runs", "GCM_NOP_SKIP", "bool"),
    ("ref_immediate", True, "SPU", "Immediate SET_REFERENCE publish", "GCM_REF_IMMEDIATE", "bool"),
    # Library fixes
    ("lwcond_real", True, "Library", "Real lwcond waits (no polling)", "PS3_LWCOND_REAL", "bool"),
    ("audio_pace", True, "Library", "Audio pacing (5333 us blocks)", "PS3RECOMP_AUDIO_PACE", "bool"),
    ("fs_write_check", True, "Library", "cellFsWrite error checks", "PS3RECOMP_FS_WRITE_CHECK", "bool"),
    ("spurs_teardown", True, "Library", "SPURS teardown drain", "PS3RECOMP_SPURS_TEARDOWN", "bool"),
    ("gcm_offset_sync", True, "Library", "GCM offset sync on map/unmap", "PS3RECOMP_GCM_OFFSET_SYNC", "bool"),
    ("flip_tick_own", True, "Library", "Menu flip tick from own state", "PS3RECOMP_FLIP_TICK_OWN_STATE", "bool"),
    ("prepare_flip_ret", True, "Library", "PrepareFlip returns buffer id", "PS3RECOMP_PREPARE_FLIP_RET", "bool"),
    ("gcm_handler_thread", True, "Library", "GCM handler thread", "GCM_HANDLER_THREAD", "bool"),
    ("gcm_flipcb_ontick", True, "Library", "GCM flip callback on tick", "GCM_FLIPCB_ONTICK", "bool"),
    # Visual fixes
    ("crowd_fix", True, "Visual", "Crowd sub-draw state fix", "PS3RECOMP_CROWD_FIX", "bool"),
    ("swz_rt", True, "Visual", "Swizzled render targets (tiled wipes)", "PS3RECOMP_ENG_SWZ_RT", "bool"),
    ("depth_bias", True, "Visual", "Guest polygon offset (shadow acne)", "PS3RECOMP_VK_DEPTH_BIAS", "bool"),
    ("fp_txd", True, "Visual", "TXD/DDX/DDY shader translate (jerseys)", "PS3RECOMP_FP_TXD", "bool"),
    ("dof_patch", True, "Visual", "Depth of field patch", "PS3RECOMP_DOF_PATCH", "bool"),
    # Diagnostics (cost frame time when on)
    ("c690_stats", False, "Diagnostics", "c690 stats (costs ~2 fps)", "PS3RECOMP_C690_STATS", "bool"),
    ("walker_prof", True, "Diagnostics", "Walker profile lines", "PS3RECOMP_WALKER_PROF", "bool"),
    ("present_stats", True, "Diagnostics", "Present stats", "PS3RECOMP_VK_PRESENT_STATS", "bool"),
    ("timeline", True, "Diagnostics", "Hitch timeline recorder", None, "fixed-timeline"),
    ("shader_cache", True, "Diagnostics", "Shader disk cache", "PS3RECOMP_SHADER_DISK_CACHE", "bool"),
]
TOGGLE_BY_KEY = {t[0]: t for t in TOGGLES}

# Fixed environment that every run gets (from play_windowed.sh).
FIXED_ENV = {
    "PS3_TITLE": "BCUS01089",
    "PS3RECOMP_PUMP_MS": "8",
    "PS3RECOMP_VK_DUMP_ASYNC": "1",
    "PS3RECOMP_SHADER_CACHE_STABLE_KEY": "1",
    "VK_DRAW_BATCH": "1",
    "VK_FRAME_PIPELINE": "1",
    "SPURS_READY_LEVEL": "1",
    "SPURS_NO_IDLE_PROBE": "1",
    "PS3RECOMP_VK_GUEST_PROGRAMS": "1",
    "PS3RECOMP_VK_WINDOW": "1",
    "__GL_SYNC_TO_VBLANK": "0",
}


def default_config():
    return {
        "format": FORMAT,
        "paths": dict(DEFAULT_PATHS),
        "toggles": {k: d for (k, d, *_rest) in TOGGLES},
    }


def merge_config(cfg):
    """Fill missing keys from defaults; ignore unknown keys."""
    base = default_config()
    if isinstance(cfg, dict):
        for k, v in (cfg.get("paths") or {}).items():
            if k in base["paths"]:
                base["paths"][k] = str(v)
        for k, v in (cfg.get("toggles") or {}).items():
            if k in TOGGLE_BY_KEY:
                base["toggles"][k] = v
    return base


def toggle_env(key, value):
    """Environment entries for one toggle."""
    t = TOGGLE_BY_KEY[key]
    _, _default, _group, _label, var, kind = t
    if kind == "bool":
        return {var: "1" if value else "0"} if var else {}
    if kind.startswith("choice:"):
        return {var: str(value)} if var else {}
    if kind.startswith("fixed:"):
        name, _, val = kind[len("fixed:"):].partition("=")
        return {name: val} if value else {name: "0"}
    if kind == "fixed-timeline":
        return {}  # handled in build_env (needs the output dir)
    return {}


def build_env(cfg, out_dir=None):
    """Full environment for the game, matching the measured play setup."""
    env = dict(os.environ)
    env.update(FIXED_ENV)
    if out_dir is None:
        out_dir = str(Path(cfg["paths"]["work_dir"]) / "launcher_out")
    env["PS3_VFS_ROOT"] = str(Path(cfg["paths"]["work_dir"]) / "vfs")
    env["PS3_GAMEDATA_ROOT"] = cfg["paths"]["work_dir"]
    env["PS3RECOMP_VK_DUMP"] = str(Path(out_dir) / "frame.ppm")
    env["PS3RECOMP_VK_DUMP_EVERY"] = "30"
    env.pop("PAD_FILE", None)
    for key, value in cfg["toggles"].items():
        if key not in TOGGLE_BY_KEY:
            continue
        env.update(toggle_env(key, value))
    # Timeline recorder: on means write hitches next to the outputs.
    if cfg["toggles"].get("timeline", True):
        env["PS3RECOMP_TIMELINE"] = str(Path(out_dir) / "hitches.pstl")
        env["PS3RECOMP_TIMELINE_HITCH_US"] = "250000"
        env["PS3RECOMP_TIMELINE_RING_MB"] = "16"
        env["PS3RECOMP_TIMELINE_FLUSH_MB"] = "2"
    # WCB: 0 means off (variable unset), 2 or 3 turn on readback plus stats.
    wcb = str(cfg["toggles"].get("wcb", "2"))
    if wcb in ("2", "3"):
        env["PS3RECOMP_RSX_WCB"] = wcb
        env["PS3RECOMP_RSX_WCB_STATS"] = "60"
    else:
        env.pop("PS3RECOMP_RSX_WCB", None)
        env.pop("PS3RECOMP_RSX_WCB_STATS", None)
    return env


def exe_path(cfg):
    base = cfg["paths"]["exe_name"]
    if base.lower().endswith(".exe"):
        base = base[:-4]
    name = base + (".exe" if IS_WINDOWS else "")
    return Path(cfg["paths"]["work_dir"]) / cfg["paths"]["build_dir"] / name


def check_paths(cfg):
    """Return a list of problems (empty if ready)."""
    problems = []
    exe = exe_path(cfg)
    if not exe.exists():
        problems.append(f"executable not found: {exe} (build first)")
    eb = Path(cfg["paths"]["eboot"])
    if not eb.exists():
        problems.append(f"EBOOT not found: {eb} (decrypt from your own copy)")
    vfs = Path(cfg["paths"]["work_dir"]) / "vfs"
    if not vfs.exists():
        problems.append(f"vfs folder not found: {vfs}")
    # The game reads its disc through vfs/PS3_GAME. On Windows that is a junction
    # to the mounted ISO, so a dangling link means the ISO is not mounted.
    disc = vfs / "PS3_GAME" / "USRDIR"
    if not disc.exists():
        problems.append(f"disc not readable: {disc} (mount the MLB The Show 16 ISO, then retry)")
    install = vfs / "game" / "BCUS01089"
    if not install.exists():
        problems.append(f"installed update not found: {install} (copy BCUS01089 from RPCS3's dev_hdd0)")
    return problems


def launch(cfg):
    """Start the game with its output captured to out/game.log.

    The returned Popen has an attribute `log_path` so callers can point the user at it.
    """
    problems = check_paths(cfg)
    if problems:
        raise SystemExit("\n".join(problems))
    out = Path(cfg["paths"]["work_dir"]) / "launcher_out" / _stamp()
    out.mkdir(parents=True, exist_ok=True)
    (out / "config.json").write_text(json.dumps(cfg, indent=2))
    env = build_env(cfg, out_dir=str(out))
    exe = str(exe_path(cfg))
    cmd = [exe, cfg["paths"]["eboot"]]
    log_path = out / "game.log"
    print(f"[launcher] output -> {out}")
    print("[launcher] " + " ".join(cmd))
    # Both streams go to one file, line-buffered, so a crash still leaves the last lines.
    log = open(log_path, "w", encoding="utf-8", errors="replace", buffering=1)
    log.write(f"[launcher] {' '.join(cmd)}\n")
    proc = subprocess.Popen(
        cmd, env=env, cwd=cfg["paths"]["work_dir"],
        stdout=log, stderr=subprocess.STDOUT,
    )
    proc.log_path = log_path
    proc.log_file = log  # closed by the caller after wait()
    return proc


def _stamp():
    import time
    return time.strftime("%Y%m%d_%H%M%S")


# ---- GUI ---------------------------------------------------------------------
def run_gui(cfg):
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    root = tk.Tk()
    root.title("MLB 16 launcher")
    # Open centred on the primary screen. Size first, then position from the screen dimensions.
    w, h = 780, 600
    root.update_idletasks()
    x = max(0, (root.winfo_screenwidth() - w) // 2)
    y = max(0, (root.winfo_screenheight() - h) // 2)
    root.geometry(f"{w}x{h}+{x}+{y}")
    state = {"cfg": cfg}

    vars_ = {}
    pvars = {}

    nb = ttk.Notebook(root)
    nb.pack(fill="both", expand=True, padx=8, pady=8)

    # Paths tab
    ptab = ttk.Frame(nb, padding=10)
    nb.add(ptab, text="Paths")
    for i, (key, label) in enumerate([
        ("work_dir", "Work folder (vfs/, build dirs)"),
        ("eboot", "EBOOT (your decrypted copy)"),
        ("build_dir", "Build dir (name inside work folder)"),
        ("exe_name", "Executable name"),
    ]):
        ttk.Label(ptab, text=label).grid(row=i, column=0, sticky="w", pady=4)
        v = tk.StringVar(value=cfg["paths"][key])
        pvars[key] = v
        ttk.Entry(ptab, textvariable=v, width=60).grid(row=i, column=1, sticky="ew", padx=6)
        if key in ("work_dir", "eboot"):
            def browse(k=key):
                if k == "work_dir":
                    p = filedialog.askdirectory()
                else:
                    p = filedialog.askopenfilename()
                if p:
                    pvars[k].set(p)
            ttk.Button(ptab, text="Browse", command=browse).grid(row=i, column=2)
    ptab.columnconfigure(1, weight=1)

    # Toggle tabs by group
    groups = []
    for t in TOGGLES:
        if t[2] not in groups:
            groups.append(t[2])
    for group in groups:
        tab = ttk.Frame(nb, padding=10)
        nb.add(tab, text=group)
        row = 0
        for key, _d, g, label, var, kind in TOGGLES:
            if g != group:
                continue
            if kind == "bool" or kind.startswith("fixed"):
                v = tk.BooleanVar(value=bool(cfg["toggles"][key]))
                ttk.Checkbutton(tab, text=label, variable=v).grid(row=row, column=0, sticky="w", pady=2)
            else:
                choices = kind.split(":", 1)[1].split(",")
                v = tk.StringVar(value=str(cfg["toggles"][key]))
                ttk.Label(tab, text=label).grid(row=row, column=0, sticky="w", pady=2)
                ttk.Combobox(tab, textvariable=v, values=choices, state="readonly", width=10).grid(
                    row=row, column=1, sticky="w")
            vars_[key] = v
            row += 1

    def collect():
        new = {"format": FORMAT, "paths": {k: v.get() for k, v in pvars.items()}, "toggles": {}}
        for key, v in vars_.items():
            new["toggles"][key] = v.get()
        return merge_config(new)

    def do_copy():
        text = json.dumps(collect(), indent=2)
        root.clipboard_clear()
        root.clipboard_append(text)
        messagebox.showinfo("Copied", "Config copied to the clipboard.")

    def do_paste():
        try:
            data = json.loads(root.clipboard_get())
        except (tk.TclError, json.JSONDecodeError) as e:
            messagebox.showerror("Paste failed", f"Clipboard does not hold a config: {e}")
            return
        if data.get("format") != FORMAT:
            messagebox.showerror("Paste failed", f"Not a {FORMAT} config.")
            return
        c = merge_config(data)
        for k, v in c["paths"].items():
            pvars[k].set(v)
        for k, v in c["toggles"].items():
            if k in vars_:
                vars_[k].set(v)

    def do_save():
        p = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("JSON", "*.json")])
        if p:
            Path(p).write_text(json.dumps(collect(), indent=2))

    def do_load():
        p = filedialog.askopenfilename(filetypes=[("JSON", "*.json")])
        if not p:
            return
        c = merge_config(json.loads(Path(p).read_text()))
        for k, v in c["paths"].items():
            pvars[k].set(v)
        for k, v in c["toggles"].items():
            if k in vars_:
                vars_[k].set(v)

    def do_check():
        problems = check_paths(collect())
        messagebox.showinfo("Check", "\n".join(problems) if problems else "Ready to launch.")

    def do_launch():
        c = collect()
        problems = check_paths(c)
        if problems:
            messagebox.showerror("Not ready", "\n".join(problems))
            return
        proc = launch(c)
        messagebox.showinfo("Started", f"Game started.\nLog: {proc.log_path}")

    bar = ttk.Frame(root, padding=(8, 0, 8, 8))
    bar.pack(fill="x")
    for text, cmd in [
        ("Copy config", do_copy), ("Paste config", do_paste),
        ("Save...", do_save), ("Load...", do_load),
        ("Check paths", do_check), ("Launch", do_launch),
    ]:
        ttk.Button(bar, text=text, command=cmd).pack(side="left", padx=3)

    note = ("Windows uses the D3D12 renderer; Vulkan is Linux-only in ps3recomp. "
            "The Vulkan toggles (WCB, cube RT, pins, pacer, present mode) do nothing on Windows.")
    ttk.Label(root, text=note, foreground="#555", wraplength=720).pack(padx=8, pady=(0, 8))
    root.mainloop()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", help="load a config JSON (paste output of Copy config)")
    ap.add_argument("--print-config", action="store_true", help="print the effective config and exit")
    ap.add_argument("--show-env", action="store_true", help="print the env the game would get and exit")
    ap.add_argument("--headless", action="store_true", help="launch without the GUI")
    args = ap.parse_args(argv)

    cfg = default_config()
    if args.config:
        cfg = merge_config(json.loads(Path(args.config).read_text()))

    if args.print_config:
        print(json.dumps(cfg, indent=2))
        return 0
    if args.show_env:
        env = build_env(cfg, out_dir="<out>")
        for k in sorted(env):
            if k.startswith(("PS3", "SPU", "GCM", "VK_", "SPURS", "__GL")):
                print(f"{k}={env[k]}")
        return 0
    if args.headless:
        proc = launch(cfg)
        try:
            rc = proc.wait()
        finally:
            proc.log_file.close()
        print(f"[launcher] game exited rc={rc}; log: {proc.log_path}")
        return rc
    run_gui(cfg)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Closed-loop menu driver: classify the frame dump against reference screens, press that screen's button,
until the options screen is confirmed (the match then loads). Prints the path taken."""
import os, shutil, subprocess, sys, time
from PIL import Image, ImageChops, ImageStat

S = os.environ.get("MLB16_AB_DIR") or os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(S))
W = os.environ.get("MLB16_WORK") or os.path.join(os.path.dirname(REPO), "recomp-work")
# screen -> (button, region that tells it apart or None)
SCREENS = {
    "title": (0x0008, None), "online": (0x4000, None), "home_rtts": (0x0040, (50, 205, 195, 310)),
    "home_playnow": (0x4000, (50, 205, 195, 310)), "panel": (0x4000, None),
    "roster_live": (0x0040, (200, 140, 440, 180)), "roster_offline": (0x4000, (200, 140, 440, 180)),
    "team": (0x4000, None), "pitcher_select": (0x4000, None), "lineup": (0x0008, None), "stadium": (0x4000, None), "options": (0x4000, None),
}
REFDIR = os.environ.get("REF_DIR", f"{S}/ref")
_missing = [k for k in SCREENS if not os.path.isfile(f"{REFDIR}/{k}.png")]
if _missing:
    print("nav3: missing screen references in %s: %s" % (REFDIR, ", ".join(_missing)), file=sys.stderr)
    print("nav3: the references are game screenshots and are not committed; see SETUP.md", file=sys.stderr)
    sys.exit(2)
REF = {k: Image.open(f"{REFDIR}/{k}.png").convert("L").resize((640, 360)) for k in SCREENS}
# The pitch screen and its meter are not "press X" screens: X on the pitch
# selects the pitch (once, deliberately, from ab_live), and a second X starts
# the throw. Keep them out of the cinematic detector as well.
def _load_optional(name):
    try:
        return Image.open(f"{REFDIR}/{name}").convert("L").resize((640, 360))
    except Exception:
        return None
PITCH_WAIT = _load_optional("pitch_wait.png")
PITCH_METERS = []
for _n in sorted(os.listdir(REFDIR)) if os.path.isdir(REFDIR) else []:
    if _n.startswith("pitch_meter") and _n.endswith(".png"):
        _im = _load_optional(_n)
        if _im is not None:
            PITCH_METERS.append(_im)

# SKIP_CINE=1 (default): press X on a frame that matches no menu reference and
# is not the pitch/meter. The user verified every pregame cinematic (intro
# logos, team icons, matchup, dugout, flyover, batter view) is skippable with
# X, with a short gap between presses.
SKIP_CINE = os.environ.get("SKIP_CINE", "1") != "0"
CINE_GAP = float(os.environ.get("CINE_GAP", "1.6"))
# The dummy texture is magenta when VK_DUMMY_BLACK is unset, and the reference
# screens were recorded with black dummies. Ignore magenta pixels in both
# frames instead of re-recording every screen: the structure is unchanged and
# the dummy regions stop dominating the luma diff.
IGNORE_MAGENTA = os.environ.get("NAV_IGNORE_MAGENTA") == "1"

def magenta_mask(rgb):
    r, g, b = rgb.split()
    hi = lambda v: 255 if v > 100 else 0
    hr, hb = r.point(hi), b.point(hi)
    mn = ImageChops.darker(r, b)
    lowg = ImageChops.subtract(mn, g).point(lambda v: 255 if v > 40 else 0)
    mag = ImageChops.multiply(ImageChops.multiply(hr, hb), lowg)
    return ImageChops.invert(mag)          # 255 = compare, 0 = dummy pixel

def frame():
    for _ in range(5):
        try:
            shutil.copy(f"{W}/frame.ppm", f"{W}/nav3.ppm")
            rgb = Image.open(f"{W}/nav3.ppm").convert("RGB").resize((640, 360))
            return rgb.convert("L"), (magenta_mask(rgb) if IGNORE_MAGENTA else None)
        except Exception:
            time.sleep(0.3)

def diff(a, b, box=None, mask=None):
    if mask is not None:
        a, b = ImageChops.multiply(a, mask), ImageChops.multiply(b, mask)
    if box: a, b = a.crop(box), b.crop(box)
    return ImageStat.Stat(ImageChops.difference(a, b)).mean[0]

def classify(img, mask=None):
    if ImageStat.Stat(img).mean[0] < 3.0: return None
    d = sorted((diff(img, REF[k], mask=mask), k) for k in SCREENS)
    best, k = d[0]
    if best > 25: return None
    # the two home and roster states differ only in where the highlight is
    for pair in (("home_rtts", "home_playnow"), ("roster_live", "roster_offline")):
        if k in pair:
            box = SCREENS[k][1]
            k = min(pair, key=lambda p: diff(img, REF[p], box, mask))
    return k

def press(m):
    open(f"{W}/pad.txt", "w").write(f"0x{m:04X} 6\n")

def is_pitch_wait(img, mask=None):
    """The pitch screen with the pitch-selection menu (pre-X reference)."""
    return (img is not None and PITCH_WAIT is not None and
            diff(img, PITCH_WAIT, (380, 100, 500, 200), mask) < 25.0)

def is_pitch_meter(img, mask=None):
    """The meter is already up: never press X again (a second X throws)."""
    if img is None:
        return False
    for r in PITCH_METERS:
        if diff(img, r, (370, 95, 395, 120), mask) < 30.0:
            return True
    return False

def is_pitch_scene(img, mask=None):
    """The pitch camera, with or without the menu/meter. The field region is
    compared loosely to the meter reference: on the automated matchup the
    dugout/flyover/batter cinematics sit 77-150 away there, the settled pitch
    camera 9-35, so this stops the cinematic X presses one press before the
    meter. Without it an X can open the meter and the next one starts the
    throw."""
    if img is None:
        return False
    if is_pitch_wait(img, mask) or is_pitch_meter(img, mask):
        return True
    return (bool(PITCH_METERS) and
            diff(img, PITCH_METERS[0], (150, 210, 470, 330), mask) < 60.0)

def is_pitch(img, mask=None):
    return is_pitch_scene(img, mask)

def is_cinematic(img, mask, k):
    return SKIP_CINE and k is None and img is not None and not is_pitch(img, mask)

def skip_cine_step(img, mask, last_x, t0):
    """Press X once if this frame is a cinematic. Returns the new last-press time."""
    if is_cinematic(img, mask, None) and time.time() - last_x >= CINE_GAP:
        press(0x4000)
        print("nav3: cine:X at %.1fs" % (time.time() - t0), flush=True)
        time.sleep(1.0)
        return time.time()
    return last_x

HOME = os.environ.get("NAV_HOME") == "1"   # take the home team: the game then waits for our pitch
FAST = os.environ.get("NAV_FAST") == "1"
POLL = 1 if FAST else 2
SETTLE = 2 if FAST else 4
t0 = time.time(); last = None; same = 0; path = []; moved = False; last_x = 0.0

# --to-pitch: the pregame cinematics after the lineup screen (team icons,
# matchup, dugout, flyover, batter view) are all skippable with X. Loop until
# the pitch screen appears and stop there WITHOUT pressing X: the caller
# presses it exactly once to open the meter.
if "--to-pitch" in sys.argv:
    while time.time() - t0 < 180:
        fr = frame(); img, mask = fr if fr else (None, None)
        if is_pitch_meter(img, mask):
            print("nav3: pitch meter already up after %.1fs" % (time.time() - t0), flush=True)
            sys.exit(10)
        # The pitch camera (loose field-region match). Once it is seen, stop
        # pressing X: the next press belongs to ab_live and is exactly the one
        # that opens the meter. Waiting for a second confirmation is unsafe --
        # the camera transition flickers between matches and the meter can
        # appear inside that gap, so a later press would start the throw.
        if is_pitch_scene(img, mask):
            print("nav3: pitch reached after %.1fs" % (time.time() - t0), flush=True)
            sys.exit(0)
        last_x = skip_cine_step(img, mask, last_x, t0)
        time.sleep(POLL)
    print("nav3: --to-pitch timed out", flush=True)
    sys.exit(1)

while time.time() - t0 < 900:
    fr = frame(); img, mask = fr if fr else (None, None)
    k = classify(img, mask) if img else None
    if k is None:
        last_x = skip_cine_step(img, mask, last_x, t0)
        last = None; same = 0
        time.sleep(POLL)
        continue
    if k == last: same += 1
    else: same = 0; last = k
    if k and same >= 1:           # seen twice in a row: the screen has settled
        if k == "team" and HOME and not moved:
            press(0x0020); path.append("team:right"); moved = True; time.sleep(2 if FAST else 3); last = None; same = 0
            continue
        press(SCREENS[k][0]); path.append(k)
        if (k == "options" and not HOME) or k == "lineup":
            print("nav3:", " > ".join(path)); sys.exit(0)
        time.sleep(SETTLE); last = None; same = 0
        continue
    time.sleep(POLL)
print("nav3: timed out:", " > ".join(path)); sys.exit(1)

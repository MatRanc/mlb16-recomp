# same.py a.png b.png [x0,y0,x1,y1] [threshold]: exit 0 if the frames look alike in that box (640x360 coords).
# IGNORE_MAGENTA=1 makes the comparison ignore magenta pixels (a magenta dummy
# texture): both luma images are multiplied by a mask built from a.png.
import os, sys
from PIL import Image, ImageChops, ImageStat
box = tuple(int(v) for v in sys.argv[3].split(",")) if len(sys.argv) > 3 else (0, 0, 640, 360)
thresh = float(sys.argv[4]) if len(sys.argv) > 4 else 18.0
a, b = (Image.open(p).convert("RGB").resize((640, 360)) for p in sys.argv[1:3])
la, lb = a.convert("L"), b.convert("L")
if os.environ.get("IGNORE_MAGENTA") == "1":
    r, g, bl = a.split()
    hi = lambda v: 255 if v > 100 else 0
    hr, hb = r.point(hi), bl.point(hi)
    mn = ImageChops.darker(r, bl)
    lowg = ImageChops.subtract(mn, g).point(lambda v: 255 if v > 40 else 0)
    mask = ImageChops.invert(ImageChops.multiply(ImageChops.multiply(hr, hb), lowg))
    la, lb = ImageChops.multiply(la, mask), ImageChops.multiply(lb, mask)
la, lb = la.crop(box), lb.crop(box)
d = ImageStat.Stat(ImageChops.difference(la, lb)).mean[0]
print("diff %.1f" % d)
sys.exit(0 if d < thresh else 1)

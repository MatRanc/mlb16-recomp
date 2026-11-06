#!/bin/bash
# Save the current guest frame as PNG: $1 = name. Retries a half-written dump.
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$S/../.." && pwd)
W=${W:-${MLB16_WORK:-$(cd "$REPO/.." && pwd)/recomp-work}}
OUT=${SHOT_DIR:-$S}
dest="$1"
case "$dest" in
    /*) ;;
    *) dest="$OUT/$dest" ;;
esac
for i in 1 2 3 4 5; do
  cp "$W/frame.ppm" "$W/cur.ppm"
  python3 -c "from PIL import Image; Image.open('$W/cur.ppm').resize((640,360)).save('$dest')" 2>/dev/null && exit 0
  sleep 0.3
done
exit 1

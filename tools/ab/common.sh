# shellcheck shell=bash
# Shared defaults for the MLB 16 helper scripts. Source this file, do not run it.
#
# Defaults reproduce the user's existing layout: this repo and recomp-work are
# siblings, the decrypted EBOOT sits beside them, the ISO is mounted where the
# disc auto-mounts on Fedora, and the ps3recomp checkout is ps3recomp-play.
# Override any of them in the environment for another machine.

MLB16_AB_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MLB16_REPO_DIR=$(cd "$MLB16_AB_DIR/../.." && pwd)
MLB16_WORK=${MLB16_WORK:-$(cd "$MLB16_REPO_DIR/.." && pwd)/recomp-work}
MLB16_ROOT=${MLB16_ROOT:-$(cd "$MLB16_WORK/.." && pwd)}
MLB16_EBOOT=${MLB16_EBOOT:-$MLB16_ROOT/EBOOT.elf}
MLB16_ISO_MOUNT=${MLB16_ISO_MOUNT:-/run/media/m/PS3VOLUME/PS3_GAME}
MLB16_PS3RECOMP=${MLB16_PS3RECOMP:-${PS3RECOMP_DIR:-$MLB16_ROOT/ps3recomp-play}}
MLB16_BUILD=${MLB16_BUILD:-${BUILD:-build-play}}
MLB16_BIN=${MLB16_BIN:-MyGameRecomp}

W=$MLB16_WORK
S=$MLB16_AB_DIR

export MLB16_WORK MLB16_ROOT MLB16_EBOOT MLB16_ISO_MOUNT MLB16_PS3RECOMP
export MLB16_BUILD MLB16_BIN

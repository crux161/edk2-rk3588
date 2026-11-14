#!/bin/sh
# Bring the Kumo Continuum renderer into NimbusSplashDxe (PROPOSAL/024 KS2).
#
# Builds libkumo_splash_ffi.a in the KUMO tree with that tree's own script (which refuses any
# x18 use and any symbol the firmware would have to supply) and copies the archive and its
# header next to the driver. Both copies are build products and are ignored by git.
#
# KUMO_ROOT selects the KUMO checkout (default ~/git/kumo).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
kumo=${KUMO_ROOT:-$HOME/git/kumo}
dest="$here/edk2-rockchip/NimbusPkg/Drivers/NimbusSplashDxe/Lib"
[ -x "$kumo/lib/kumo-splash-ffi/build-firmware-lib.sh" ] || {
  echo "kumo-splash-lib: no lib/kumo-splash-ffi in $kumo" >&2
  echo "kumo-splash-lib: export KUMO_ROOT=<a KUMO checkout that has lib/kumo-splash-ffi>." >&2
  echo "kumo-splash-lib: Until KS1/KS2 are merged, that is only the session worktree:" >&2
  echo "kumo-splash-lib:   $HOME/git/kumo/.claude/worktrees/self-hosting-toolchain-review-3800a8" >&2
  echo "kumo-splash-lib: Or build with NIMBUS_SPLASH=FALSE for the stock logo." >&2
  exit 1
}
out=$(mktemp -d "${TMPDIR:-/tmp}/kumo-splash-fw.XXXXXX")
"$kumo/lib/kumo-splash-ffi/build-firmware-lib.sh" "$out" | sed 's/^/kumo-splash-lib: /'
mkdir -p "$dest"
cp "$out/libkumo_splash_ffi.a" "$out/kumo_splash.h" "$dest/"
echo "kumo-splash-lib: installed into $dest"

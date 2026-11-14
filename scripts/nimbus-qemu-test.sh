#!/opt/homebrew/bin/bash
# Watch NimbusSplashDxe run in QEMU before flashing anything (PROPOSAL/024 KS2).
#
# Builds NimbusPkg/NimbusPkg.dsc (the driver plus a kick app, generic AArch64 libraries, debug
# output on the QEMU virt PL011), boots QEMU's stock edk2 with a ramfb display, loads the driver
# from the shell, and lets the kick app start it. scripts/nimbus-qemu-run.py captures screendumps
# at fixed offsets after the splash epoch, then after the kick app's line of console text, which
# must end the splash. Results land in OUT (default workspace/nimbus-qemu).
set -euo pipefail
ROOTDIR=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOTDIR/workspace/nimbus-qemu}
export WORKSPACE="$ROOTDIR/workspace"
cd "$ROOTDIR"

[ -f edk2-rockchip/NimbusPkg/Drivers/NimbusSplashDxe/Lib/libkumo_splash_ffi.a ] || scripts/kumo-splash-lib.sh

export PACKAGES_PATH="$ROOTDIR:$ROOTDIR/edk2:$ROOTDIR/edk2-platforms:$ROOTDIR/edk2-rockchip"
export CLANGDWARF_BIN="$(brew --prefix llvm)/bin/"
export CLANG_HOST_BIN="$WORKSPACE/host-bin/"
set +u
source "$ROOTDIR/edk2/edksetup.sh" >/dev/null
set -u
build -s -n 0 -a AARCH64 -t CLANGDWARF -b DEBUG -p "$ROOTDIR/edk2-rockchip/NimbusPkg/NimbusPkg.dsc" >"$WORKSPACE/nimbus-build.log" 2>&1 \
  || { tail -30 "$WORKSPACE/nimbus-build.log"; exit 1; }

BIN="$WORKSPACE/Build/NimbusPkg/DEBUG_CLANGDWARF/AARCH64"
rm -rf "$OUT"; mkdir -p "$OUT/esp"
cp "$BIN/NimbusSplashDxe.efi" "$BIN/NimbusSplashKick.efi" "$OUT/esp/"
printf 'fs0:\r\nload NimbusSplashDxe.efi\r\nNimbusSplashKick.efi\r\n' >"$OUT/esp/startup.nsh"
python3 "$ROOTDIR/scripts/nimbus-qemu-run.py" "$OUT"

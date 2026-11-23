#!/bin/sh
# Build u-boot.bin for the Raspberry Pi 5 family (nextbsd/nextbsd#687).
#
#   sh u-boot/build-u-boot.sh <workdir> <outdir>
#
# Needs git, make, an aarch64 cross gcc (CROSS_COMPILE, default
# aarch64-linux-gnu-) and U-Boot's host build dependencies. Sources and patch
# hashes are pinned in SOURCES; the build stops if any of them differ.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:?usage: build-u-boot.sh <workdir> <outdir>}
OUT=${2:?usage: build-u-boot.sh <workdir> <outdir>}
CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}
export CROSS_COMPILE

. "$HERE/SOURCES"

mkdir -p "$WORK" "$OUT"
WORK=$(cd "$WORK" && pwd)
OUT=$(cd "$OUT" && pwd)
SRC="$WORK/u-boot"
test ! -e "$SRC" || { echo "ERROR: $SRC already exists; use an empty workdir" >&2; exit 1; }

sha256() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1 || shasum -a 256 "$1" | cut -d' ' -f1; }

echo "==> U-Boot $UBOOT_TAG from $UBOOT_GIT"
git -c advice.detachedHead=false clone -q --depth 1 --branch "$UBOOT_TAG" "$UBOOT_GIT" "$SRC"
GOT=$(git -C "$SRC" rev-parse HEAD)
test "$GOT" = "$UBOOT_COMMIT" ||
    { echo "ERROR: $UBOOT_TAG is $GOT, expected $UBOOT_COMMIT" >&2; exit 1; }
echo "    commit $GOT"

# Reproducible timestamps: the release commit's date, not the build's.
SOURCE_DATE_EPOCH=$(git -C "$SRC" log -1 --format=%ct HEAD)
export SOURCE_DATE_EPOCH

echo "==> patches"
echo "$UBOOT_PATCHES" | while read -r f sum strip; do
    [ -n "$f" ] || continue
    p="$HERE/patches/$f"
    got=$(sha256 "$p")
    test "$got" = "$sum" ||
        { echo "ERROR: $f sha256 $got, expected $sum" >&2; exit 1; }
    git -C "$SRC" apply -p"$strip" "$p"
    echo "    applied $f ($sum)"
done

# Check the series as a series: what the tree says after all of it.
grep -q 'while (parent && !dev_has_ofnode(parent))' "$SRC/drivers/core/device.c" ||
    { echo "ERROR: 0001 is not in drivers/core/device.c" >&2; exit 1; }
grep -qx 'CONFIG_NVME_PCI=y' "$SRC/configs/$UBOOT_DEFCONFIG" ||
    { echo "ERROR: 0002 is not in $UBOOT_DEFCONFIG" >&2; exit 1; }
grep -qx 'boot_targets=mmc usb nvme pxe dhcp' "$SRC/board/raspberrypi/rpi/rpi.env" ||
    { echo "ERROR: 0003 is not in rpi.env" >&2; exit 1; }
grep -q 'EFI_TIMER_PERIODIC, 100\*1000\*10' "$SRC/lib/efi_loader/efi_console.c" ||
    { echo "ERROR: the FreeBSD console patch is not in efi_console.c" >&2; exit 1; }

echo "==> config: $UBOOT_DEFCONFIG + nextbsd_rpi_arm64_fragment"
cat "$SRC/configs/$UBOOT_DEFCONFIG" "$HERE/nextbsd_rpi_arm64_fragment" \
    > "$SRC/configs/nextbsd_rpi_arm64_defconfig"
make -C "$SRC" -s nextbsd_rpi_arm64_defconfig

# Every setting in the fragment must have survived Kconfig (an unknown or
# dependency-blocked symbol is dropped silently).
grep -E '^(CONFIG_|# CONFIG_.* is not set$)' "$HERE/nextbsd_rpi_arm64_fragment" |
while IFS= read -r line; do
    grep -qxF "$line" "$SRC/.config" ||
        { echo "ERROR: not in .config: $line" >&2; exit 1; }
done
echo "    fragment settings all present in .config"

echo "==> build"
make -C "$SRC" -s -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" LOCALVERSION= all
make -C "$SRC" -s LOCALVERSION= u-boot-initial-env

test -s "$SRC/u-boot.bin" || { echo "ERROR: no u-boot.bin" >&2; exit 1; }
ENVF="$SRC/u-boot-initial-env"
# The built-in environment is what the board will actually run.
for want in 'bootdelay=-2' 'boot_targets=mmc usb nvme pxe dhcp'; do
    grep -qxF "$want" "$ENVF" || { echo "ERROR: env lacks $want" >&2; exit 1; }
done
grep -q '^preboot=.*nvme scan' "$ENVF" || { echo "ERROR: preboot has no nvme scan" >&2; exit 1; }
grep -q '^bootcmd=.*bootefi .*; reset$' "$ENVF" || { echo "ERROR: bootcmd does not end in reset" >&2; exit 1; }
echo "    uboot.release: $(cat "$SRC/include/config/uboot.release" 2>/dev/null || echo '?')"
BANNER=$(strings -a "$SRC/u-boot.bin" | grep -m1 -F 'U-Boot 2026.10-nextbsd1' || true)
if [ -z "$BANNER" ]; then
    echo "ERROR: no 'U-Boot 2026.10-nextbsd1' banner in u-boot.bin; U-Boot strings found:" >&2
    strings -a "$SRC/u-boot.bin" | grep -m5 'U-Boot 20' >&2 || true
    exit 1
fi

cp "$SRC/u-boot.bin" "$OUT/u-boot.bin"
cp "$SRC/.config" "$OUT/u-boot.config"
cp "$ENVF" "$OUT/u-boot-initial-env"
{
    echo "u-boot: $UBOOT_GIT $UBOOT_TAG $UBOOT_COMMIT"
    echo "defconfig: $UBOOT_DEFCONFIG + nextbsd_rpi_arm64_fragment"
    echo "$UBOOT_PATCHES" | while read -r f sum strip; do
        [ -n "$f" ] && echo "patch: $f sha256=$sum"
    done
    echo "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"
    echo "banner: $BANNER"
    echo "cc: $(${CROSS_COMPILE}gcc --version | head -1)"
    echo "u-boot.bin sha256=$(sha256 "$OUT/u-boot.bin") size=$(wc -c < "$OUT/u-boot.bin" | tr -d ' ')"
} > "$OUT/BUILDINFO"
cat "$OUT/BUILDINFO"
grep -E '^(bootcmd|bootdelay|preboot|boot_targets|bootretry)=' "$ENVF" | sed 's/^/env: /'

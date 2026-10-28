#!/bin/sh
# kexts.sh — build this repo's kexts from the data in their directories.
#
# Every top-level directory holding a kext.env is one kext, named after the
# directory (E21 P4, nextbsd/nextbsd#556). Nothing about a kext is written into
# the workflow: where its source is, which module it builds from and what it
# bundles are all here, so moving a module's source (E22) changes only its
# kext.env. kext.env is a sh fragment that sets:
#
#   KEXT_NAME           the bundle name, <KEXT_NAME>.kext; must match the directory
#   KEXT_ID             CFBundleIdentifier
#   KEXT_ARCHES         the arches it ships on, e.g. "amd64" or "amd64 arm64"
#   KEXT_MODULE         the sys/modules/<module> directory it is built from. The
#                       build's MODULES_OVERRIDE is the list of these, so modules
#                       compiled into the kernel, or not shipped as a kext, are
#                       never built.
#   KEXT_KO             the .ko that module produces
#   KEXT_PERSONALITIES  optional: "<generator in tools/> <path in the source tree>".
#                       The generator writes the IOKitPersonalities (device-id
#                       match table) from the driver's own id tables.
#   KEXT_FIRMWARE       optional: "<linux-firmware directory> [<licence file>]",
#                       bundled flat into Contents/Resources/firmware, from
#                       linux-firmware at $LINUX_FIRMWARE_TAG.
#
# usage:
#   kexts.sh modules <arch>
#       print the MODULES_OVERRIDE list for <arch>
#   kexts.sh inputs <arch> <srcdir> <objdir> <out>
#       copy each kext's .ko from the obj tree, and generate its personalities
#       from the source tree, into <out>/<KEXT_NAME>/ (runs where the kernel
#       source is, in the toolchain container)
#   kexts.sh wrap <arch> <inputs> <out>
#       wrap each into <out>/<KEXT_NAME>.kext with its firmware, and stamp every
#       kext with <inputs>/KERNEL-BUILD-ID (nextbsd-kernel#326)
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$HERE")

die() { echo "ERROR: $*" >&2; exit 1; }

# load <dir>: read <dir>/kext.env into KEXT_* and check it.
load() {
	KEXT_NAME= KEXT_ID= KEXT_ARCHES= KEXT_MODULE= KEXT_KO= KEXT_PERSONALITIES= KEXT_FIRMWARE=
	. "$1/kext.env"
	for _v in KEXT_NAME KEXT_ID KEXT_ARCHES KEXT_MODULE KEXT_KO; do
		eval "_x=\${$_v}"
		[ -n "$_x" ] || die "$1/kext.env: $_v is not set"
	done
	[ "$(basename "$1")" = "$KEXT_NAME" ] || die "$1/kext.env: KEXT_NAME $KEXT_NAME does not match its directory"
}

# kexts <arch>: the kext directories that ship on <arch>.
kexts() {
	for _f in "$ROOT"/*/kext.env; do
		[ -f "$_f" ] || continue
		_d=$(dirname "$_f")
		( load "$_d"; case " $KEXT_ARCHES " in *" $1 "*) echo "$_d" ;; esac )
	done
}

cmd=${1:-}; [ $# -ge 2 ] || die "usage: kexts.sh modules|inputs|wrap <arch> ..."
arch=$2
dirs=$(kexts "$arch")
[ -n "$dirs" ] || die "no kext ships on $arch"

case "$cmd" in
modules)
	for d in $dirs; do ( load "$d"; echo "$KEXT_MODULE" ); done | sort -u | tr '\n' ' ' | sed 's/ $//'
	echo
	;;
inputs)
	[ $# -eq 5 ] || die "usage: kexts.sh inputs <arch> <srcdir> <objdir> <out>"
	src=$3; obj=$4; out=$5
	for d in $dirs; do
		(
		load "$d"
		mkdir -p "$out/$KEXT_NAME"
		ko=$(find "$obj" -type f -name "$KEXT_KO" -path '*/modules/*' | head -1)
		[ -n "$ko" ] || die "$KEXT_NAME: $KEXT_KO not built (module $KEXT_MODULE)"
		cp "$ko" "$out/$KEXT_NAME/"
		if [ -n "$KEXT_PERSONALITIES" ]; then
			set -- $KEXT_PERSONALITIES
			[ -e "$src/$2" ] || die "$KEXT_NAME: $2 not in the source tree"
			sh "$HERE/$1" "$src/$2" "$KEXT_ID" "$KEXT_NAME" > "$out/$KEXT_NAME/$KEXT_NAME.iokitpersonalities"
		fi
		echo "$KEXT_NAME: $(ls "$out/$KEXT_NAME" | tr '\n' ' ')"
		)
	done
	;;
wrap)
	[ $# -eq 4 ] || die "usage: kexts.sh wrap <arch> <inputs> <out>"
	in=$3; out=$4
	kbid=$(cat "$in/KERNEL-BUILD-ID") || die "no $in/KERNEL-BUILD-ID"
	[ -n "$kbid" ] || die "$in/KERNEL-BUILD-ID is empty"
	mkdir -p "$out"
	# One sparse linux-firmware checkout for every kext that bundles firmware.
	fw_paths=$(for d in $dirs; do ( load "$d"; [ -z "$KEXT_FIRMWARE" ] || echo "$KEXT_FIRMWARE" ); done | tr '\n' ' ')
	lf=$(mktemp -d)
	if [ -n "$(echo $fw_paths)" ]; then
		: "${LINUX_FIRMWARE_TAG:?LINUX_FIRMWARE_TAG is not set}"
		git clone -q --depth 1 --branch "$LINUX_FIRMWARE_TAG" --filter=blob:none --sparse \
			https://gitlab.com/kernel-firmware/linux-firmware.git "$lf/lf"
		( cd "$lf/lf" && git sparse-checkout set --no-cone $fw_paths )
	fi
	for d in $dirs; do
		(
		load "$d"
		set --
		if [ -n "$KEXT_PERSONALITIES" ]; then
			pers="$in/$KEXT_NAME/$KEXT_NAME.iokitpersonalities"
			[ -s "$pers" ] || die "$KEXT_NAME: no personalities in $pers"
			set -- "$@" -p "$pers"
		fi
		if [ -n "$KEXT_FIRMWARE" ]; then
			fwdir=${KEXT_FIRMWARE%% *}
			lic=; [ "$fwdir" = "$KEXT_FIRMWARE" ] || lic=${KEXT_FIRMWARE#* }
			[ -d "$lf/lf/$fwdir" ] || die "$KEXT_NAME: linux-firmware $LINUX_FIRMWARE_TAG has no $fwdir"
			# A private copy, so the licence can go in beside the blobs.
			fw="$lf/fw-$KEXT_NAME"
			cp -RL "$lf/lf/$fwdir" "$fw"
			[ -z "$lic" ] || cp -L "$lf/lf/$lic" "$fw/"
			set -- "$@" -f "$fw"
		fi
		sh "$HERE/ko2kext.sh" -o "$out" "$@" "$in/$KEXT_NAME/$KEXT_KO" "$KEXT_NAME" "$KEXT_ID"
		echo "==> $KEXT_NAME.kext: $(du -sh "$out/$KEXT_NAME.kext" | cut -f1)$([ -z "$KEXT_FIRMWARE" ] || echo ", $(find "$out/$KEXT_NAME.kext/Contents/Resources/firmware" -type f | wc -l | tr -d ' ') firmware files")"
		)
	done
	python3 "$HERE/stamp-kernel-build-id.py" "$kbid" "$out"
	;;
*)
	die "unknown command $cmd"
	;;
esac

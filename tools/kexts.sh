#!/bin/sh
# kexts.sh — build this repo's kexts from the data in their directories.
#
# Every top-level directory holding a kext.env is one kext, named after the
# directory (E21 P4, nextbsd-kernel-extensions#3). Nothing about a kext is
# written into the workflow: where its source is, which module it builds from
# and what it bundles are all here, so moving a module's source (E22) changes
# only its kext.env. The directories are seeded by tools/gen-kext-dirs.py from
# data/ and then edited by hand. kext.env is a sh fragment that sets:
#
#   KEXT_NAME           the bundle name, <KEXT_NAME>.kext; must match the directory
#   KEXT_ID             CFBundleIdentifier (E21 D7: org.nextbsd.<group>.<Name>)
#   KEXT_ARCHES         the arches it ships on, e.g. "amd64" or "amd64 arm64"
#   KEXT_MODULE         the sys/modules/<module> directory it is built from (the
#                       build's MODULES_OVERRIDE is the list of these), or
#                       "local": built from this directory's own Makefile (the
#                       net80211 family, the vendored Realtek driver)
#   KEXT_KO             the .ko that module produces
#   KEXT_CATEGORY       the inventory category (hwdriver, netgraph, ...)
#   KEXT_MATCH          "required" for a hardware driver: it must carry a
#                       personality the kernel matches (E21 D12), or the build
#                       fails; "none" for libraries and request-loaded kexts
#   KEXT_PERSONALITIES  optional: "<generator in tools/> <path in the source
#                       tree>", a hand-written generator used instead of the
#                       module's own PNP metadata
#   KEXT_FIRMWARE       optional: "<linux-firmware directory> [<licence file>]",
#                       bundled flat into Contents/Resources/firmware, from
#                       linux-firmware at $LINUX_FIRMWARE_TAG
#
# Personalities and OSBundleLibraries otherwise come from the module itself:
# `inputs` records each .ko's metadata with kldxref, and `wrap` turns it into
# Info.plist entries with tools/kext-meta.py (E21 #4).
#
# usage:
#   kexts.sh modules <arch>
#       print the MODULES_OVERRIDE list for <arch> (sys/modules kexts)
#   kexts.sh local <arch>
#       print the directories built from their own Makefile for <arch>
#   kexts.sh inputs <arch> <srcdir> <objdir> <out>
#       copy each kext's .ko from the obj tree, record its metadata with
#       $KLDXREF (when set), and run any hand-written personality generator,
#       into <out>/<KEXT_NAME>/ (in the toolchain container)
#   kexts.sh wrap <arch> <inputs> <out>
#       wrap each into <out>/<KEXT_NAME>.kext with its personalities,
#       dependencies and firmware, and stamp every kext with
#       <inputs>/KERNEL-BUILD-ID (nextbsd-kernel#326)
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$HERE")

die() { echo "ERROR: $*" >&2; exit 1; }

# load <dir>: read <dir>/kext.env into KEXT_* and check it.
load() {
	KEXT_NAME= KEXT_ID= KEXT_ARCHES= KEXT_MODULE= KEXT_KO= KEXT_CATEGORY=
	KEXT_MATCH=none KEXT_PERSONALITIES= KEXT_FIRMWARE=
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

cmd=${1:-}; [ $# -ge 2 ] || die "usage: kexts.sh modules|local|inputs|wrap <arch> ..."
arch=$2
dirs=$(kexts "$arch")
[ -n "$dirs" ] || die "no kext ships on $arch"

case "$cmd" in
modules)
	for d in $dirs; do ( load "$d"; [ "$KEXT_MODULE" = local ] || echo "$KEXT_MODULE" ); done |
	    sort -u | tr '\n' ' ' | sed 's/ $//'
	echo
	;;
local)
	for d in $dirs; do ( load "$d"; [ "$KEXT_MODULE" != local ] || echo "$d" ); done
	;;
inputs)
	[ $# -eq 5 ] || die "usage: kexts.sh inputs <arch> <srcdir> <objdir> <out>"
	src=$3; obj=$4; out=$5
	missing=$(mktemp)
	for d in $dirs; do
		(
		load "$d"
		if [ "$KEXT_MODULE" = local ]; then
			# An out-of-tree build leaves its objects beside the source.
			ko=$(find "$d" "$obj" -type f -name "$KEXT_KO" -path "*/$KEXT_NAME/*" 2>/dev/null | head -1)
		else
			ko=$(find "$obj" -type f -name "$KEXT_KO" -path "*/modules/$KEXT_MODULE/*" | head -1)
			[ -n "$ko" ] || ko=$(find "$obj" -type f -name "$KEXT_KO" -path '*/modules/*' | head -1)
		fi
		if [ -z "$ko" ]; then
			echo "$KEXT_NAME ($KEXT_MODULE/$KEXT_KO)" >> "$missing"
			exit 0
		fi
		mkdir -p "$out/$KEXT_NAME"
		cp "$ko" "$out/$KEXT_NAME/"
		echo "$KEXT_ID" > "$out/$KEXT_NAME/kext.id"
		if [ -n "${KLDXREF:-}" ]; then
			"$KLDXREF" -d -v -v "$out/$KEXT_NAME/$KEXT_KO" > "$out/$KEXT_NAME/$KEXT_NAME.kldxref" 2>&1 ||
			    die "$KEXT_NAME: kldxref failed"
		fi
		if [ -n "$KEXT_PERSONALITIES" ]; then
			set -- $KEXT_PERSONALITIES
			[ -e "$src/$2" ] || die "$KEXT_NAME: $2 not in the source tree"
			sh "$HERE/$1" "$src/$2" "$KEXT_ID" "$KEXT_NAME" > "$out/$KEXT_NAME/$KEXT_NAME.iokitpersonalities"
		fi
		)
	done
	if [ -s "$missing" ]; then
		echo "ERROR: $(wc -l < "$missing" | tr -d ' ') kext module(s) were not built for $arch:" >&2
		sed 's/^/  /' "$missing" >&2
		exit 1
	fi
	echo "inputs: $(ls "$out" | wc -l | tr -d ' ') kexts for $arch"
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
	nomatch=$(mktemp)
	for d in $dirs; do
		(
		load "$d"
		set --
		pers="$in/$KEXT_NAME/$KEXT_NAME.iokitpersonalities"
		libs=""
		if [ -f "$in/$KEXT_NAME/$KEXT_NAME.kldxref" ]; then
			# Generated: dependencies always, personalities unless a
			# hand-written generator already produced them.
			gen="$in/$KEXT_NAME/$KEXT_NAME.generated"
			libs=$(python3 "$HERE/kext-meta.py" "$in" "$KEXT_NAME" "$gen")
			[ -s "$pers" ] || { [ ! -s "$gen" ] || cp "$gen" "$pers"; }
		fi
		for l in $libs; do set -- "$@" -l "$l"; done
		if [ -s "$pers" ]; then
			set -- "$@" -p "$pers"
		elif [ "$KEXT_MATCH" = required ]; then
			echo "$KEXT_NAME ($KEXT_KO)" >> "$nomatch"
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
		sh "$HERE/ko2kext.sh" -o "$out" "$@" "$in/$KEXT_NAME/$KEXT_KO" "$KEXT_NAME" "$KEXT_ID" > /dev/null
		)
	done
	# E21 D12: a hardware driver ships as a kext only if it autoloads.
	if [ -s "$nomatch" ]; then
		echo "ERROR: $(wc -l < "$nomatch" | tr -d ' ') hardware kext(s) on $arch carry no personality the kernel can match (E21 D12):" >&2
		sed 's/^/  /' "$nomatch" >&2
		echo "  Add a personality, or hold the driver back in data/decisions.tsv." >&2
		exit 1
	fi
	echo "wrap: $(ls -d "$out"/*.kext | wc -l | tr -d ' ') kexts for $arch"
	python3 "$HERE/stamp-kernel-build-id.py" "$kbid" "$out"
	;;
*)
	die "unknown command $cmd"
	;;
esac

#!/bin/sh
# gen-geom-personalities.sh — emit an IOKitPersonalities block that loads a
# GEOM layer kext when a disk carrying its metadata appears (nextbsd-kernel#364).
#
# Darwin loads CoreStorage and AppleRAID from IOKit matching on the partition:
# IOProviderClass IOMedia, with the media's content in "Content Mask". NextBSD's
# kernel publishes a GEOM provider's content to the same matcher (the IOMEDIA
# GEOM class): the GEOM metadata magic in the provider's last sector, which is
# what the layer's own taste looks for, or a GPT partition's type GUID.
# FreeBSD's layers have no partition type of their own, so the personality
# names the magic, read here straight from the class header being compiled:
#
#     #define G_ELI_MAGIC "GEOM::ELI"   ->   Content Mask "GEOM::ELI"
#
# usage: gen-geom-personalities.sh <g_<class>.h> <bundle-id> <ioclass>
# Output (stdout): a complete <key>IOKitPersonalities</key><dict>...</dict>
# block for ko2kext.sh -p.
set -eu

[ $# -eq 3 ] || { echo "usage: gen-geom-personalities.sh <g_<class>.h> <bundle-id> <ioclass>" >&2; exit 1; }
SRC="$1"; BUNDLE_ID="$2"; IOCLASS="$3"
[ -f "$SRC" ] || { echo "gen-geom-personalities: no such file: $SRC" >&2; exit 1; }

# The one string-valued *_MAGIC define that names a GEOM class ("GEOM::<NAME>").
MAGIC=$(sed -n 's/^#define[[:space:]]*G_[A-Z0-9_]*_MAGIC[[:space:]]*"\(GEOM::[A-Z0-9]*\)".*/\1/p' "$SRC")
[ "$(printf '%s\n' "$MAGIC" | grep -c .)" -eq 1 ] ||
	{ echo "gen-geom-personalities: expected one GEOM magic in $SRC, found: ${MAGIC:-none}" >&2; exit 1; }

cat <<PLIST
	<key>IOKitPersonalities</key>
	<dict>
		<key>${IOCLASS}</key>
		<dict>
			<key>CFBundleIdentifier</key>
			<string>${BUNDLE_ID}</string>
			<key>IOClass</key>
			<string>${IOCLASS}</string>
			<key>IOProviderClass</key>
			<string>IOMedia</string>
			<key>Content Mask</key>
			<string>${MAGIC}</string>
			<key>IOProbeScore</key>
			<integer>1000</integer>
		</dict>
	</dict>
PLIST

echo "gen-geom-personalities: ${IOCLASS} matches IOMedia content ${MAGIC} ($(basename "$SRC"))" >&2

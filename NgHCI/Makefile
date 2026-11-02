# ng_hci, the Bluetooth HCI layer: sys/modules/netgraph/bluetooth/hci, its
# sources from freebsd-src.
#
# NextBSD changes (src/): each ng_hci_*.c named in NEXTBSD_PATCHED is built
# from a patched copy, <name>_nextbsd.c, generated in the obj directory by
# applying src/patches/NNNN-<name>-*.patch in order; the source tree is not
# touched. The patches call into NextBSD's own files in src/
# (nextbsd/nextbsd-kernel#234: LE hardening, with ng_hci_le_lp.h shared with
# NgL2cap; nextbsd/nextbsd-kernel#237: SCO and eSCO connections, with
# ng_hci_sync_lp.h shared with NgBtsocket).
SRCTOP?=	/usr/src
.PATH: ${SRCTOP}/sys/netgraph/bluetooth/hci ${.CURDIR}/src

CFLAGS+=	-I${SRCTOP}/sys/netgraph/bluetooth/include \
		-I${SRCTOP}/sys/netgraph/bluetooth/hci \
		-I${.CURDIR}/src

KMOD=		ng_hci
# sys/modules/netgraph/bluetooth/hci, every file patched (<name>_nextbsd.c)
NEXTBSD_PATCHED=	ng_hci_main ng_hci_cmds ng_hci_evnt ng_hci_ulpi \
			ng_hci_misc
SRCS=		${NEXTBSD_PATCHED:S/$/_nextbsd.c/}
# NextBSD: the LE buffer pool, LE_Connection_Update and the LE meta events
SRCS+=		ng_hci_le.c
# NextBSD: SCO and eSCO connections, Synchronous_Connection_Complete/_Changed
SRCS+=		ng_hci_sync.c

# Each <name>.c with its src/patches applied. patch(1) is given the file to
# patch, so the paths in the patches are only documentation; a patch that no
# longer applies fails the build.
.for f in ${NEXTBSD_PATCHED}
NEXTBSD_PATCHES.${f}!=	ls ${.CURDIR}/src/patches/[0-9][0-9][0-9][0-9]-${f}-*.patch
${f}_nextbsd.c: ${SRCTOP}/sys/netgraph/bluetooth/hci/${f}.c ${NEXTBSD_PATCHES.${f}}
	cp ${SRCTOP}/sys/netgraph/bluetooth/hci/${f}.c ${.TARGET}.tmp
.for p in ${NEXTBSD_PATCHES.${f}}
	patch -s ${.TARGET}.tmp < ${p}
.endfor
	rm -f ${.TARGET}.tmp.orig
	mv ${.TARGET}.tmp ${.TARGET}
CLEANFILES+=	${f}_nextbsd.c ${f}_nextbsd.c.tmp ${f}_nextbsd.c.tmp.orig
.endfor

.include <bsd.kmod.mk>

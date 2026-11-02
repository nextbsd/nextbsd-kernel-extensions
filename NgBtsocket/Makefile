# ng_btsocket, the Bluetooth sockets (HCI raw, L2CAP, RFCOMM, SCO):
# sys/modules/netgraph/bluetooth/socket, its sources from freebsd-src.
#
# NextBSD changes (src/): each ng_btsocket_*.c named in NEXTBSD_PATCHED is
# built from a patched copy, <name>_nextbsd.c, generated in the obj directory
# by applying src/patches/NNNN-<name>-*.patch in order; the source tree is not
# touched. The patches call into NextBSD's own files in src/
# (nextbsd/nextbsd-kernel#237: SCO and eSCO parameters on SCO sockets).
# ng_hci_sync_lp.h, the messages ng_btsocket_sco exchanges with ng_hci, comes
# from NgHCI's src/.
SRCTOP?=	/usr/src
.PATH: ${SRCTOP}/sys/netgraph/bluetooth/socket ${.CURDIR}/src

CFLAGS+=	-I${SRCTOP}/sys/netgraph/bluetooth/include \
		-I${.CURDIR}/src -I${.CURDIR}/../NgHCI/src

KMOD=		ng_btsocket
# sys/modules/netgraph/bluetooth/socket, with these patched (<name>_nextbsd.c)
NEXTBSD_PATCHED=	ng_btsocket_sco
SRCS=		ng_btsocket.c \
		ng_btsocket_hci_raw.c \
		ng_btsocket_l2cap_raw.c \
		ng_btsocket_l2cap.c \
		ng_btsocket_rfcomm.c \
		${NEXTBSD_PATCHED:S/$/_nextbsd.c/}
# NextBSD: SO_SCO_SYNC_PARAMS and SO_SCO_SYNC_INFO
SRCS+=		ng_btsocket_sco_sync.c

EXPORT_SYMS=	YES

# Each <name>.c with its src/patches applied. patch(1) is given the file to
# patch, so the paths in the patches are only documentation; a patch that no
# longer applies fails the build.
.for f in ${NEXTBSD_PATCHED}
NEXTBSD_PATCHES.${f}!=	ls ${.CURDIR}/src/patches/[0-9][0-9][0-9][0-9]-${f}-*.patch
${f}_nextbsd.c: ${SRCTOP}/sys/netgraph/bluetooth/socket/${f}.c ${NEXTBSD_PATCHES.${f}}
	cp ${SRCTOP}/sys/netgraph/bluetooth/socket/${f}.c ${.TARGET}.tmp
.for p in ${NEXTBSD_PATCHES.${f}}
	patch -s ${.TARGET}.tmp < ${p}
.endfor
	rm -f ${.TARGET}.tmp.orig
	mv ${.TARGET}.tmp ${.TARGET}
CLEANFILES+=	${f}_nextbsd.c ${f}_nextbsd.c.tmp ${f}_nextbsd.c.tmp.orig
.endfor

.include <bsd.kmod.mk>

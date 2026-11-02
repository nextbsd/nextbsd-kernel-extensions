# ng_l2cap, the Bluetooth L2CAP layer: sys/modules/netgraph/bluetooth/l2cap,
# its sources from freebsd-src.
#
# NextBSD changes (src/): each ng_l2cap_*.c named in NEXTBSD_PATCHED is built
# from a patched copy, <name>_nextbsd.c, generated in the obj directory by
# applying src/patches/NNNN-<name>-*.patch in order; the source tree is not
# touched. The patches call into NextBSD's own files in src/
# (nextbsd/nextbsd-kernel#234: LE hardening). ng_hci_le_lp.h, the message
# ng_l2cap sends ng_hci, comes from NgHCI's src/.
SRCTOP?=	/usr/src
.PATH: ${SRCTOP}/sys/netgraph/bluetooth/l2cap ${.CURDIR}/src

CFLAGS+=	-I${SRCTOP}/sys/netgraph/bluetooth/include \
		-I${SRCTOP}/sys/netgraph/bluetooth/l2cap \
		-I${.CURDIR}/src -I${.CURDIR}/../NgHCI/src

KMOD=		ng_l2cap
# sys/modules/netgraph/bluetooth/l2cap, with these patched (<name>_nextbsd.c)
NEXTBSD_PATCHED=	ng_l2cap_cmds ng_l2cap_evnt ng_l2cap_llpi
SRCS=		ng_l2cap_main.c ng_l2cap_ulpi.c ng_l2cap_misc.c \
		${NEXTBSD_PATCHED:S/$/_nextbsd.c/}
# NextBSD: LE connection-parameter updates to ng_hci
SRCS+=		ng_l2cap_le.c

# Each <name>.c with its src/patches applied. patch(1) is given the file to
# patch, so the paths in the patches are only documentation; a patch that no
# longer applies fails the build.
.for f in ${NEXTBSD_PATCHED}
NEXTBSD_PATCHES.${f}!=	ls ${.CURDIR}/src/patches/[0-9][0-9][0-9][0-9]-${f}-*.patch
${f}_nextbsd.c: ${SRCTOP}/sys/netgraph/bluetooth/l2cap/${f}.c ${NEXTBSD_PATCHES.${f}}
	cp ${SRCTOP}/sys/netgraph/bluetooth/l2cap/${f}.c ${.TARGET}.tmp
.for p in ${NEXTBSD_PATCHES.${f}}
	patch -s ${.TARGET}.tmp < ${p}
.endfor
	rm -f ${.TARGET}.tmp.orig
	mv ${.TARGET}.tmp ${.TARGET}
CLEANFILES+=	${f}_nextbsd.c ${f}_nextbsd.c.tmp ${f}_nextbsd.c.tmp.orig
.endfor

.include <bsd.kmod.mk>

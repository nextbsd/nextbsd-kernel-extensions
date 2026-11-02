# ng_ubt, the USB Bluetooth HCI transport: sys/modules/netgraph/bluetooth/ubt,
# its sources from freebsd-src (the generic driver plus its Intel and Realtek
# probes, which share its attach and detach).
#
# NextBSD changes (src/): ng_ubt.c is built from a patched copy,
# ng_ubt_nextbsd.c, generated in the obj directory by applying
# src/patches/*.patch in order; the source tree is not touched. The patches
# call into NextBSD's own files in src/ (nextbsd-kernel-extensions#27: each
# controller is published in the IORegistry as an IOBluetoothHostController
# entry; nextbsd/nextbsd-kernel#237: the isochronous alternate setting
# follows the controller's voice links, and each SCO packet starts its own
# isochronous frame).
SRCTOP?=	/usr/src
.PATH: ${SRCTOP}/sys/netgraph/bluetooth/drivers/ubt ${.CURDIR}/src

CFLAGS+=	-I${SRCTOP}/sys/netgraph/bluetooth/include \
		-I${SRCTOP}/sys/netgraph/bluetooth/drivers/ubt \
		-I${.CURDIR}/src

KMOD=		ng_ubt
# sys/modules/netgraph/bluetooth/ubt, with ng_ubt.c patched (ng_ubt_nextbsd.c)
SRCS=		ng_ubt_nextbsd.c ng_ubt_intel.c ng_ubt_rtl.c opt_bus.h opt_usb.h \
		device_if.h bus_if.h usb_if.h usbdevs.h
# NextBSD: the IOBluetoothHostController registry entries
SRCS+=		ng_ubt_ioreg.c
# NextBSD: the isochronous (SCO) alternate setting by voice link
SRCS+=		ng_ubt_sco.c

# ng_ubt.c with src/patches applied. patch(1) is given the file to patch,
# so the paths in the patches are only documentation; a patch that no longer
# applies fails the build.
NEXTBSD_PATCHES!=	ls ${.CURDIR}/src/patches/*.patch
ng_ubt_nextbsd.c: ${SRCTOP}/sys/netgraph/bluetooth/drivers/ubt/ng_ubt.c ${NEXTBSD_PATCHES}
	cp ${SRCTOP}/sys/netgraph/bluetooth/drivers/ubt/ng_ubt.c ${.TARGET}.tmp
.for p in ${NEXTBSD_PATCHES}
	patch -s ${.TARGET}.tmp < ${p}
.endfor
	rm -f ${.TARGET}.tmp.orig
	mv ${.TARGET}.tmp ${.TARGET}
CLEANFILES+=	ng_ubt_nextbsd.c ng_ubt_nextbsd.c.tmp \
		ng_ubt_nextbsd.c.tmp.orig

.include <bsd.kmod.mk>

# vhid, the IOHIDUserDevice hidbus transport (nextbsd-kernel-extensions#25):
# HID devices created from userland through /dev/hidresource, for bluetoothd's
# HID host. NextBSD's own code, in src/; the hidbus, hid and evdev it plugs
# into are compiled into the kernel. src/hidresource.h is the userland
# interface. Not upstream.
.PATH:	${.CURDIR}/src
KMOD=	vhid
SRCS=	vhid.c bus_if.h device_if.h hid_if.h
CFLAGS+=	-I${.CURDIR}/src

.include <bsd.kmod.mk>

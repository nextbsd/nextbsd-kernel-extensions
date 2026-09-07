# Broadcom L2 interrupt controller (nextbsd-kernel-extensions#51).
#
# A plain newbus/INTRNG driver -- no LinuxKPI, no DRM. It is here rather than
# in the kernel because it can be: INTRNG resolves a device-tree interrupt when
# the resource is activated, not when the bus is enumerated, so a PIC that
# registers at module load still fixes every consumer that allocates later.

KMOD=	bcm_l2intc
SRCS=	bcm_l2intc.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h pic_if.h opt_platform.h

.include <bsd.kmod.mk>

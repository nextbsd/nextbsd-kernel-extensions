# Realtek's own FreeBSD driver for its PCIe Ethernet family, 1G to 10G
# (RTL8111/8168, RTL8125, RTL8126, RTL8127), vendored from the FreeBSD port
# net/realtek-re-kmod (E21 D14). It replaces base re(4): both are the "re"
# driver, so interfaces are still re0. See VENDOR.md.
#
# Same build options as upstream's Makefile (src/Makefile.upstream).
.PATH:	${.CURDIR}/src
KMOD	= if_re
SRCS	= if_re.c opt_bdg.h device_if.h bus_if.h pci_if.h opt_inet.h opt_inet6.h
CFLAGS	+= -DENABLE_INTERRUPT_MITIGATIN

.include <bsd.kmod.mk>

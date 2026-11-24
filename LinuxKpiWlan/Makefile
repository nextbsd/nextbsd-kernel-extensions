# Build sys/modules/linuxkpi_wlan with the FreeBSD stable/15 station
# registration fix (dada5634d4d2) until the base source includes it.
SRCTOP?=	/usr/src
WLAN_SRC=	${SRCTOP}/sys/compat/linuxkpi/common/src
.PATH: ${WLAN_SRC}

KMOD=	linuxkpi_wlan
SRCS=	linux_80211_nextbsd.c linux_80211_macops.c linux_mhi.c
SRCS+=	opt_wlan.h ${LINUXKPI_GENSRCS}
CFLAGS+= ${LINUXKPI_INCLUDES} -I${WLAN_SRC}
EXPORT_SYMS=	YES

STA_PATCH=	${.CURDIR}/src/patches/0001-pass-only-registered-stations.patch
linux_80211_nextbsd.c: ${WLAN_SRC}/linux_80211.c ${STA_PATCH}
	patch -s -F 0 -o ${.TARGET}.tmp ${WLAN_SRC}/linux_80211.c < ${STA_PATCH}
	mv ${.TARGET}.tmp ${.TARGET}
CLEANFILES+=	linux_80211_nextbsd.c linux_80211_nextbsd.c.tmp

.include <bsd.kmod.mk>

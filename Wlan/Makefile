# The net80211 family as one module (E21 D11): sys/modules/wlan plus the
# sys/modules/wlan_* plug-ins, their sources from freebsd-src. Each plug-in
# still declares its own module (wlan_ccmp, ...), so MODULE_DEPEND on any of
# them is satisfied by this file.
#
# NextBSD changes (src/): ieee80211.c is built from a patched copy,
# ieee80211_nextbsd.c, generated in the obj directory by applying
# src/patches/*.patch in order; the source tree is not touched. The patches
# call into NextBSD's own files in src/ (nextbsd-userland#502: each radio is
# published in the IORegistry as an IO80211Controller entry).
SRCTOP?=	/usr/src
.PATH: ${SRCTOP}/sys/net80211 ${SRCTOP}/sys/crypto/rijndael ${.CURDIR}/src

KMOD=	wlan
# sys/modules/wlan, with ieee80211.c patched (ieee80211_nextbsd.c)
SRCS=	ieee80211_nextbsd.c ieee80211_action.c ieee80211_ageq.c \
	ieee80211_crypto.c ieee80211_crypto_none.c ieee80211_dfs.c \
	ieee80211_freebsd.c ieee80211_input.c ieee80211_ioctl.c \
	ieee80211_mesh.c ieee80211_node.c ieee80211_output.c ieee80211_phy.c \
	ieee80211_power.c ieee80211_proto.c ieee80211_scan.c ieee80211_scan_sw.c \
	ieee80211_scan_sta.c ieee80211_radiotap.c ieee80211_ratectl.c \
	ieee80211_ratectl_none.c ieee80211_regdomain.c \
	ieee80211_ht.c ieee80211_hwmp.c ieee80211_adhoc.c ieee80211_hostap.c \
	ieee80211_monitor.c ieee80211_sta.c ieee80211_wds.c ieee80211_ddb.c \
	ieee80211_tdma.c ieee80211_superg.c ieee80211_vht.c
# the plug-ins: wlan_wep, wlan_tkip, wlan_ccmp, wlan_gcmp, wlan_amrr,
# wlan_rssadapt, wlan_xauth, wlan_acl (rijndael once, shared by ccmp and gcmp)
SRCS+=	ieee80211_crypto_wep.c ieee80211_crypto_tkip.c \
	ieee80211_crypto_ccmp.c ieee80211_crypto_gcmp.c ieee80211_crypto_gcm.c \
	rijndael-alg-fst.c rijndael-api.c \
	ieee80211_amrr.c ieee80211_rssadapt.c ieee80211_xauth.c ieee80211_acl.c
SRCS+=	bus_if.h device_if.h opt_ddb.h opt_inet.h opt_inet6.h \
	opt_tdma.h opt_wlan.h
# NextBSD: the IO80211Controller registry entries
SRCS+=	ieee80211_ioreg.c
CFLAGS+=	-I${.CURDIR}/src
EXPORT_SYMS=	YES

# ieee80211.c with src/patches applied. patch(1) is given the file to patch,
# so the paths in the patches are only documentation; a patch that no longer
# applies fails the build.
NEXTBSD_PATCHES!=	ls ${.CURDIR}/src/patches/*.patch
ieee80211_nextbsd.c: ${SRCTOP}/sys/net80211/ieee80211.c ${NEXTBSD_PATCHES}
	cp ${SRCTOP}/sys/net80211/ieee80211.c ${.TARGET}.tmp
.for p in ${NEXTBSD_PATCHES}
	patch -s ${.TARGET}.tmp < ${p}
.endfor
	rm -f ${.TARGET}.tmp.orig
	mv ${.TARGET}.tmp ${.TARGET}
CLEANFILES+=	ieee80211_nextbsd.c ieee80211_nextbsd.c.tmp \
		ieee80211_nextbsd.c.tmp.orig

.include <bsd.kmod.mk>

CWARNFLAGS.ieee80211_adhoc.c=	-Wno-unused-function
CWARNFLAGS.ieee80211_crypto.c=	-Wno-unused-function
CWARNFLAGS.ieee80211_hostap.c=	-Wno-unused-function
CWARNFLAGS.ieee80211_mesh.c=	-Wno-unused-function
CWARNFLAGS.ieee80211_sta.c=	-Wno-unused-function

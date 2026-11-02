/*
 * NextBSD: choose a USB Bluetooth controller's isochronous alternate setting
 * from its voice links (nextbsd/nextbsd-kernel#237). Not upstream. What and
 * why are in ng_ubt_sco.h; this is the mechanism.
 *
 * Why. releng/15.1's ng_ubt set interface 1 once, at attach, to the setting
 * with the largest wMaxPacketSize. The controller sizes its isochronous
 * stream by the setting the host chose for the links it has, so with one
 * CVSD link at alt 5 the SCO packets the host reassembles and sends are out
 * of step with the controller's, and audio is garbage or silent on many
 * controllers; mSBC needs alt 1 (or 6).
 *
 * State. One entry per attached controller, in a list keyed by softc, so
 * struct ubt_softc keeps its layout: the interface indexes, which settings
 * interface 1 has, the attach setting, the forced setting if any, the voice
 * setting, and the open synchronous links (handle and air mode). The
 * snooping hooks run in USB callback and netgraph context and only update
 * this and signal the explore process; the change itself
 * (ubt_isoc_set_alt() in ng_ubt.c) runs there, where reconfiguring an
 * interface is safe against enumeration and detach.
 *
 * Locking. ubt_sco_mtx guards the list and the entries. Order: the transfer
 * mutex (sc_if_mtx, held in USB callbacks), then ubt_sco_mtx, then the USB
 * bus lock (usb_proc_explore_lock). The explore callback is entered with the
 * bus lock held and drops it before taking ubt_sco_mtx. ubt_sco_detach()
 * marks the entry dying under ubt_sco_mtx and then waits for the explore
 * messages, so no message is queued for a freed entry.
 *
 * Nothing here prints at the default debug level except failures.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/condvar.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usb_process.h>

#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>
#include <netgraph/bluetooth/include/ng_bluetooth.h>
#include <netgraph/bluetooth/include/ng_hci.h>
#include <netgraph/bluetooth/include/ng_ubt.h>
#include <netgraph/bluetooth/drivers/ubt/ng_ubt_var.h>

#include "ng_ubt_sco.h"

#define	UBT_SCO_MAX_LINKS	3	/* a controller carries at most 3 */

/* HCI codes this file reads (Core 5.x Vol 4 Part E) */
#define	UBT_SCO_EV_CON_COMPL		0x03
#define	UBT_SCO_EV_DISCON_COMPL		0x05
#define	UBT_SCO_EV_SYNC_CON_COMPL	0x2c
#define	UBT_SCO_OP_RESET		0x0c03
#define	UBT_SCO_OP_WRITE_VOICE		0x0c26
#define	UBT_SCO_OP_SETUP_SYNC		0x0428
#define	UBT_SCO_OP_ACCEPT_SYNC		0x0429

#define	UBT_SCO_AIR_ULAW		0
#define	UBT_SCO_AIR_ALAW		1
#define	UBT_SCO_AIR_CVSD		2
#define	UBT_SCO_AIR_TRANSPARENT		3

#define	UBT_SCO_VOICE_DEFAULT		0x0060	/* after HCI_Reset */
#define	UBT_SCO_VOICE_16BIT		0x0020

static int ubt_sco_auto = 1;
SYSCTL_INT(_net_bluetooth, OID_AUTO, usb_isoc_auto_alt,
    CTLFLAG_RWTUN | CTLFLAG_MPSAFE, &ubt_sco_auto, 0,
    "set the USB isochronous alternate setting by voice link");

static MALLOC_DEFINE(M_UBT_SCO, "ubtsco", "ubt isochronous alternate setting");

struct ubt_sco;

struct ubt_sco_msg {
	struct usb_proc_msg	hdr;
	struct ubt_sco		*us;
};

struct ubt_sco_link {
	uint16_t		handle;
	uint8_t			air_mode;
};

struct ubt_sco {
	LIST_ENTRY(ubt_sco)	link;
	struct ubt_softc	*sc;		/* lookup key */
	struct usb_device	*udev;
	uint8_t			iface_index[2];
	uint32_t		alt_mask;	/* bit n: interface 1 has alt n */
	uint8_t			attach_alt;
	uint8_t			cur_alt;
	int32_t			mode;		/* NG_UBT_ISOC_ALT_AUTO or alt */
	uint16_t		voice_setting;
	int			nlinks;
	struct ubt_sco_link	links[UBT_SCO_MAX_LINKS];
	int			dying;
	struct ubt_sco_msg	msg[2];
};

static LIST_HEAD(, ubt_sco) ubt_sco_list = LIST_HEAD_INITIALIZER(ubt_sco_list);
static struct mtx ubt_sco_mtx;
MTX_SYSINIT(ubt_sco, &ubt_sco_mtx, "ubtsco", MTX_DEF);

static usb_proc_callback_t ubt_sco_configure_msg;

static struct ubt_sco *
ubt_sco_find(struct ubt_softc *sc)
{
	struct ubt_sco	*us;

	mtx_assert(&ubt_sco_mtx, MA_OWNED);

	LIST_FOREACH(us, &ubt_sco_list, link)
		if (us->sc == sc && !us->dying)
			return (us);

	return (NULL);
}

static int
ubt_sco_has_alt(const struct ubt_sco *us, int alt)
{
	return (alt >= 0 && alt < 32 && (us->alt_mask & (1u << alt)) != 0);
}

/* The setting the links call for. ubt_sco_mtx held. */
static uint8_t
ubt_sco_target(const struct ubt_sco *us)
{
	static const uint8_t alts16[UBT_SCO_MAX_LINKS] = { 2, 4, 5 };
	int	alt, i, n, transparent;

	if (us->mode != NG_UBT_ISOC_ALT_AUTO)
		return (us->mode);

	n = us->nlinks;
	if (!ubt_sco_auto || n == 0)
		return (us->attach_alt);

	transparent = 0;
	for (i = 0; i < n; i++)
		if (us->links[i].air_mode == UBT_SCO_AIR_TRANSPARENT)
			transparent = 1;

	if (transparent)
		alt = ubt_sco_has_alt(us, 6) ? 6 : 1;
	else if (us->voice_setting & UBT_SCO_VOICE_16BIT)
		alt = alts16[n - 1];
	else
		alt = n;

	if (!ubt_sco_has_alt(us, alt))
		alt = us->attach_alt;

	return (alt);
}

/* Ask the explore process to apply the target. ubt_sco_mtx held. */
static void
ubt_sco_kick(struct ubt_sco *us)
{
	mtx_assert(&ubt_sco_mtx, MA_OWNED);

	if (us->dying)
		return;

	usb_proc_explore_lock(us->udev);
	(void)usb_proc_explore_msignal(us->udev, &us->msg[0], &us->msg[1]);
	usb_proc_explore_unlock(us->udev);
}

/* USB explore process, entered with the bus lock held. */
static void
ubt_sco_configure_msg(struct usb_proc_msg *pm)
{
	struct ubt_sco		*us = ((struct ubt_sco_msg *)pm)->us;
	struct usb_device	*udev = us->udev;
	uint8_t			 target, cur;
	int			 error;

	usb_proc_explore_unlock(udev);

	mtx_lock(&ubt_sco_mtx);
	if (us->dying) {
		mtx_unlock(&ubt_sco_mtx);
		goto done;
	}
	target = ubt_sco_target(us);
	cur = us->cur_alt;
	mtx_unlock(&ubt_sco_mtx);

	if (target == cur)
		goto done;

	UBT_INFO(us->sc, "isochronous alternate setting %d -> %d\n", cur,
	    target);

	error = ubt_isoc_set_alt(us->sc, udev, us->iface_index, target);
	if (error != 0) {
		UBT_ALERT(us->sc, "could not set isochronous alternate " \
		    "setting %d, error=%d\n", target, error);
		if (target != us->attach_alt &&
		    ubt_isoc_set_alt(us->sc, udev, us->iface_index,
		    us->attach_alt) == 0)
			target = us->attach_alt;
		else
			target = 0xff;	/* unknown; try again next time */
	}

	mtx_lock(&ubt_sco_mtx);
	us->cur_alt = target;
	mtx_unlock(&ubt_sco_mtx);
done:
	usb_proc_explore_lock(udev);
}

/*
 * End of a successful ubt_attach(): interface 1 is at attach_alt and its
 * transfers are set up. Attach context, may sleep.
 */
void
ubt_sco_attach(struct ubt_softc *sc, struct usb_device *udev,
    const uint8_t *iface_index, uint8_t attach_alt)
{
	struct usb_interface		*iface;
	struct usb_interface_descriptor	*id;
	struct usb_descriptor		*desc = NULL;
	struct ubt_sco			*us;
	uint8_t				 ifno;

	iface = usbd_get_iface(udev, iface_index[1]);
	if (iface == NULL || iface->idesc == NULL)
		return;
	ifno = iface->idesc->bInterfaceNumber;

	us = malloc(sizeof(*us), M_UBT_SCO, M_WAITOK | M_ZERO);
	us->sc = sc;
	us->udev = udev;
	us->iface_index[0] = iface_index[0];
	us->iface_index[1] = iface_index[1];
	us->attach_alt = attach_alt;
	us->cur_alt = attach_alt;
	us->mode = NG_UBT_ISOC_ALT_AUTO;
	us->voice_setting = UBT_SCO_VOICE_DEFAULT;
	us->msg[0].hdr.pm_callback = &ubt_sco_configure_msg;
	us->msg[0].us = us;
	us->msg[1].hdr.pm_callback = &ubt_sco_configure_msg;
	us->msg[1].us = us;

	while ((desc = usb_desc_foreach(usbd_get_config_descriptor(udev),
	    desc)) != NULL) {
		if (desc->bDescriptorType != UDESC_INTERFACE ||
		    desc->bLength < sizeof(*id))
			continue;
		id = (struct usb_interface_descriptor *)desc;
		if (id->bInterfaceNumber == ifno && id->bAlternateSetting < 32)
			us->alt_mask |= 1u << id->bAlternateSetting;
	}

	UBT_INFO(sc, "isochronous alternate settings %#x, attach %d\n",
	    us->alt_mask, attach_alt);

	mtx_lock(&ubt_sco_mtx);
	LIST_INSERT_HEAD(&ubt_sco_list, us, link);
	mtx_unlock(&ubt_sco_mtx);
}

/* Start of ubt_detach(), before the transfers go. May sleep. */
void
ubt_sco_detach(struct ubt_softc *sc)
{
	struct ubt_sco	*us;

	mtx_lock(&ubt_sco_mtx);
	us = ubt_sco_find(sc);
	if (us != NULL)
		us->dying = 1;
	mtx_unlock(&ubt_sco_mtx);

	if (us == NULL)
		return;

	usb_proc_explore_lock(us->udev);
	usb_proc_explore_mwait(us->udev, &us->msg[0], &us->msg[1]);
	usb_proc_explore_unlock(us->udev);

	mtx_lock(&ubt_sco_mtx);
	LIST_REMOVE(us, link);
	mtx_unlock(&ubt_sco_mtx);

	free(us, M_UBT_SCO);
}

static int
ubt_sco_link_index(const struct ubt_sco *us, uint16_t handle)
{
	int	i;

	for (i = 0; i < us->nlinks; i++)
		if (us->links[i].handle == handle)
			return (i);

	return (-1);
}

static int
ubt_sco_voice_air_mode(uint16_t voice_setting)
{
	switch (voice_setting & 0x0003) {
	case 0x0000:	return (UBT_SCO_AIR_CVSD);
	case 0x0001:	return (UBT_SCO_AIR_ULAW);
	case 0x0002:	return (UBT_SCO_AIR_ALAW);
	default:	return (UBT_SCO_AIR_TRANSPARENT);
	}
}

/*
 * An HCI event on its way up: evt is the event code, len the bytes from
 * there. USB callback context.
 */
void
ubt_sco_event(struct ubt_softc *sc, const uint8_t *evt, int len)
{
	struct ubt_sco	*us;
	uint16_t	 handle;
	uint8_t		 air_mode;
	int		 i, changed = 0;

	if (len < 2)
		return;

	/* Cheap filter before the lookup: almost every event is none of these */
	switch (evt[0]) {
	case UBT_SCO_EV_CON_COMPL:
		/* status, handle(2), bdaddr(6), link_type */
		if (len < 2 + 10 || evt[2] != 0 || evt[2 + 9] != 0x00)
			return;
		break;
	case UBT_SCO_EV_SYNC_CON_COMPL:
		/* status, handle(2), bdaddr(6), link_type, ..., air_mode */
		if (len < 2 + 17 || evt[2] != 0)
			return;
		break;
	case UBT_SCO_EV_DISCON_COMPL:
		if (len < 2 + 3 || evt[2] != 0)
			return;
		break;
	default:
		return;
	}

	handle = (evt[3] | (evt[4] << 8)) & 0x0fff;

	mtx_lock(&ubt_sco_mtx);
	us = ubt_sco_find(sc);
	if (us == NULL) {
		mtx_unlock(&ubt_sco_mtx);
		return;
	}

	i = ubt_sco_link_index(us, handle);

	switch (evt[0]) {
	case UBT_SCO_EV_CON_COMPL:
	case UBT_SCO_EV_SYNC_CON_COMPL:
		if (evt[0] == UBT_SCO_EV_SYNC_CON_COMPL)
			air_mode = evt[2 + 16];
		else
			air_mode = ubt_sco_voice_air_mode(us->voice_setting);

		if (i >= 0) {
			changed = (us->links[i].air_mode != air_mode);
			us->links[i].air_mode = air_mode;
		} else if (us->nlinks < UBT_SCO_MAX_LINKS) {
			us->links[us->nlinks].handle = handle;
			us->links[us->nlinks].air_mode = air_mode;
			us->nlinks++;
			changed = 1;
		}
		break;

	case UBT_SCO_EV_DISCON_COMPL:
		if (i >= 0) {
			us->links[i] = us->links[us->nlinks - 1];
			us->nlinks--;
			changed = 1;
		}
		break;
	}

	if (changed)
		ubt_sco_kick(us);
	mtx_unlock(&ubt_sco_mtx);
}

/*
 * An HCI command on its way down (m starts with the packet type). Netgraph
 * context.
 */
void
ubt_sco_command(struct ubt_softc *sc, struct mbuf *m)
{
	struct ubt_sco	*us;
	uint8_t		 hdr[4], v[2];
	uint16_t	 opcode;
	int		 off, reset = 0;

	if (m->m_pkthdr.len < (int)sizeof(hdr))
		return;
	m_copydata(m, 0, sizeof(hdr), (caddr_t)hdr);
	opcode = hdr[1] | (hdr[2] << 8);

	switch (opcode) {
	case UBT_SCO_OP_RESET:
		reset = 1;
		off = -1;
		break;
	case UBT_SCO_OP_WRITE_VOICE:
		off = 0;
		break;
	case UBT_SCO_OP_SETUP_SYNC:
		off = 12;	/* handle, tx and rx bandwidth, latency */
		break;
	case UBT_SCO_OP_ACCEPT_SYNC:
		off = 16;	/* bdaddr, tx and rx bandwidth, latency */
		break;
	default:
		return;
	}

	if (off >= 0) {
		if (hdr[3] < off + 2 ||
		    m->m_pkthdr.len < (int)sizeof(hdr) + off + 2)
			return;
		m_copydata(m, sizeof(hdr) + off, sizeof(v), (caddr_t)v);
	}

	mtx_lock(&ubt_sco_mtx);
	us = ubt_sco_find(sc);
	if (us != NULL) {
		if (reset) {
			us->voice_setting = UBT_SCO_VOICE_DEFAULT;
			if (us->nlinks != 0) {
				us->nlinks = 0;
				ubt_sco_kick(us);
			}
		} else
			us->voice_setting = (v[0] | (v[1] << 8)) & 0x03ff;
	}
	mtx_unlock(&ubt_sco_mtx);
}

/* NGM_UBT_NODE_SET_ISOC_ALT. Netgraph context. */
int
ubt_sco_set_mode(struct ubt_softc *sc, int32_t mode)
{
	struct ubt_sco	*us;
	int		 error = 0;

	mtx_lock(&ubt_sco_mtx);
	us = ubt_sco_find(sc);
	if (us == NULL)
		error = ENXIO;	/* no isochronous transfers */
	else if (mode != NG_UBT_ISOC_ALT_AUTO && !ubt_sco_has_alt(us, mode))
		error = EINVAL;
	else {
		us->mode = mode;
		ubt_sco_kick(us);
	}
	mtx_unlock(&ubt_sco_mtx);

	return (error);
}

/* NGM_UBT_NODE_GET_ISOC_ALT. Netgraph context. */
int
ubt_sco_get(struct ubt_softc *sc, ng_ubt_node_isoc_alt_ep *ep)
{
	struct ubt_sco	*us;
	int		 error = 0, i;

	bzero(ep, sizeof(*ep));

	mtx_lock(&ubt_sco_mtx);
	us = ubt_sco_find(sc);
	if (us == NULL)
		error = ENXIO;
	else {
		ep->mode = us->mode;
		ep->alt = (us->cur_alt == 0xff) ? -1 : us->cur_alt;
		ep->attach_alt = us->attach_alt;
		ep->sco_links = us->nlinks;
		ep->air_mode = -1;
		for (i = 0; i < us->nlinks; i++)
			if (ep->air_mode < us->links[i].air_mode)
				ep->air_mode = us->links[i].air_mode;
		ep->voice_setting = us->voice_setting;
		ep->alt_mask = us->alt_mask;
	}
	mtx_unlock(&ubt_sco_mtx);

	return (error);
}

/*
 * NextBSD: publish each USB Bluetooth controller in the in-kernel IORegistry
 * as an IOBluetoothHostController entry (nextbsd-kernel-extensions#27). Not
 * upstream.
 *
 * Why. bluetoothd has to learn about controllers from a kernel notification,
 * the way wland learns about radios from IO80211Controller (Wlan/src/
 * ieee80211_ioreg.c), and Darwin's bluetoothd follows
 * IOBluetoothHostController. Matching the bare "ubt" newbus node would tie
 * bluetoothd to a FreeBSD driver name and to a node that exists before the
 * controller is usable; polling netgraph (NGM_LISTNODES) races arrival. This
 * entry is published only once ng_ubt has a named netgraph node and its USB
 * transfers are set up, so a client that sees it can connect the hook at once.
 * bluetoothd matches it with IOServiceAddMatchingNotification (first-match and
 * terminated) and drains the arming iterators, so a controller that attached
 * before bluetoothd subscribed is still delivered.
 *
 * Entry. Name and class: IOBluetoothHostController, as on Darwin. Parent: the
 * controller's own newbus node (ubtN), so the entry sits under the USB device
 * that carries it. Properties:
 *   idVendor, idProduct, bcdDevice   the USB device's ids (Darwin's USB keys,
 *                                    as on IOUSBHostDevice)
 *   Transport                        "USB"
 *   org.nextbsd.bluetooth.NodeName   the ng_ubt netgraph node, "ubt0"; the
 *                                    name bluetoothd builds its graph from
 *   org.nextbsd.ugen                 the device's ugen node, "ugen0.3", the
 *                                    same key IOUSBHostDevice carries
 *                                    (nextbsd-kernel#391)
 * They are copied at attach; the registry only carries strings, numbers and
 * bools.
 *
 * When. At the end of a successful ubt_attach(). The registry holds a
 * virtual entry's ARRIVE while any newbus probe/attach is in flight and sends
 * it once the tree quiesces, after its parent's ARRIVE. Retired at the start
 * of ubt_detach(), before the netgraph node is torn down, so a client that
 * sees the DEPART stops using the node before it goes.
 *
 * Firmware. An Intel or Realtek part still in bootloader mode is refused by
 * ng_ubt's probe (ng_ubt_intel.c, ng_ubt_rtl.c and ubt_ignore_devs), so it
 * never attaches and publishes nothing here; it is visible only as an
 * IOUSBHostDevice (nextbsd-kernel#391). Once the firmware is loaded the device
 * re-enumerates, ng_ubt attaches and this entry appears.
 *
 * Nothing here prints: a normal boot stays silent on the console.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/nv.h>
#include <sys/ioregistry.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include "ng_ubt_ioreg.h"

#define	UBT_IOREG_CLASS		"IOBluetoothHostController"

static MALLOC_DEFINE(M_UBT_IOREG, "ubtioreg", "ubt IORegistry entries");

struct ubt_ioreg_ent {
	LIST_ENTRY(ubt_ioreg_ent)	link;
	device_t			dev;	/* lookup key only */
	uint64_t			id;	/* registry id */
};

/* Controllers published and not yet retired, by device_t. */
static LIST_HEAD(, ubt_ioreg_ent) ubt_ioreg_list =
    LIST_HEAD_INITIALIZER(ubt_ioreg_list);
static struct mtx ubt_ioreg_mtx;
MTX_SYSINIT(ubt_ioreg, &ubt_ioreg_mtx, "ubtioreg", MTX_DEF);

void
ubt_ioreg_publish(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);
	struct ubt_ioreg_ent *e;
	nvlist_t *props;
	char ugen[16];

	snprintf(ugen, sizeof(ugen), "ugen%u.%u",
	    usbd_get_bus_index(uaa->device),
	    usbd_get_device_index(uaa->device));

	props = nvlist_create(0);
	nvlist_add_number(props, "idVendor", uaa->info.idVendor);
	nvlist_add_number(props, "idProduct", uaa->info.idProduct);
	nvlist_add_number(props, "bcdDevice", uaa->info.bcdDevice);
	nvlist_add_string(props, "Transport", "USB");
	nvlist_add_string(props, "org.nextbsd.bluetooth.NodeName",
	    device_get_nameunit(dev));
	nvlist_add_string(props, "org.nextbsd.ugen", ugen);

	e = malloc(sizeof(*e), M_UBT_IOREG, M_WAITOK | M_ZERO);
	e->dev = dev;
	/* The registry takes ownership of props, even on failure. */
	e->id = ioreg_publish_virtual(dev, UBT_IOREG_CLASS, UBT_IOREG_CLASS,
	    props);
	if (e->id == 0) {
		free(e, M_UBT_IOREG);
		return;
	}
	mtx_lock(&ubt_ioreg_mtx);
	LIST_INSERT_HEAD(&ubt_ioreg_list, e, link);
	mtx_unlock(&ubt_ioreg_mtx);
}

void
ubt_ioreg_retire(device_t dev)
{
	struct ubt_ioreg_ent *e;

	mtx_lock(&ubt_ioreg_mtx);
	LIST_FOREACH(e, &ubt_ioreg_list, link) {
		if (e->dev == dev) {
			LIST_REMOVE(e, link);
			break;
		}
	}
	mtx_unlock(&ubt_ioreg_mtx);
	/* None when ubt_attach() failed and called ubt_detach() itself. */
	if (e == NULL)
		return;
	/* DEPART to watchers; the entry lingers readable for a while. */
	(void)ioreg_retire_virtual(e->id);
	free(e, M_UBT_IOREG);
}

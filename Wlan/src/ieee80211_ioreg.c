/*
 * NextBSD: publish each 802.11 radio in the in-kernel IORegistry as an
 * IO80211Controller entry (nextbsd-userland#502). Not upstream.
 *
 * Why. A net80211 radio (struct ieee80211com, "iwlwifi0") is not a newbus
 * device of its own and not an ifnet, so nothing in the registry marks the
 * moment it becomes usable. The driver's newbus node attaches early, but a
 * driver such as iwlwifi loads its firmware asynchronously and calls
 * ieee80211_ifattach() seconds later; until then there is no radio to clone a
 * VAP from. wland matches this entry with IOServiceAddMatchingNotification
 * (first-match and terminated) instead of polling net.wlan.devices, the way
 * Darwin's wifid follows IO80211Controller.
 *
 * Entry. Name: the radio's name, ic_name, which is also what
 * net.wlan.devices lists and what SIOCIFCREATE2's wlandev takes. Class:
 * IO80211Controller. Parent: the synthetic IOResources node. The radio has no
 * device_t of its own, and finding the driver's would need the newbus topology
 * lock, which ieee80211_ifattach() callers do not consistently hold (a USB
 * driver calls it from device_attach, iwlwifi from its firmware-load task).
 * Properties use org.nextbsd.* keys; the registry only carries strings,
 * numbers and bools, so the MAC address is a string. They are copied at
 * attach, so nothing here reads the ieee80211com after that.
 *
 * When. Drivers call ieee80211_ifattach() before they fill in their
 * ieee80211com methods (ic_vap_create, ic_parent, ic_transmit, ...): LinuxKPI
 * sets them right after it returns, as do iwn, iwm, rtwn and the rest. An
 * ARRIVE sent from inside ieee80211_ifattach() could therefore reach wland,
 * and its SIOCIFCREATE2 reach net80211, before ic_vap_create is set. So the
 * publish is a task on taskqueue_thread, run after the caller is done:
 *   - LinuxKPI's request_firmware_nowait() runs the firmware callback, and so
 *     iwlwifi's and ath10k's ieee80211_ifattach() and the method setup after
 *     it, as a task on the same single-threaded taskqueue_thread; ours runs
 *     after that task returns;
 *   - for a driver that attaches from newbus (rtwn, run, rtw88, rtw89), the
 *     registry itself holds a virtual entry's ARRIVE while any newbus
 *     probe/attach is in flight and sends it when the tree quiesces, so it
 *     follows the driver's attach routine whichever thread publishes.
 * The retire is a task too, queued behind the publish, so the pair always runs
 * in order and a radio that detaches before its publish ran ends up retired.
 * It is queued at the end of ieee80211_ifdetach(), once the radio's VAPs are
 * destroyed and net.wlan.devices no longer lists it, so a client that sees the
 * DEPART finds neither.
 *
 * Nothing here prints: a normal boot stays silent on the console.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/socket.h>
#include <sys/taskqueue.h>
#include <sys/nv.h>
#include <sys/ioregistry.h>

#include <net/if.h>
#include <net/if_media.h>
#include <net/ethernet.h>

#include <net80211/ieee80211_var.h>

#include "ieee80211_ioreg.h"

#define	IEEE80211_IOREG_CLASS	"IO80211Controller"

static MALLOC_DEFINE(M_80211_IOREG, "80211ioreg", "802.11 IORegistry entries");

struct ieee80211_ioreg_ent {
	LIST_ENTRY(ieee80211_ioreg_ent)	link;
	const struct ieee80211com	*ic;	/* lookup key only */
	struct task			publish_task;
	struct task			retire_task;
	uint64_t			id;	/* registry id, 0 if none */
	char				name[IFNAMSIZ];	/* iwlwifi0 */
	char				driver[IFNAMSIZ]; /* iwlwifi */
	char				mac[ETHER_ADDR_LEN * 3];
};

/* Radios attached and not yet detached, by ieee80211com. */
static LIST_HEAD(, ieee80211_ioreg_ent) ieee80211_ioreg_list =
    LIST_HEAD_INITIALIZER(ieee80211_ioreg_list);
static struct mtx ieee80211_ioreg_mtx;
MTX_SYSINIT(ieee80211_ioreg, &ieee80211_ioreg_mtx, "80211ioreg", MTX_DEF);

static void
ieee80211_ioreg_publish_task(void *arg, int pending __unused)
{
	struct ieee80211_ioreg_ent *e = arg;
	nvlist_t *props;

	props = nvlist_create(0);
	nvlist_add_string(props, "org.nextbsd.80211.Device", e->name);
	nvlist_add_string(props, "org.nextbsd.80211.Driver", e->driver);
	nvlist_add_string(props, "org.nextbsd.80211.MACAddress", e->mac);
	/* The registry takes ownership of props, even on failure. */
	e->id = ioreg_publish_virtual(NULL, e->name, IEEE80211_IOREG_CLASS,
	    props);
}

static void
ieee80211_ioreg_retire_task(void *arg, int pending __unused)
{
	struct ieee80211_ioreg_ent *e = arg;

	/* DEPART to watchers; the entry lingers readable for a while. */
	if (e->id != 0)
		(void)ioreg_retire_virtual(e->id);
	free(e, M_80211_IOREG);
}

void
ieee80211_ioreg_publish(struct ieee80211com *ic)
{
	struct ieee80211_ioreg_ent *e;
	size_t len;

	if (ic->ic_name == NULL || ic->ic_name[0] == '\0')
		return;

	e = malloc(sizeof(*e), M_80211_IOREG, M_WAITOK | M_ZERO);
	e->ic = ic;
	strlcpy(e->name, ic->ic_name, sizeof(e->name));
	/* The driver name is the radio name without its unit. */
	strlcpy(e->driver, ic->ic_name, sizeof(e->driver));
	len = strlen(e->driver);
	while (len > 0 && e->driver[len - 1] >= '0' &&
	    e->driver[len - 1] <= '9')
		e->driver[--len] = '\0';
	snprintf(e->mac, sizeof(e->mac), "%6D", ic->ic_macaddr, ":");
	TASK_INIT(&e->publish_task, 0, ieee80211_ioreg_publish_task, e);
	TASK_INIT(&e->retire_task, 0, ieee80211_ioreg_retire_task, e);

	mtx_lock(&ieee80211_ioreg_mtx);
	LIST_INSERT_HEAD(&ieee80211_ioreg_list, e, link);
	mtx_unlock(&ieee80211_ioreg_mtx);
	taskqueue_enqueue(taskqueue_thread, &e->publish_task);
}

void
ieee80211_ioreg_retire(struct ieee80211com *ic)
{
	struct ieee80211_ioreg_ent *e;

	mtx_lock(&ieee80211_ioreg_mtx);
	LIST_FOREACH(e, &ieee80211_ioreg_list, link) {
		if (e->ic == ic) {
			LIST_REMOVE(e, link);
			break;
		}
	}
	mtx_unlock(&ieee80211_ioreg_mtx);
	/* Runs after the publish task, which taskqueue_thread ran or will. */
	if (e != NULL)
		taskqueue_enqueue(taskqueue_thread, &e->retire_task);
}

/*
 * Every radio has detached before net80211 can unload (each driver depends on
 * it), but its retire task may still be queued: let it run while its code is
 * still here.
 */
static void
ieee80211_ioreg_uninit(void *arg __unused)
{
	taskqueue_drain_all(taskqueue_thread);
}
SYSUNINIT(ieee80211_ioreg, SI_SUB_DRIVERS, SI_ORDER_ANY,
    ieee80211_ioreg_uninit, NULL);

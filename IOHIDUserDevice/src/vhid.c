/*
 * NextBSD: vhid, a hidbus transport whose devices are created from userland
 * through /dev/hidresource (IOHIDUserDevice.kext,
 * nextbsd-kernel-extensions#25). Not upstream.
 *
 * Why. bluetoothd owns the Bluetooth HID host (HIDP over L2CAP, and HOGP over
 * GATT) in userland, as Darwin's does. Each keyboard, mouse or trackpad it
 * connects becomes a vhid device, so the ordinary hidbus drivers attach to it
 * as they do for USB and I2C: hkbd joins kbdmux and works at the console and
 * loginwindow, hms and hmt feed evdev, and Hcons and Hsctrl autoload through
 * IOKit matching. The IORegistry publishes their evdev nodes as IOHIDDevice,
 * with Transport = "Bluetooth" for BUS_BLUETOOTH. This replaces bthidd and
 * vkbd. Design: nextbsd-userland wiki, E16-Audit-3-Virtual-HID-Prior-Art §4.
 *
 * Shape. Darwin's IOHIDResource is a singleton on IOResources; each
 * IOServiceOpen of it makes an IOHIDResourceDeviceUserClient, which creates
 * one IOHIDUserDevice. Here hidresource0 is a pseudo-bus under nexus that owns
 * /dev/hidresource, each open of the node is one user client (its state hangs
 * off the descriptor with devfs_set_cdevpriv), and its CREATE adds one vhidN
 * child, the IOHIDUserDevice, whose hidbus child the drivers attach to:
 *   nexus0 -> hidresource0 -> vhid0 -> hidbus0 -> hkbd0 -> IOHIDDevice
 * The ioctls, read(2) messages and their semantics are in hidresource.h.
 *
 * Attach. hidbus's drivers make synchronous requests while they attach: hmt
 * reads feature reports, hkbd sets the protocol, the idle rate and its LEDs,
 * bcm5974 switches modes. Each request is answered by the daemon that called
 * CREATE, so attaching in its ioctl would deadlock. CREATE returns at once and
 * a task on the kext's own taskqueue attaches the device under the topology
 * lock (which also lets the IORegistry parent the evdev nodes to the drivers'
 * newbus nodes). Requests then sleep on the device's mutex until the reply or
 * the timeout; the reply path takes only that mutex, never Giant or the
 * topology lock.
 *
 * Teardown. DESTROY, or the last close (the cdevpriv destructor, which also
 * runs when the process dies), fails every pending request with ENXIO, sends
 * an all-zero input report for each report ID so no key or button stays
 * pressed, and deletes vhidN from the same taskqueue. The descriptor can
 * CREATE again after a DESTROY.
 *
 * Input. write(2) or INPUT hands one report to hidbus's interrupt handler,
 * between intr_start and intr_stop; outside that it is dropped and counted.
 * A userland transport cannot be polled, so a vhid keyboard does not work in
 * ddb or at a panic, as on Darwin.
 *
 * Nothing here prints: the nodes are quiet and a normal boot stays silent.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bitstring.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/filio.h>
#include <sys/kdb.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/poll.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/selinfo.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/uio.h>

#include <dev/evdev/input.h>
#include <dev/hid/hid.h>

#include "hid_if.h"

#include "hidresource.h"

CTASSERT(HIDRESOURCE_BUS_BLUETOOTH == BUS_BLUETOOTH);
CTASSERT(HIDRESOURCE_BUS_VIRTUAL == BUS_VIRTUAL);
CTASSERT(HIDRESOURCE_REPORT_INPUT == HID_INPUT_REPORT);
CTASSERT(HIDRESOURCE_REPORT_OUTPUT == HID_OUTPUT_REPORT);
CTASSERT(HIDRESOURCE_REPORT_FEATURE == HID_FEATURE_REPORT);
CTASSERT(sizeof(struct hidresource_create) <= IOCPARM_MAX);
CTASSERT(sizeof(struct hidresource_reply) <= IOCPARM_MAX);
CTASSERT(HIDRESOURCE_NAME_MAX == sizeof(((struct hid_device_info *)0)->name));
CTASSERT(HIDRESOURCE_NAME_MAX == sizeof(((struct hid_device_info *)0)->serial));

/* sys/dev/hid/hidbus.c; global, but no header declares it. */
extern driver_t hidbus_driver;

/* Request timeout, ms: Darwin's default and RequestTimeout range. */
#define	VHID_TIMEOUT_DEFAULT	1000
#define	VHID_TIMEOUT_MIN	50
#define	VHID_TIMEOUT_MAX	1200000

/* Messages queued for the daemon per device. */
#define	VHID_QUEUE_MAX		128

static MALLOC_DEFINE(M_VHID, "vhid", "IOHIDUserDevice virtual HID devices");

static SYSCTL_NODE(_hw, OID_AUTO, hidresource, CTLFLAG_RD | CTLFLAG_MPSAFE,
    0, "IOHIDUserDevice: HID devices created from userland");
static int vhid_max_devices = 32;
SYSCTL_INT(_hw_hidresource, OID_AUTO, max_devices, CTLFLAG_RWTUN,
    &vhid_max_devices, 0, "Most devices that can exist at once");
static u_long vhid_dropped_input;
SYSCTL_ULONG(_hw_hidresource, OID_AUTO, dropped_input, CTLFLAG_RD,
    &vhid_dropped_input, 0, "Input reports dropped: no driver had it open");
static u_long vhid_dropped_events;
SYSCTL_ULONG(_hw_hidresource, OID_AUTO, dropped_events, CTLFLAG_RD,
    &vhid_dropped_events, 0, "Messages to the daemon dropped: queue full");
static u_long vhid_timeouts;
SYSCTL_ULONG(_hw_hidresource, OID_AUTO, timeouts, CTLFLAG_RD,
    &vhid_timeouts, 0, "Requests the daemon did not answer in time");

enum vhid_state {
	VHID_IDLE,	/* no device: CREATE may run */
	VHID_LIVE,	/* created; attached, or attaching */
	VHID_DYING,	/* being destroyed; every request fails */
};

/* A message for the daemon, as read(2) returns it. */
struct vhid_event {
	STAILQ_ENTRY(vhid_event)	link;
	struct hidresource_event	hdr;
	uint8_t				data[];
};

/* A driver's request, on its stack while it sleeps for the reply. */
struct vhid_request {
	TAILQ_ENTRY(vhid_request)	link;
	uint32_t			token;
	void				*buf;	/* GET_REPORT: the reply */
	hid_size_t			maxlen;
	hid_size_t			actlen;
	int				error;
	bool				done;
};

/* One open of /dev/hidresource: a user client and its device. */
struct vhid_softc {
	struct mtx		mtx;	/* everything below but intr* */
	struct sx		ctl_sx;	/* serialises CREATE and DESTROY */
	struct sx		intr_sx; /* intr* against input delivery */
	enum vhid_state		state;
	device_t		dev;	/* vhidN while attached */
	struct hid_device_info	hdi;	/* hidbus's ivars */
	uint8_t			*rdesc;
	bool			numbered; /* reports carry an ID byte */
	int			timeout; /* ticks */
	bool			counted; /* holds a max_devices slot */

	hid_intr_t		*intr;	/* hidbus_intr */
	void			*intr_ctx;
	bool			intr_on;

	STAILQ_HEAD(, vhid_event) evq;
	int			nev;
	TAILQ_HEAD(, vhid_request) reqs;
	uint32_t		next_token;
	struct selinfo		rsel;

	struct task		attach_task;
	struct task		detach_task;
};

/*
 * The hidresource0 bus, its node and its taskqueue. They exist while it is
 * attached; it refuses to detach (and the kext to unload) while the node is
 * open, so an open descriptor can use them without a lock.
 */
static struct mtx	hr_mtx;
MTX_SYSINIT(hidresource, &hr_mtx, "hidresource", MTX_DEF);
static device_t		hr_dev;
static struct cdev	*hr_cdev;
static struct taskqueue	*hr_tq;
static int		hr_opens;	/* under hr_mtx */
static bool		hr_closing;	/* under hr_mtx */
static int		hr_ndevices;	/* under hr_mtx */

static d_open_t		hr_open;
static d_read_t		hr_read;
static d_write_t	hr_write;
static d_ioctl_t	hr_ioctl;
static d_poll_t		hr_poll;
static d_kqfilter_t	hr_kqfilter;

static struct cdevsw hr_cdevsw = {
	.d_version =	D_VERSION,
	.d_name =	"hidresource",
	.d_open =	hr_open,
	.d_read =	hr_read,
	.d_write =	hr_write,
	.d_ioctl =	hr_ioctl,
	.d_poll =	hr_poll,
	.d_kqfilter =	hr_kqfilter,
};

static void	vhid_kqdetach(struct knote *);
static int	vhid_kqread(struct knote *, long);

static const struct filterops vhid_kqread_ops = {
	.f_isfd =	1,
	.f_detach =	vhid_kqdetach,
	.f_event =	vhid_kqread,
};

/* ---- the message queue ---- */

/* Queue `ev` for the daemon and wake its readers. */
static void
vhid_enqueue_locked(struct vhid_softc *sc, struct vhid_event *ev)
{
	mtx_assert(&sc->mtx, MA_OWNED);
	STAILQ_INSERT_TAIL(&sc->evq, ev, link);
	sc->nev++;
	wakeup(&sc->evq);
	selwakeuppri(&sc->rsel, PZERO);
	KNOTE_LOCKED(&sc->rsel.si_note, 0);
}

/*
 * Make room for one message by dropping the oldest that needs no reply (an
 * output report, START or STOP); a newer LED state supersedes an older one.
 * False if every queued message is a request.
 */
static bool
vhid_make_room_locked(struct vhid_softc *sc)
{
	struct vhid_event *ev;

	mtx_assert(&sc->mtx, MA_OWNED);
	if (sc->nev < VHID_QUEUE_MAX)
		return (true);
	STAILQ_FOREACH(ev, &sc->evq, link) {
		if (ev->hdr.he_token == 0) {
			STAILQ_REMOVE(&sc->evq, ev, vhid_event, link);
			sc->nev--;
			free(ev, M_VHID);
			atomic_add_long(&vhid_dropped_events, 1);
			return (true);
		}
	}
	return (false);
}

/* Drop the unread request `token`, if it is still queued. */
static void
vhid_unqueue_locked(struct vhid_softc *sc, uint32_t token)
{
	struct vhid_event *ev;

	STAILQ_FOREACH(ev, &sc->evq, link) {
		if (ev->hdr.he_token == token) {
			STAILQ_REMOVE(&sc->evq, ev, vhid_event, link);
			sc->nev--;
			free(ev, M_VHID);
			return;
		}
	}
}

static void
vhid_flush_locked(struct vhid_softc *sc)
{
	struct vhid_event *ev;

	while ((ev = STAILQ_FIRST(&sc->evq)) != NULL) {
		STAILQ_REMOVE_HEAD(&sc->evq, link);
		free(ev, M_VHID);
	}
	sc->nev = 0;
}

/*
 * A message that needs no reply (START, STOP, OUTPUT). It may come from a
 * driver that cannot sleep (hkbd sets its LEDs from the keyboard layer), so
 * it never does; it is dropped and counted if memory or the queue is short.
 */
static int
vhid_notify(struct vhid_softc *sc, uint32_t type, const void *data,
    hid_size_t len)
{
	struct vhid_event *ev;

	ev = malloc(sizeof(*ev) + len, M_VHID, M_NOWAIT | M_ZERO);
	if (ev == NULL) {
		atomic_add_long(&vhid_dropped_events, 1);
		return (ENOMEM);
	}
	ev->hdr.he_type = type;
	if (type == HIDRESOURCE_EVENT_OUTPUT) {
		ev->hdr.he_report_type = HIDRESOURCE_REPORT_OUTPUT;
		/* The ID byte leads a numbered report. */
		if (sc->numbered && len > 0)
			ev->hdr.he_report_id = ((const uint8_t *)data)[0];
	}
	ev->hdr.he_len = len;
	if (len > 0)
		memcpy(ev->data, data, len);

	mtx_lock(&sc->mtx);
	if (sc->state != VHID_LIVE) {
		mtx_unlock(&sc->mtx);
		free(ev, M_VHID);
		return (ENXIO);
	}
	if (!vhid_make_room_locked(sc)) {
		mtx_unlock(&sc->mtx);
		free(ev, M_VHID);
		atomic_add_long(&vhid_dropped_events, 1);
		return (ENOBUFS);
	}
	vhid_enqueue_locked(sc, ev);
	mtx_unlock(&sc->mtx);
	return (0);
}

/*
 * A request (GET_REPORT, SET_REPORT, SET_IDLE, SET_PROTOCOL): queue it with a
 * fresh token and sleep until HIDRESOURCE_IOC_REPLY completes it, the device
 * is destroyed, or the timeout passes. `data` is SET_REPORT's report;
 * `rbuf`/`maxlen` take GET_REPORT's.
 */
static int
vhid_request(struct vhid_softc *sc, uint32_t type, uint8_t rtype,
    uint8_t id, uint16_t value, const void *data, hid_size_t len,
    void *rbuf, hid_size_t maxlen, hid_size_t *actlen)
{
	struct vhid_request req;
	struct vhid_event *ev;
	int deadline, error, left;

	/* ddb or a panic: nothing can sleep, and the daemon is not running. */
	if (HID_IN_POLLING_MODE())
		return (EOPNOTSUPP);
	if (len > HIDRESOURCE_REPORT_MAX)
		return (EINVAL);
	if (maxlen > HIDRESOURCE_REPORT_MAX)
		maxlen = HIDRESOURCE_REPORT_MAX;

	ev = malloc(sizeof(*ev) + len, M_VHID, M_WAITOK | M_ZERO);
	ev->hdr.he_type = type;
	ev->hdr.he_report_type = rtype;
	ev->hdr.he_report_id = id;
	ev->hdr.he_value = value;
	ev->hdr.he_maxlen = maxlen;
	ev->hdr.he_len = len;
	if (len > 0)
		memcpy(ev->data, data, len);

	bzero(&req, sizeof(req));
	req.buf = rbuf;
	req.maxlen = maxlen;

	mtx_lock(&sc->mtx);
	if (sc->state != VHID_LIVE) {
		mtx_unlock(&sc->mtx);
		free(ev, M_VHID);
		return (ENXIO);
	}
	if (!vhid_make_room_locked(sc)) {
		mtx_unlock(&sc->mtx);
		free(ev, M_VHID);
		atomic_add_long(&vhid_dropped_events, 1);
		return (ENOBUFS);
	}
	if (++sc->next_token == 0)
		sc->next_token = 1;
	req.token = ev->hdr.he_token = sc->next_token;
	TAILQ_INSERT_TAIL(&sc->reqs, &req, link);
	vhid_enqueue_locked(sc, ev);

	deadline = ticks + sc->timeout;
	while (!req.done) {
		left = deadline - ticks;
		if (left <= 0)
			break;
		(void)msleep(&req, &sc->mtx, PZERO, "vhidrq", left);
	}
	TAILQ_REMOVE(&sc->reqs, &req, link);
	if (req.done) {
		error = req.error;
		if (error == 0 && actlen != NULL)
			*actlen = req.actlen;
	} else {
		/* Unanswered: a late reply finds no token (ESRCH). */
		vhid_unqueue_locked(sc, req.token);
		atomic_add_long(&vhid_timeouts, 1);
		error = ETIMEDOUT;
	}
	if (TAILQ_EMPTY(&sc->reqs))
		wakeup(&sc->reqs);
	mtx_unlock(&sc->mtx);
	return (error);
}

/* HIDRESOURCE_IOC_REPLY. Takes only the device's mutex. */
static int
vhid_reply(struct vhid_softc *sc, const struct hidresource_reply *hr)
{
	struct vhid_request *req;
	hid_size_t n;

	if (hr->hr_len > HIDRESOURCE_REPORT_MAX)
		return (EINVAL);
	mtx_lock(&sc->mtx);
	TAILQ_FOREACH(req, &sc->reqs, link) {
		if (req->token == hr->hr_token && !req->done)
			break;
	}
	if (req == NULL) {
		mtx_unlock(&sc->mtx);
		return (ESRCH);
	}
	req->error = hr->hr_error;
	if (req->error < 0 || req->error > ELAST)
		req->error = EIO;
	if (req->error == 0 && req->buf != NULL) {
		n = MIN(hr->hr_len, req->maxlen);
		memcpy(req->buf, hr->hr_data, n);
		req->actlen = n;
	}
	req->done = true;
	wakeup(req);
	mtx_unlock(&sc->mtx);
	return (0);
}

/* ---- input ---- */

/* Hand one input report to hidbus, if a driver has the device open. */
static void
vhid_deliver(struct vhid_softc *sc, void *buf, hid_size_t len)
{
	sx_xlock(&sc->intr_sx);
	if (sc->intr != NULL && sc->intr_on)
		sc->intr(sc->intr_ctx, buf, len);
	else
		atomic_add_long(&vhid_dropped_input, 1);
	sx_xunlock(&sc->intr_sx);
}

/* write(2) and HIDRESOURCE_IOC_INPUT: Darwin's HandleReport. */
static int
vhid_input(struct vhid_softc *sc, void *buf, hid_size_t len)
{
	bool live;

	if (len == 0 || len > HIDRESOURCE_REPORT_MAX)
		return (EINVAL);
	mtx_lock(&sc->mtx);
	live = sc->state == VHID_LIVE;
	mtx_unlock(&sc->mtx);
	if (!live)
		return (ENXIO);
	vhid_deliver(sc, buf, len);
	return (0);
}

/*
 * Before the device goes, send an all-zero input report for each input
 * report ID: every key and button reads as released, and no key repeats on
 * after the link drops mid-press.
 */
static void
vhid_release_inputs(struct vhid_softc *sc)
{
	struct hid_data *hd;
	struct hid_item hi;
	bitstr_t bit_decl(ids, 256);
	uint8_t *buf;
	int id, len;

	if (sc->rdesc == NULL)
		return;
	bit_nclear(ids, 0, 255);
	hd = hid_start_parse(sc->rdesc, sc->hdi.rdescsize, 1 << hid_input);
	while (hid_get_item(hd, &hi)) {
		if (hi.kind == hid_input)
			bit_set(ids, hi.report_ID);
	}
	hid_end_parse(hd);

	buf = malloc(HIDRESOURCE_REPORT_MAX, M_VHID, M_WAITOK);
	for (id = 0; id < 256; id++) {
		if (!bit_test(ids, id))
			continue;
		len = hid_report_size(sc->rdesc, sc->hdi.rdescsize, hid_input,
		    id);
		if (len <= 0 || len > HIDRESOURCE_REPORT_MAX)
			continue;
		bzero(buf, len);
		buf[0] = id;	/* 0 when unnumbered, the first data byte */
		sx_xlock(&sc->intr_sx);
		if (sc->intr != NULL && sc->intr_on)
			sc->intr(sc->intr_ctx, buf, len);
		sx_xunlock(&sc->intr_sx);
	}
	free(buf, M_VHID);
}

/* ---- create and destroy ---- */

/* Attach vhidN and, through it, hidbus and its drivers. */
static void
vhid_attach_task(void *arg, int pending __unused)
{
	struct vhid_softc *sc = arg;
	device_t child;
	bool live;

	bus_topo_lock();
	mtx_lock(&sc->mtx);
	live = sc->state == VHID_LIVE;
	mtx_unlock(&sc->mtx);
	if (live && sc->dev == NULL) {
		child = device_add_child(hr_dev, "vhid", DEVICE_UNIT_ANY);
		if (child != NULL) {
			device_quiet(child);
			device_set_ivars(child, sc);
			if (device_probe_and_attach(child) != 0)
				(void)device_delete_child(hr_dev, child);
		}
	}
	bus_topo_unlock();
}

static void
vhid_detach_task(void *arg, int pending __unused)
{
	struct vhid_softc *sc = arg;

	bus_topo_lock();
	if (sc->dev != NULL)
		(void)device_delete_child(hr_dev, sc->dev);
	bus_topo_unlock();
}

/* Whether the descriptor numbers its reports (then all of them are). */
static bool
vhid_numbered(const void *desc, hid_size_t len)
{
	struct hid_data *hd;
	struct hid_item hi;
	bool numbered;

	numbered = false;
	hd = hid_start_parse(desc, len,
	    (1 << hid_input) | (1 << hid_output) | (1 << hid_feature));
	while (hid_get_item(hd, &hi)) {
		if (hi.report_ID != 0) {
			numbered = true;
			break;
		}
	}
	hid_end_parse(hd);
	return (numbered);
}

/* HIDRESOURCE_IOC_CREATE: Darwin's Create. */
static int
vhid_create(struct vhid_softc *sc, struct hidresource_create *hc,
    struct thread *td)
{
	uint8_t *rdesc;
	uint32_t ms;
	uint16_t bus;
	int error;
	bool numbered;

	if (hc->hc_desc_len == 0 || hc->hc_desc_len > HIDRESOURCE_DESC_MAX)
		return (EINVAL);
	switch (hc->hc_bus) {
	case 0:
	case HIDRESOURCE_BUS_VIRTUAL:
		bus = BUS_VIRTUAL;
		break;
	case HIDRESOURCE_BUS_BLUETOOTH:
		/* Darwin: only a privileged creator may claim real hardware. */
		bus = priv_check(td, PRIV_DRIVER) == 0 ?
		    BUS_BLUETOOTH : BUS_VIRTUAL;
		break;
	default:
		return (EINVAL);
	}
	hc->hc_name[sizeof(hc->hc_name) - 1] = '\0';
	hc->hc_uniq[sizeof(hc->hc_uniq) - 1] = '\0';
	ms = hc->hc_timeout == 0 ? VHID_TIMEOUT_DEFAULT :
	    MAX(VHID_TIMEOUT_MIN, MIN(VHID_TIMEOUT_MAX, hc->hc_timeout));

	rdesc = malloc(hc->hc_desc_len, M_VHID, M_WAITOK);
	memcpy(rdesc, hc->hc_desc, hc->hc_desc_len);
	numbered = vhid_numbered(rdesc, hc->hc_desc_len);

	sx_xlock(&sc->ctl_sx);
	if (sc->state != VHID_IDLE) {
		error = EBUSY;
		goto out;
	}
	mtx_lock(&hr_mtx);
	if (hr_ndevices >= vhid_max_devices) {
		mtx_unlock(&hr_mtx);
		error = ENOSPC;
		goto out;
	}
	hr_ndevices++;
	mtx_unlock(&hr_mtx);
	sc->counted = true;

	bzero(&sc->hdi, sizeof(sc->hdi));
	strlcpy(sc->hdi.name, hc->hc_name[0] != '\0' ? hc->hc_name :
	    "IOHIDUserDevice", sizeof(sc->hdi.name));
	strlcpy(sc->hdi.serial, hc->hc_uniq, sizeof(sc->hdi.serial));
	sc->hdi.idBus = bus;
	sc->hdi.idVendor = hc->hc_vendor;
	sc->hdi.idProduct = hc->hc_product;
	sc->hdi.idVersion = hc->hc_version;
	sc->hdi.rdescsize = hc->hc_desc_len;

	mtx_lock(&sc->mtx);
	sc->rdesc = rdesc;
	rdesc = NULL;
	sc->numbered = numbered;
	sc->timeout = MAX(1, howmany((uint64_t)ms * hz, 1000));
	sc->state = VHID_LIVE;
	mtx_unlock(&sc->mtx);
	hc->hc_bus = bus;

	/* Attach from the taskqueue: the drivers' requests come back here. */
	taskqueue_enqueue(hr_tq, &sc->attach_task);
	error = 0;
out:
	sx_xunlock(&sc->ctl_sx);
	free(rdesc, M_VHID);
	return (error);
}

/*
 * HIDRESOURCE_IOC_DESTROY and the last close: Darwin's Terminate and
 * clientClose. Returns EBUSY only if the device would not detach; it then
 * stays, inert, and its state is never freed.
 */
static int
vhid_destroy(struct vhid_softc *sc)
{
	struct vhid_request *req;
	int error;

	sx_xlock(&sc->ctl_sx);
	mtx_lock(&sc->mtx);
	if (sc->state != VHID_LIVE) {
		error = sc->state == VHID_IDLE ? 0 : EBUSY;
		mtx_unlock(&sc->mtx);
		sx_xunlock(&sc->ctl_sx);
		return (error);
	}
	sc->state = VHID_DYING;
	/* Darwin's cleanupPendingReports: fail them all. */
	TAILQ_FOREACH(req, &sc->reqs, link) {
		req->error = ENXIO;
		req->done = true;
		wakeup(req);
	}
	vhid_flush_locked(sc);
	wakeup(&sc->evq);
	mtx_unlock(&sc->mtx);

	vhid_release_inputs(sc);

	/* Behind any attach still queued; the taskqueue has one thread. */
	taskqueue_enqueue(hr_tq, &sc->detach_task);
	taskqueue_drain(hr_tq, &sc->attach_task);
	taskqueue_drain(hr_tq, &sc->detach_task);

	mtx_lock(&sc->mtx);
	while (!TAILQ_EMPTY(&sc->reqs))
		(void)msleep(&sc->reqs, &sc->mtx, PZERO, "vhiddrn", hz / 10);
	if (sc->dev != NULL) {
		/* A driver refused to detach. */
		mtx_unlock(&sc->mtx);
		sx_xunlock(&sc->ctl_sx);
		return (EBUSY);
	}
	vhid_flush_locked(sc);
	free(sc->rdesc, M_VHID);
	sc->rdesc = NULL;
	sc->state = VHID_IDLE;
	mtx_unlock(&sc->mtx);

	if (sc->counted) {
		mtx_lock(&hr_mtx);
		hr_ndevices--;
		mtx_unlock(&hr_mtx);
		sc->counted = false;
	}
	sx_xunlock(&sc->ctl_sx);
	return (0);
}

/* ---- /dev/hidresource ---- */

/* The last close of a descriptor, also when its process dies. */
static void
vhid_dtor(void *arg)
{
	struct vhid_softc *sc = arg;
	bool leak;

	leak = vhid_destroy(sc) != 0;

	knlist_clear(&sc->rsel.si_note, 0);
	seldrain(&sc->rsel);
	knlist_destroy(&sc->rsel.si_note);
	if (!leak) {
		sx_destroy(&sc->intr_sx);
		sx_destroy(&sc->ctl_sx);
		mtx_destroy(&sc->mtx);
		free(sc, M_VHID);
	}

	mtx_lock(&hr_mtx);
	hr_opens--;
	mtx_unlock(&hr_mtx);
}

static int
hr_open(struct cdev *cdev __unused, int oflags __unused, int devtype __unused,
    struct thread *td __unused)
{
	struct vhid_softc *sc;
	int error;

	mtx_lock(&hr_mtx);
	if (hr_closing) {
		mtx_unlock(&hr_mtx);
		return (ENXIO);
	}
	hr_opens++;
	mtx_unlock(&hr_mtx);

	sc = malloc(sizeof(*sc), M_VHID, M_WAITOK | M_ZERO);
	mtx_init(&sc->mtx, "vhid", NULL, MTX_DEF);
	sx_init(&sc->ctl_sx, "vhid ctl");
	sx_init(&sc->intr_sx, "vhid intr");
	sc->state = VHID_IDLE;
	STAILQ_INIT(&sc->evq);
	TAILQ_INIT(&sc->reqs);
	knlist_init_mtx(&sc->rsel.si_note, &sc->mtx);
	TASK_INIT(&sc->attach_task, 0, vhid_attach_task, sc);
	TASK_INIT(&sc->detach_task, 0, vhid_detach_task, sc);

	error = devfs_set_cdevpriv(sc, vhid_dtor);
	if (error != 0)
		vhid_dtor(sc);
	return (error);
}

static int
hr_read(struct cdev *cdev __unused, struct uio *uio, int ioflag)
{
	struct vhid_softc *sc;
	struct vhid_event *ev;
	int error;

	error = devfs_get_cdevpriv((void **)&sc);
	if (error != 0)
		return (error);

	mtx_lock(&sc->mtx);
	while ((ev = STAILQ_FIRST(&sc->evq)) == NULL) {
		if (sc->state != VHID_LIVE) {
			mtx_unlock(&sc->mtx);
			return (ENXIO);
		}
		if ((ioflag & O_NONBLOCK) != 0) {
			mtx_unlock(&sc->mtx);
			return (EWOULDBLOCK);
		}
		error = msleep(&sc->evq, &sc->mtx, PZERO | PCATCH, "vhidrd", 0);
		if (error != 0) {
			mtx_unlock(&sc->mtx);
			return (error);
		}
	}
	if (uio->uio_resid < (ssize_t)(sizeof(ev->hdr) + ev->hdr.he_len)) {
		mtx_unlock(&sc->mtx);
		return (EMSGSIZE);
	}
	STAILQ_REMOVE_HEAD(&sc->evq, link);
	sc->nev--;
	mtx_unlock(&sc->mtx);

	error = uiomove(&ev->hdr, sizeof(ev->hdr), uio);
	if (error == 0 && ev->hdr.he_len > 0)
		error = uiomove(ev->data, ev->hdr.he_len, uio);
	free(ev, M_VHID);
	return (error);
}

static int
hr_write(struct cdev *cdev __unused, struct uio *uio, int ioflag __unused)
{
	struct vhid_softc *sc;
	uint8_t *buf;
	size_t len;
	int error;

	error = devfs_get_cdevpriv((void **)&sc);
	if (error != 0)
		return (error);

	len = uio->uio_resid;
	if (len == 0 || len > HIDRESOURCE_REPORT_MAX)
		return (EINVAL);
	buf = malloc(len, M_VHID, M_WAITOK);
	error = uiomove(buf, len, uio);
	if (error == 0)
		error = vhid_input(sc, buf, len);
	free(buf, M_VHID);
	return (error);
}

static int
hr_ioctl(struct cdev *cdev __unused, u_long cmd, caddr_t data,
    int fflag __unused, struct thread *td)
{
	struct hidresource_input *hi;
	struct vhid_softc *sc;
	int error;

	error = devfs_get_cdevpriv((void **)&sc);
	if (error != 0)
		return (error);

	switch (cmd) {
	case HIDRESOURCE_IOC_CREATE:
		return (vhid_create(sc, (struct hidresource_create *)data, td));
	case HIDRESOURCE_IOC_DESTROY:
		return (vhid_destroy(sc));
	case HIDRESOURCE_IOC_INPUT:
		hi = (struct hidresource_input *)data;
		return (vhid_input(sc, hi->hi_data, hi->hi_len));
	case HIDRESOURCE_IOC_REPLY:
		return (vhid_reply(sc, (struct hidresource_reply *)data));
	case FIONBIO:
	case FIOASYNC:
		return (0);
	default:
		return (ENOTTY);
	}
}

static int
hr_poll(struct cdev *cdev __unused, int events, struct thread *td)
{
	struct vhid_softc *sc;
	int revents;

	if (devfs_get_cdevpriv((void **)&sc) != 0)
		return (POLLNVAL);

	revents = events & (POLLOUT | POLLWRNORM);
	if ((events & (POLLIN | POLLRDNORM)) != 0) {
		mtx_lock(&sc->mtx);
		if (!STAILQ_EMPTY(&sc->evq))
			revents |= events & (POLLIN | POLLRDNORM);
		else
			selrecord(td, &sc->rsel);
		mtx_unlock(&sc->mtx);
	}
	return (revents);
}

static int
hr_kqfilter(struct cdev *cdev __unused, struct knote *kn)
{
	struct vhid_softc *sc;
	int error;

	error = devfs_get_cdevpriv((void **)&sc);
	if (error != 0)
		return (error);
	if (kn->kn_filter != EVFILT_READ)
		return (EINVAL);
	kn->kn_fop = &vhid_kqread_ops;
	kn->kn_hook = sc;
	knlist_add(&sc->rsel.si_note, kn, 0);
	return (0);
}

/* Called with the device's mutex held (the knlist's lock). */
static int
vhid_kqread(struct knote *kn, long hint __unused)
{
	struct vhid_softc *sc = kn->kn_hook;
	struct vhid_event *ev;

	mtx_assert(&sc->mtx, MA_OWNED);
	ev = STAILQ_FIRST(&sc->evq);
	if (ev == NULL)
		return (0);
	kn->kn_data = sizeof(ev->hdr) + ev->hdr.he_len;
	return (1);
}

static void
vhid_kqdetach(struct knote *kn)
{
	struct vhid_softc *sc = kn->kn_hook;

	knlist_remove(&sc->rsel.si_note, kn, 0);
}

/* ---- vhid: the hidbus transport (hid_if.m) ---- */

static int
vhid_probe(device_t dev)
{
	if (device_get_ivars(dev) == NULL)
		return (ENXIO);
	device_set_desc(dev, "IOHIDUserDevice");
	return (BUS_PROBE_NOWILDCARD);
}

static int
vhid_attach(device_t dev)
{
	struct vhid_softc *sc = device_get_ivars(dev);
	device_t child;

	child = device_add_child(dev, "hidbus", DEVICE_UNIT_ANY);
	if (child == NULL)
		return (ENOMEM);
	device_quiet(child);
	device_set_ivars(child, &sc->hdi);
	sc->dev = dev;
	bus_attach_children(dev);
	return (0);
}

static int
vhid_detach(device_t dev)
{
	struct vhid_softc *sc = device_get_ivars(dev);
	int error;

	error = bus_generic_detach(dev);
	if (error != 0)
		return (error);
	sc->dev = NULL;
	return (0);
}

static void
vhid_intr_setup(device_t dev, device_t child __unused, hid_intr_t intr,
    void *context, struct hid_rdesc_info *rdesc)
{
	struct vhid_softc *sc = device_get_ivars(dev);

	/* hidbus asks for polling (HIDBUS_FLAG_CAN_POLL): not possible. */
	if (intr == NULL)
		return;

	sx_xlock(&sc->intr_sx);
	sc->intr = intr;
	sc->intr_ctx = context;
	sc->intr_on = false;
	sx_xunlock(&sc->intr_sx);

	rdesc->rdsize = rdesc->isize;
	rdesc->wrsize = HIDRESOURCE_REPORT_MAX;
	rdesc->grsize = HIDRESOURCE_REPORT_MAX;
	rdesc->srsize = HIDRESOURCE_REPORT_MAX;
}

static void
vhid_intr_unsetup(device_t dev, device_t child __unused)
{
	struct vhid_softc *sc = device_get_ivars(dev);

	sx_xlock(&sc->intr_sx);
	sc->intr = NULL;
	sc->intr_ctx = NULL;
	sc->intr_on = false;
	sx_xunlock(&sc->intr_sx);
}

static int
vhid_intr_start(device_t dev, device_t child __unused)
{
	struct vhid_softc *sc = device_get_ivars(dev);

	sx_xlock(&sc->intr_sx);
	sc->intr_on = true;
	sx_xunlock(&sc->intr_sx);
	(void)vhid_notify(sc, HIDRESOURCE_EVENT_START, NULL, 0);
	return (0);
}

static int
vhid_intr_stop(device_t dev, device_t child __unused)
{
	struct vhid_softc *sc = device_get_ivars(dev);

	sx_xlock(&sc->intr_sx);
	sc->intr_on = false;
	sx_xunlock(&sc->intr_sx);
	(void)vhid_notify(sc, HIDRESOURCE_EVENT_STOP, NULL, 0);
	return (0);
}

/* No polled input from userland (ddb, panic). */
static void
vhid_intr_poll(device_t dev __unused, device_t child __unused)
{
}

static int
vhid_get_rdesc(device_t dev, device_t child __unused, void *buf,
    hid_size_t len)
{
	struct vhid_softc *sc = device_get_ivars(dev);

	if (len < sc->hdi.rdescsize)
		return (EMSGSIZE);
	memcpy(buf, sc->rdesc, sc->hdi.rdescsize);
	return (0);
}

/* hid_if.m: "should not be used"; input arrives through intr. */
static int
vhid_read(device_t dev __unused, device_t child __unused, void *buf __unused,
    hid_size_t maxlen __unused, hid_size_t *actlen __unused)
{
	return (EOPNOTSUPP);
}

/* An output report (keyboard LEDs, rumble): queued, never waited for. */
static int
vhid_write(device_t dev, device_t child __unused, const void *buf,
    hid_size_t len)
{
	struct vhid_softc *sc = device_get_ivars(dev);

	if (HID_IN_POLLING_MODE())
		return (EOPNOTSUPP);
	if (len > HIDRESOURCE_REPORT_MAX)
		return (EINVAL);
	return (vhid_notify(sc, HIDRESOURCE_EVENT_OUTPUT, buf, len));
}

static int
vhid_get_report(device_t dev, device_t child __unused, void *buf,
    hid_size_t maxlen, hid_size_t *actlen, uint8_t type, uint8_t id)
{
	return (vhid_request(device_get_ivars(dev),
	    HIDRESOURCE_EVENT_GET_REPORT, type, id, 0, NULL, 0, buf, maxlen,
	    actlen));
}

static int
vhid_set_report(device_t dev, device_t child __unused, const void *buf,
    hid_size_t len, uint8_t type, uint8_t id)
{
	return (vhid_request(device_get_ivars(dev),
	    HIDRESOURCE_EVENT_SET_REPORT, type, id, 0, buf, len, NULL, 0,
	    NULL));
}

static int
vhid_set_idle(device_t dev, device_t child __unused, uint16_t duration,
    uint8_t id)
{
	return (vhid_request(device_get_ivars(dev),
	    HIDRESOURCE_EVENT_SET_IDLE, 0, id, duration, NULL, 0, NULL, 0,
	    NULL));
}

static int
vhid_set_protocol(device_t dev, device_t child __unused, uint16_t protocol)
{
	return (vhid_request(device_get_ivars(dev),
	    HIDRESOURCE_EVENT_SET_PROTOCOL, 0, 0, protocol, NULL, 0, NULL, 0,
	    NULL));
}

/* No transport-specific ioctls (usbhid's USB_REQUEST and the like). */
static int
vhid_ioctl(device_t dev __unused, device_t child __unused,
    unsigned long cmd __unused, uintptr_t data __unused)
{
	return (EINVAL);
}

static device_method_t vhid_methods[] = {
	DEVMETHOD(device_probe,		vhid_probe),
	DEVMETHOD(device_attach,	vhid_attach),
	DEVMETHOD(device_detach,	vhid_detach),

	DEVMETHOD(hid_intr_setup,	vhid_intr_setup),
	DEVMETHOD(hid_intr_unsetup,	vhid_intr_unsetup),
	DEVMETHOD(hid_intr_start,	vhid_intr_start),
	DEVMETHOD(hid_intr_stop,	vhid_intr_stop),
	DEVMETHOD(hid_intr_poll,	vhid_intr_poll),

	DEVMETHOD(hid_get_rdesc,	vhid_get_rdesc),
	DEVMETHOD(hid_read,		vhid_read),
	DEVMETHOD(hid_write,		vhid_write),
	DEVMETHOD(hid_get_report,	vhid_get_report),
	DEVMETHOD(hid_set_report,	vhid_set_report),
	DEVMETHOD(hid_set_idle,		vhid_set_idle),
	DEVMETHOD(hid_set_protocol,	vhid_set_protocol),
	DEVMETHOD(hid_ioctl,		vhid_ioctl),

	DEVMETHOD_END
};

static driver_t vhid_driver = {
	.name = "vhid",
	.methods = vhid_methods,
	.size = 0,	/* the state is the descriptor's, in the ivars */
};

/* ---- hidresource0: Darwin's IOHIDResource ---- */

static void
hr_identify(driver_t *driver __unused, device_t parent)
{
	if (device_find_child(parent, "hidresource", DEVICE_UNIT_ANY) == NULL)
		(void)BUS_ADD_CHILD(parent, 0, "hidresource", 0);
}

static int
hr_probe(device_t dev)
{
	device_set_desc(dev, "IOHIDResource");
	device_quiet(dev);
	return (BUS_PROBE_NOWILDCARD);
}

static int
hr_attach(device_t dev)
{
	struct make_dev_args args;
	struct taskqueue *tq;
	int error;

	/* One thread: a device's detach runs after its attach. */
	tq = taskqueue_create("hidresource", M_WAITOK,
	    taskqueue_thread_enqueue, &hr_tq);
	hr_tq = tq;
	taskqueue_start_threads(&hr_tq, 1, PWAIT, "hidresource taskq");

	make_dev_args_init(&args);
	args.mda_flags = MAKEDEV_WAITOK | MAKEDEV_CHECKNAME;
	args.mda_devsw = &hr_cdevsw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_WHEEL;
	args.mda_mode = 0600;
	error = make_dev_s(&args, &hr_cdev, "hidresource");
	if (error != 0) {
		taskqueue_free(hr_tq);
		hr_tq = NULL;
		return (error);
	}

	mtx_lock(&hr_mtx);
	hr_dev = dev;
	hr_closing = false;
	mtx_unlock(&hr_mtx);
	return (0);
}

static int
hr_detach(device_t dev __unused)
{
	mtx_lock(&hr_mtx);
	if (hr_opens > 0 || hr_ndevices > 0) {
		mtx_unlock(&hr_mtx);
		return (EBUSY);
	}
	hr_closing = true;
	mtx_unlock(&hr_mtx);

	destroy_dev(hr_cdev);
	hr_cdev = NULL;
	taskqueue_free(hr_tq);
	hr_tq = NULL;
	hr_dev = NULL;
	return (0);
}

static device_method_t hr_methods[] = {
	DEVMETHOD(device_identify,	hr_identify),
	DEVMETHOD(device_probe,		hr_probe),
	DEVMETHOD(device_attach,	hr_attach),
	DEVMETHOD(device_detach,	hr_detach),

	DEVMETHOD_END
};

static driver_t hr_driver = {
	.name = "hidresource",
	.methods = hr_methods,
	.size = 0,
};

DRIVER_MODULE(hidresource, nexus, hr_driver, NULL, NULL);
DRIVER_MODULE(vhid, hidresource, vhid_driver, NULL, NULL);
/* hidbus.c names its four hardware parents; the fifth is declared here. */
DRIVER_MODULE(hidbus, vhid, hidbus_driver, NULL, NULL);
MODULE_DEPEND(vhid, hid, 1, 1, 1);
MODULE_DEPEND(vhid, hidbus, 1, 1, 1);
MODULE_VERSION(vhid, 1);

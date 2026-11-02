/*
 * NextBSD: ng_hci LE hardening (nextbsd/nextbsd-kernel#234). Not upstream.
 *
 * LE ACL buffers. A controller with a separate LE data pool reports it in
 * LE_Read_Buffer_Size (v1 or v2); releng/15.1 ignored the reply, so LE links
 * were charged to the BR/EDR pool from Read_Buffer_Size and a controller with
 * a smaller LE pool could be over-driven. ng_hci_cmds.c now passes the reply
 * here. A non-zero packet count gives the unit its own LE pool, which
 * ng_hci_send_data(), Number_Of_Completed_Packets and ng_hci_free_con() use
 * for LE links; zero means LE shares the ACL pool, which is what happens
 * without an entry. The pool lives in a side table keyed by unit, so
 * ng_hci_unit_t and ng_hci_unit_buff_t keep their layout and
 * NGM_HCI_NODE_GET_BUFFER its reply. ng_hci itself never sends
 * LE_Read_Buffer_Size; whoever initialises the controller (bluetoothd, on the
 * raw hook) does, after Read_Buffer_Size. The LE packet length is recorded
 * only for the log: ng_l2cap splits LE frames at 27 bytes, the smallest LE
 * buffer the spec allows, so no fragment is ever too long.
 *
 * Connection parameters. ng_l2cap accepts a peripheral's L2CAP Connection
 * Parameter Update Request and sends NGM_HCI_LP_LE_CON_UPDATE_REQ
 * (ng_hci_le_lp.h); this file turns it into LE_Connection_Update. A
 * peripheral that uses the link-layer Connection Parameters Request procedure
 * instead raises LE Remote Connection Parameter Request, which is answered
 * here: positively with the peripheral's own values when they are within the
 * spec's limits, otherwise negatively with Unacceptable Connection
 * Parameters. It has to be answered, or the link layer times the procedure
 * out and drops the link. bluetoothd must not answer it too.
 *
 * Nothing here prints at the default debug level except errors.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/endian.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>
#include <netgraph/bluetooth/include/ng_bluetooth.h>
#include <netgraph/bluetooth/include/ng_hci.h>
#include <netgraph/bluetooth/hci/ng_hci_var.h>
#include <netgraph/bluetooth/hci/ng_hci_cmds.h>
#include <netgraph/bluetooth/hci/ng_hci_misc.h>

#include "ng_hci_le_lp.h"
#include "ng_hci_le.h"

#define NG_HCI_STATUS_UNACCEPTABLE_CONN_PARAMS		0x3b

typedef struct {
	u_int16_t	con_handle;
	u_int16_t	interval_min;
	u_int16_t	interval_max;
	u_int16_t	latency;
	u_int16_t	timeout;
	u_int16_t	min_ce_length;
	u_int16_t	max_ce_length;
} __attribute__ ((packed)) ng_hci_le_remote_conn_param_reply_cp;

typedef struct {
	u_int16_t	con_handle;
	u_int8_t	reason;
} __attribute__ ((packed)) ng_hci_le_remote_conn_param_neg_reply_cp;

/*
 * LE buffer pool side table. Entries are added and changed from the owning
 * node's context only, and ng_hci nodes are writers, so the mutex guards just
 * the list itself.
 */

struct ng_hci_le_buff {
	LIST_ENTRY(ng_hci_le_buff)	next;
	ng_hci_unit_p			unit;
	u_int16_t			size;	/* max. LE data packet length */
	u_int16_t			pkts;	/* LE data packets (total) */
	u_int16_t			free;	/* LE data packets (free) */
};

static LIST_HEAD(, ng_hci_le_buff)	ng_hci_le_buffs =
    LIST_HEAD_INITIALIZER(ng_hci_le_buffs);
static struct mtx			ng_hci_le_mtx;
MTX_SYSINIT(ng_hci_le, &ng_hci_le_mtx, "ng_hci LE buffers", MTX_DEF);

static struct ng_hci_le_buff *
ng_hci_le_buffer(ng_hci_unit_p unit)
{
	struct ng_hci_le_buff	*lb;

	mtx_lock(&ng_hci_le_mtx);
	LIST_FOREACH(lb, &ng_hci_le_buffs, next)
		if (lb->unit == unit)
			break;
	mtx_unlock(&ng_hci_le_mtx);

	return (lb);
}

static int
ng_hci_le_link(int link_type)
{
	return (link_type == NG_HCI_LINK_LE_PUBLIC ||
	    link_type == NG_HCI_LINK_LE_RANDOM);
}

/* Packets in flight on the unit's LE links */
static int
ng_hci_le_pending(ng_hci_unit_p unit)
{
	ng_hci_unit_con_p	con;
	int			pending = 0;

	LIST_FOREACH(con, &unit->con_list, next)
		if (ng_hci_le_link(con->link_type))
			pending += con->pending;

	return (pending);
}

int
ng_hci_le_buffer_class(ng_hci_unit_p unit, int link_type)
{
	if (link_type == NG_HCI_LINK_SCO)
		return (NG_HCI_LINK_SCO);
	if (ng_hci_le_link(link_type) && ng_hci_le_buffer(unit) != NULL)
		return (NG_HCI_BUFF_CLASS_LE);

	return (NG_HCI_LINK_ACL);
}

int
ng_hci_le_buffer_avail(ng_hci_unit_p unit)
{
	struct ng_hci_le_buff	*lb = ng_hci_le_buffer(unit);

	return ((lb != NULL) ? lb->free : 0);
}

void
ng_hci_le_buffer_use(ng_hci_unit_p unit, int n)
{
	struct ng_hci_le_buff	*lb = ng_hci_le_buffer(unit);

	if (lb == NULL || n <= 0)
		return;
	lb->free = (n < lb->free) ? lb->free - n : 0;
}

void
ng_hci_le_buffer_free(ng_hci_unit_p unit, int n)
{
	struct ng_hci_le_buff	*lb = ng_hci_le_buffer(unit);

	if (lb == NULL || n <= 0)
		return;
	lb->free = (lb->free + n > lb->pkts) ? lb->pkts : lb->free + n;
}

/*
 * LE_Read_Buffer_Size completed. Packets already in flight on LE links move
 * with the links from one pool to the other, so neither pool leaks.
 */

void
ng_hci_le_buffer_set(ng_hci_unit_p unit, u_int16_t size, u_int16_t pkts)
{
	struct ng_hci_le_buff	*lb = ng_hci_le_buffer(unit);
	int			 pending = ng_hci_le_pending(unit);
	int			 acl_free;

	NG_HCI_INFO(
"%s: %s - LE buffer size=%d, num_pkts=%d%s\n",
		__func__, NG_NODE_NAME(unit->node), size, pkts,
		(pkts == 0 || size == 0) ? " (shared with ACL)" : "");

	if (pkts == 0 || size == 0) {
		if (lb == NULL)
			return;

		mtx_lock(&ng_hci_le_mtx);
		LIST_REMOVE(lb, next);
		mtx_unlock(&ng_hci_le_mtx);
		free(lb, M_NETGRAPH_HCI);

		NG_HCI_BUFF_ACL_AVAIL(unit->buffer, acl_free);
		NG_HCI_BUFF_ACL_USE(unit->buffer,
		    (pending < acl_free) ? pending : acl_free);
		return;
	}

	if (lb == NULL) {
		lb = malloc(sizeof(*lb), M_NETGRAPH_HCI, M_NOWAIT | M_ZERO);
		if (lb == NULL) {
			/* Keep sharing the ACL pool */
			NG_HCI_ERR(
"%s: %s - no memory for the LE buffer descriptor\n",
				__func__, NG_NODE_NAME(unit->node));
			return;
		}
		lb->unit = unit;

		mtx_lock(&ng_hci_le_mtx);
		LIST_INSERT_HEAD(&ng_hci_le_buffs, lb, next);
		mtx_unlock(&ng_hci_le_mtx);

		NG_HCI_BUFF_ACL_FREE(unit->buffer, pending);
	}

	lb->size = size;
	lb->pkts = pkts;
	lb->free = (pending < pkts) ? pkts - pending : 0;
}

/* The unit was cleaned: no connections, so the whole LE pool is free */
void
ng_hci_le_buffer_reset(ng_hci_unit_p unit)
{
	struct ng_hci_le_buff	*lb = ng_hci_le_buffer(unit);

	if (lb != NULL)
		lb->free = lb->pkts;
}

void
ng_hci_le_buffer_destroy(ng_hci_unit_p unit)
{
	struct ng_hci_le_buff	*lb;

	mtx_lock(&ng_hci_le_mtx);
	LIST_FOREACH(lb, &ng_hci_le_buffs, next)
		if (lb->unit == unit)
			break;
	if (lb != NULL)
		LIST_REMOVE(lb, next);
	mtx_unlock(&ng_hci_le_mtx);

	free(lb, M_NETGRAPH_HCI);
}

/*
 * Queue an HCI command built here and send it if the controller can take
 * one. The caller has filled in the whole packet.
 */

static int
ng_hci_le_queue_command(ng_hci_unit_p unit, struct mbuf *m)
{
	if (NG_BT_MBUFQ_FULL(&unit->cmdq)) {
		NG_HCI_ERR(
"%s: %s - command queue is full, dropping OCF=%#x\n",
			__func__, NG_NODE_NAME(unit->node),
			NG_HCI_OCF(le16toh(mtod(m, ng_hci_cmd_pkt_t *)->opcode)));
		NG_BT_MBUFQ_DROP(&unit->cmdq);
		NG_FREE_M(m);
		return (ENOBUFS);
	}

	NG_BT_MBUFQ_ENQUEUE(&unit->cmdq, m);
	if (!(unit->state & NG_HCI_UNIT_COMMAND_PENDING))
		return (ng_hci_send_command(unit));

	return (0);
}

/*
 * NGM_HCI_LP_LE_CON_UPDATE_REQ from ng_l2cap: send LE_Connection_Update with
 * the parameters ng_l2cap accepted. The result is the LE Connection Update
 * Complete event.
 */

int
ng_hci_le_lp_con_update_req(ng_hci_unit_p unit, item_p item, hook_p hook)
{
	struct le_con_update_req {
		ng_hci_cmd_pkt_t		 hdr;
		ng_hci_le_connection_update_cp	 cp;
	} __attribute__ ((packed))		*req = NULL;
	ng_hci_lp_le_con_update_req_ep		*ep = NULL;
	ng_hci_unit_con_p			 con = NULL;
	struct mbuf				*m = NULL;
	int					 error = 0;

	if ((unit->state & NG_HCI_UNIT_READY) != NG_HCI_UNIT_READY) {
		NG_HCI_WARN(
"%s: %s - unit is not ready, state=%#x\n",
			__func__, NG_NODE_NAME(unit->node), unit->state);
		error = ENXIO;
		goto out;
	}

	if (NGI_MSG(item)->header.arglen != sizeof(*ep)) {
		NG_HCI_ALERT(
"%s: %s - invalid LP_LEConUpdateReq message size=%d\n",
			__func__, NG_NODE_NAME(unit->node),
			NGI_MSG(item)->header.arglen);
		error = EMSGSIZE;
		goto out;
	}

	ep = (ng_hci_lp_le_con_update_req_ep *)(NGI_MSG(item)->data);

	con = ng_hci_con_by_handle(unit, ep->con_handle);
	if (con == NULL) {
		NG_HCI_ERR(
"%s: %s - invalid connection handle=%d\n",
			__func__, NG_NODE_NAME(unit->node), ep->con_handle);
		error = EINVAL;
		goto out;
	}

	if (!ng_hci_le_link(con->link_type) || con->state != NG_HCI_CON_OPEN) {
		NG_HCI_ERR(
"%s: %s - not an open LE connection, handle=%d, link_type=%d, state=%d\n",
			__func__, NG_NODE_NAME(unit->node), con->con_handle,
			con->link_type, con->state);
		error = EINVAL;
		goto out;
	}

	if (!ng_hci_le_con_params_valid(ep->interval_min, ep->interval_max,
	    ep->latency, ep->timeout)) {
		NG_HCI_ERR(
"%s: %s - invalid LE connection parameters, handle=%d, interval=%d-%d, " \
"latency=%d, timeout=%d\n",
			__func__, NG_NODE_NAME(unit->node), con->con_handle,
			ep->interval_min, ep->interval_max, ep->latency,
			ep->timeout);
		error = EINVAL;
		goto out;
	}

	MGETHDR(m, M_NOWAIT, MT_DATA);
	if (m == NULL) {
		error = ENOBUFS;
		goto out;
	}

	m->m_pkthdr.len = m->m_len = sizeof(*req);
	req = mtod(m, struct le_con_update_req *);
	req->hdr.type = NG_HCI_CMD_PKT;
	req->hdr.length = sizeof(req->cp);
	req->hdr.opcode = htole16(NG_HCI_OPCODE(NG_HCI_OGF_LE,
			NG_HCI_OCF_LE_CONNECTION_UPDATE));

	req->cp.connection_handle = htole16(con->con_handle);
	req->cp.conn_interval_min = htole16(ep->interval_min);
	req->cp.conn_interval_max = htole16(ep->interval_max);
	req->cp.conn_latency = htole16(ep->latency);
	req->cp.supervision_timeout = htole16(ep->timeout);
	req->cp.minimum_ce_length = 0;
	req->cp.maximum_ce_length = 0;

	NG_HCI_INFO(
"%s: %s - LE_Connection_Update handle=%d, interval=%d-%d, latency=%d, " \
"timeout=%d\n",
		__func__, NG_NODE_NAME(unit->node), con->con_handle,
		ep->interval_min, ep->interval_max, ep->latency, ep->timeout);

	error = ng_hci_le_queue_command(unit, m);
out:
	NG_FREE_ITEM(item);

	return (error);
}

/* LE Connection Update Complete (subevent 0x03) */
int
ng_hci_le_con_update_compl(ng_hci_unit_p unit, struct mbuf *event)
{
	ng_hci_connection_update_complete_ep	*ep = NULL;
	u_int16_t				 h;

	NG_HCI_M_PULLUP(event, sizeof(*ep));
	if (event == NULL)
		return (ENOBUFS);

	ep = mtod(event, ng_hci_connection_update_complete_ep *);
	h = NG_HCI_CON_HANDLE(le16toh(ep->connection_handle));

	if (ep->status != 0) {
		NG_HCI_ERR(
"%s: %s - LE connection update failed, handle=%d, status=%#x\n",
			__func__, NG_NODE_NAME(unit->node), h, ep->status);
	} else {
		NG_HCI_INFO(
"%s: %s - LE connection updated, handle=%d, interval=%d, latency=%d, " \
"timeout=%d\n",
			__func__, NG_NODE_NAME(unit->node), h,
			le16toh(ep->conn_interval), le16toh(ep->conn_latency),
			le16toh(ep->supervision_timeout));
	}

	NG_FREE_M(event);

	return (0);
}

/*
 * LE Read Remote Features Complete (subevent 0x04): record the peer's LE
 * features in the neighbor cache, as Read_Remote_Supported_Features does for
 * BR/EDR. The entry is keyed by the LE address type, so the two kinds of
 * feature bits never mix.
 */

int
ng_hci_le_read_remote_features_compl(ng_hci_unit_p unit, struct mbuf *event)
{
	ng_hci_le_read_remote_features_ep	*ep = NULL;
	ng_hci_unit_con_p			 con = NULL;
	ng_hci_neighbor_p			 n = NULL;
	u_int16_t				 h;
	int					 error = 0;

	NG_HCI_M_PULLUP(event, sizeof(*ep));
	if (event == NULL)
		return (ENOBUFS);

	ep = mtod(event, ng_hci_le_read_remote_features_ep *);
	h = NG_HCI_CON_HANDLE(le16toh(ep->connection_handle));

	if (ep->status != 0) {
		NG_HCI_ERR(
"%s: %s - failed to read remote LE features, handle=%d, status=%#x\n",
			__func__, NG_NODE_NAME(unit->node), h, ep->status);
		goto out;
	}

	con = ng_hci_con_by_handle(unit, h);
	if (con == NULL || !ng_hci_le_link(con->link_type)) {
		NG_HCI_ALERT(
"%s: %s - invalid LE connection handle=%d\n",
			__func__, NG_NODE_NAME(unit->node), h);
		error = ENOENT;
		goto out;
	}

	n = ng_hci_get_neighbor(unit, &con->bdaddr, con->link_type);
	if (n == NULL) {
		n = ng_hci_new_neighbor(unit);
		if (n == NULL) {
			error = ENOMEM;
			goto out;
		}

		bcopy(&con->bdaddr, &n->bdaddr, sizeof(n->bdaddr));
		n->addrtype = con->link_type;
	} else
		getmicrotime(&n->updated);

	bcopy(ep->features, n->features, sizeof(n->features));
out:
	NG_FREE_M(event);

	return (error);
}

/*
 * LE Remote Connection Parameter Request (subevent 0x06). Always answered,
 * since an unanswered request ends in an LL response timeout and a dropped
 * link.
 */

int
ng_hci_le_remote_conn_param_req(ng_hci_unit_p unit, struct mbuf *event)
{
	struct rcp_reply {
		ng_hci_cmd_pkt_t			 hdr;
		ng_hci_le_remote_conn_param_reply_cp	 cp;
	} __attribute__ ((packed))			*rep = NULL;
	struct rcp_neg_reply {
		ng_hci_cmd_pkt_t				 hdr;
		ng_hci_le_remote_conn_param_neg_reply_cp	 cp;
	} __attribute__ ((packed))			*neg = NULL;
	ng_hci_le_remote_conn_param_ep		*ep = NULL;
	struct mbuf				*m = NULL;
	u_int16_t				 h, imin, imax, lat, timo;
	int					 ok;

	NG_HCI_M_PULLUP(event, sizeof(*ep));
	if (event == NULL)
		return (ENOBUFS);

	ep = mtod(event, ng_hci_le_remote_conn_param_ep *);
	h = NG_HCI_CON_HANDLE(le16toh(ep->connection_handle));
	imin = le16toh(ep->interval_min);
	imax = le16toh(ep->interval_max);
	lat = le16toh(ep->latency);
	timo = le16toh(ep->timeout);

	ok = ng_hci_le_con_params_valid(imin, imax, lat, timo);

	NG_HCI_INFO(
"%s: %s - LE remote connection parameter request, handle=%d, " \
"interval=%d-%d, latency=%d, timeout=%d, %s\n",
		__func__, NG_NODE_NAME(unit->node), h, imin, imax, lat, timo,
		ok ? "accepted" : "rejected");

	MGETHDR(m, M_NOWAIT, MT_DATA);
	if (m == NULL) {
		NG_FREE_M(event);
		return (ENOBUFS);
	}

	if (ok) {
		m->m_pkthdr.len = m->m_len = sizeof(*rep);
		rep = mtod(m, struct rcp_reply *);
		rep->hdr.type = NG_HCI_CMD_PKT;
		rep->hdr.length = sizeof(rep->cp);
		rep->hdr.opcode = htole16(NG_HCI_OPCODE(NG_HCI_OGF_LE,
			NG_HCI_OCF_LE_REMOTE_CONN_PARAM_REQ_REPLY));
		rep->cp.con_handle = htole16(h);
		rep->cp.interval_min = htole16(imin);
		rep->cp.interval_max = htole16(imax);
		rep->cp.latency = htole16(lat);
		rep->cp.timeout = htole16(timo);
		rep->cp.min_ce_length = 0;
		rep->cp.max_ce_length = 0;
	} else {
		NG_HCI_ERR(
"%s: %s - rejected LE connection parameters, handle=%d, interval=%d-%d, " \
"latency=%d, timeout=%d\n",
			__func__, NG_NODE_NAME(unit->node), h, imin, imax, lat,
			timo);

		m->m_pkthdr.len = m->m_len = sizeof(*neg);
		neg = mtod(m, struct rcp_neg_reply *);
		neg->hdr.type = NG_HCI_CMD_PKT;
		neg->hdr.length = sizeof(neg->cp);
		neg->hdr.opcode = htole16(NG_HCI_OPCODE(NG_HCI_OGF_LE,
			NG_HCI_OCF_LE_REMOTE_CONN_PARAM_REQ_NEG_REPLY));
		neg->cp.con_handle = htole16(h);
		neg->cp.reason = NG_HCI_STATUS_UNACCEPTABLE_CONN_PARAMS;
	}

	NG_FREE_M(event);

	return (ng_hci_le_queue_command(unit, m));
}

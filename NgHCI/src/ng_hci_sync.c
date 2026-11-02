/*
 * NextBSD: ng_hci synchronous (SCO/eSCO) connections
 * (nextbsd/nextbsd-kernel#237). Not upstream.
 *
 * Why. HFP needs eSCO, and its wide-band speech (mSBC) needs the transparent
 * air mode, but releng/15.1's ng_hci only knows Add_SCO_Connection (HV1-HV3,
 * CVSD) and Accept_Connection_Request, and drops Synchronous_Connection_
 * Complete (0x2c) and _Changed (0x2d) as unknown events. A headset that asks
 * for eSCO (Connection_Request with link type 2, which ng_hci read as LE
 * Public) could not be answered at all.
 *
 * Outgoing. ng_btsocket_sco sends NGM_HCI_LP_SYNC_CON_REQ (ng_hci_sync_lp.h)
 * for a socket that set SO_SCO_SYNC_PARAMS; this file sends
 * Setup_Synchronous_Connection over the open ACL link to the peer. A socket
 * that set nothing still sends NGM_HCI_LP_CON_REQ and gets Add_SCO_Connection,
 * as before.
 *
 * Incoming. Connection_Request for SCO (0) or eSCO (2) is one SCO-class
 * connection descriptor (NG_HCI_LINK_SCO) in ng_hci, so the sco hook sees an
 * NGM_HCI_LP_CON_IND as before. Its answer is always a synchronous command:
 * Accept_Synchronous_Connection_Request with the socket's parameters
 * (NGM_HCI_LP_SYNC_CON_RSP) or the CVSD defaults (a plain NGM_HCI_LP_CON_RSP),
 * or Reject_Synchronous_Connection_Request. Accept_Connection_Request must not
 * answer an eSCO request, and every controller since Bluetooth 1.2 takes the
 * synchronous commands for SCO too (NextBSD's hardware baseline is 2012).
 *
 * Completion. Synchronous_Connection_Complete opens the descriptor the way
 * Connection_Complete does, and sends NGM_HCI_LP_SYNC_CON_INFO (link type,
 * air mode, interval, window, packet lengths) and then NGM_HCI_LP_CON_CFM on
 * the sco hook. _Changed sends NGM_HCI_LP_SYNC_CON_INFO again. A failed
 * Command_Status for the setup or accept fails the connection at once, so a
 * caller falling back from mSBC to CVSD does not wait for the timeout.
 *
 * SCO data. Controllers report Number_Of_Completed_Packets for synchronous
 * handles only when the host enables SCO flow control, which nothing here
 * does. releng/15.1 charged every SCO packet to the controller's SCO pool and
 * never got it back, so SCO transmission stopped after sco_pkts packets (and
 * never started on a controller that reports none). SCO packets are now
 * handed to the driver as they come, the way other stacks do; the USB
 * isochronous pipe paces them.
 *
 * The controller's event mask must leave events 0x2c and 0x2d enabled (the
 * default mask does). Nothing here prints at the default debug level except
 * errors.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/endian.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/queue.h>
#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>
#include <netgraph/bluetooth/include/ng_bluetooth.h>
#include <netgraph/bluetooth/include/ng_hci.h>
#include <netgraph/bluetooth/hci/ng_hci_var.h>
#include <netgraph/bluetooth/hci/ng_hci_cmds.h>
#include <netgraph/bluetooth/hci/ng_hci_ulpi.h>
#include <netgraph/bluetooth/hci/ng_hci_misc.h>

#include "ng_hci_sync_lp.h"
#include "ng_hci_sync.h"

/* Command parameters, Core 5.x Vol 4 Part E 7.1.26-7.1.28 */
typedef struct {
	u_int16_t	con_handle;	/* ACL connection handle */
	u_int32_t	tx_bandwidth;
	u_int32_t	rx_bandwidth;
	u_int16_t	max_latency;
	u_int16_t	voice_setting;
	u_int8_t	retrans_effort;
	u_int16_t	pkt_type;
} __attribute__ ((packed)) ng_hci_setup_sync_con_cp;

typedef struct {
	bdaddr_t	bdaddr;
	u_int32_t	tx_bandwidth;
	u_int32_t	rx_bandwidth;
	u_int16_t	max_latency;
	u_int16_t	voice_setting;
	u_int8_t	retrans_effort;
	u_int16_t	pkt_type;
} __attribute__ ((packed)) ng_hci_accept_sync_con_req_cp;

typedef struct {
	bdaddr_t	bdaddr;
	u_int8_t	reason;
} __attribute__ ((packed)) ng_hci_reject_sync_con_req_cp;

/* Events, Core 5.x Vol 4 Part E 7.7.35 and 7.7.36 */
typedef struct {
	u_int8_t	status;
	u_int16_t	con_handle;
	bdaddr_t	bdaddr;
	u_int8_t	link_type;
	u_int8_t	tx_interval;
	u_int8_t	retrans_window;
	u_int16_t	rx_pkt_len;
	u_int16_t	tx_pkt_len;
	u_int8_t	air_mode;
} __attribute__ ((packed)) ng_hci_sync_con_compl_ep;

typedef struct {
	u_int8_t	status;
	u_int16_t	con_handle;
	u_int8_t	tx_interval;
	u_int8_t	retrans_window;
	u_int16_t	rx_pkt_len;
	u_int16_t	tx_pkt_len;
} __attribute__ ((packed)) ng_hci_sync_con_changed_ep;

/* Reject reasons the synchronous reject accepts */
#define NG_HCI_SYNC_REJECT_MIN		0x0d	/* limited resources */
#define NG_HCI_SYNC_REJECT_MAX		0x0f	/* unacceptable BD_ADDR */

static const ng_hci_sync_params_t ng_hci_sync_defaults = {
	.tx_bandwidth =		NG_HCI_SYNC_DEFAULT_BANDWIDTH,
	.rx_bandwidth =		NG_HCI_SYNC_DEFAULT_BANDWIDTH,
	.max_latency =		NG_HCI_SYNC_DEFAULT_LATENCY,
	.voice_setting =	NG_HCI_SYNC_DEFAULT_VOICE,
	.pkt_type =		NG_HCI_SYNC_DEFAULT_PKT_TYPE,
	.retrans_effort =	NG_HCI_SYNC_DEFAULT_RETRANS,
};

/* Queue an HCI command built in m and send it if the controller is idle */
static int
ng_hci_sync_queue_command(ng_hci_unit_p unit, struct mbuf *m)
{
	NG_BT_MBUFQ_ENQUEUE(&unit->cmdq, m);
	if (!(unit->state & NG_HCI_UNIT_COMMAND_PENDING))
		return (ng_hci_send_command(unit));

	return (0);
}

/* Allocate a command packet with room for len bytes of parameters */
static struct mbuf *
ng_hci_sync_command(u_int16_t ocf, int len)
{
	ng_hci_cmd_pkt_t	*hdr;
	struct mbuf		*m;

	MGETHDR(m, M_NOWAIT, MT_DATA);
	if (m == NULL)
		return (NULL);

	m->m_pkthdr.len = m->m_len = sizeof(*hdr) + len;
	hdr = mtod(m, ng_hci_cmd_pkt_t *);
	hdr->type = NG_HCI_CMD_PKT;
	hdr->opcode = htole16(NG_HCI_OPCODE(NG_HCI_OGF_LINK_CONTROL, ocf));
	hdr->length = len;

	return (m);
}

/*
 * Fail an outgoing request that never got a connection descriptor: send
 * NGM_HCI_LP_CON_CFM with the HCI status straight to the sco hook, so the
 * socket fails now rather than at its timeout.
 */
static void
ng_hci_sync_refuse(ng_hci_unit_p unit, bdaddr_p bdaddr, int status)
{
	struct ng_mesg		*msg = NULL;
	ng_hci_lp_con_cfm_ep	*ep = NULL;
	int			 error;

	if (unit->sco == NULL || NG_HOOK_NOT_VALID(unit->sco))
		return;

	NG_MKMESSAGE(msg, NGM_HCI_COOKIE, NGM_HCI_LP_CON_CFM, sizeof(*ep),
		M_NOWAIT);
	if (msg == NULL)
		return;

	ep = (ng_hci_lp_con_cfm_ep *)(msg->data);
	ep->status = status;
	ep->link_type = NG_HCI_LINK_SCO;
	ep->con_handle = 0;
	bcopy(bdaddr, &ep->bdaddr, sizeof(ep->bdaddr));

	NG_SEND_MSG_HOOK(error, unit->node, msg, unit->sco, 0);
}

/* Send NGM_HCI_LP_SYNC_CON_INFO to the sco hook */
static void
ng_hci_sync_send_info(ng_hci_unit_p unit, const ng_hci_lp_sync_con_info_ep *info)
{
	struct ng_mesg	*msg = NULL;
	int		 error;

	if (unit->sco == NULL || NG_HOOK_NOT_VALID(unit->sco))
		return;

	NG_MKMESSAGE(msg, NGM_HCI_COOKIE, NGM_HCI_LP_SYNC_CON_INFO,
		sizeof(*info), M_NOWAIT);
	if (msg == NULL)
		return;

	bcopy(info, msg->data, sizeof(*info));
	NG_SEND_MSG_HOOK(error, unit->node, msg, unit->sco, 0);
}

/* The pending SCO-class descriptor for bdaddr, or NULL */
static ng_hci_unit_con_p
ng_hci_sync_pending_con(ng_hci_unit_p unit, bdaddr_p bdaddr, int state)
{
	ng_hci_unit_con_p	con;

	LIST_FOREACH(con, &unit->con_list, next)
		if (con->link_type == NG_HCI_LINK_SCO &&
		    con->state == state &&
		    bcmp(&con->bdaddr, bdaddr, sizeof(bdaddr_t)) == 0)
			return (con);

	return (NULL);
}

/*
 * NGM_HCI_LP_SYNC_CON_REQ: Setup_Synchronous_Connection. Mirrors
 * ng_hci_lp_sco_con_req() in ng_hci_ulpi.c.
 */
int
ng_hci_sync_lp_con_req(ng_hci_unit_p unit, item_p item, hook_p hook)
{
	ng_hci_lp_sync_con_req_ep	*ep = NULL;
	ng_hci_setup_sync_con_cp	*cp = NULL;
	ng_hci_unit_con_p		 acl_con = NULL, sco_con = NULL;
	struct mbuf			*m = NULL;
	int				 error = 0;

	if (NGI_MSG(item)->header.arglen != sizeof(*ep)) {
		NG_HCI_ALERT(
"%s: %s - invalid LP_SyncConnectReq message size=%d\n",
			__func__, NG_NODE_NAME(unit->node),
			NGI_MSG(item)->header.arglen);
		error = EMSGSIZE;
		goto out;
	}

	ep = (ng_hci_lp_sync_con_req_ep *)(NGI_MSG(item)->data);

	if (hook != unit->sco) {
		NG_HCI_WARN(
"%s: %s - LP_SyncConnectReq came from wrong hook=%p\n",
			__func__, NG_NODE_NAME(unit->node), hook);
		error = EINVAL;
		goto out;
	}

	if ((unit->state & NG_HCI_UNIT_READY) != NG_HCI_UNIT_READY) {
		NG_HCI_WARN(
"%s: %s - unit is not ready, state=%#x\n",
			__func__, NG_NODE_NAME(unit->node), unit->state);
		ng_hci_sync_refuse(unit, &ep->bdaddr, 0x0c);
		error = ENXIO;
		goto out;
	}

	/* A synchronous link rides on the open ACL link to the peer */
	LIST_FOREACH(acl_con, &unit->con_list, next)
		if (acl_con->link_type == NG_HCI_LINK_ACL &&
		    acl_con->state == NG_HCI_CON_OPEN &&
		    bcmp(&acl_con->bdaddr, &ep->bdaddr, sizeof(bdaddr_t)) == 0)
			break;

	if (acl_con == NULL) {
		NG_HCI_INFO(
"%s: %s - No open ACL connection to bdaddr=%x:%x:%x:%x:%x:%x\n",
			__func__, NG_NODE_NAME(unit->node),
			ep->bdaddr.b[5], ep->bdaddr.b[4], ep->bdaddr.b[3],
			ep->bdaddr.b[2], ep->bdaddr.b[1], ep->bdaddr.b[0]);
		ng_hci_sync_refuse(unit, &ep->bdaddr, 0x02);
		error = ENOENT;
		goto out;
	}

	/* As for SCO: one synchronous link to a peer is set up at a time */
	sco_con = ng_hci_sync_pending_con(unit, &ep->bdaddr,
			NG_HCI_CON_W4_LP_CON_RSP);
	if (sco_con != NULL) {
		error = EALREADY;
		goto out;
	}

	sco_con = ng_hci_sync_pending_con(unit, &ep->bdaddr,
			NG_HCI_CON_W4_CONN_COMPLETE);
	if (sco_con != NULL) {
		sco_con->flags |= NG_HCI_CON_NOTIFY_SCO;
		goto out;
	}

	sco_con = ng_hci_new_con(unit, NG_HCI_LINK_SCO);
	if (sco_con == NULL) {
		error = ENOMEM;
		goto out;
	}

	bcopy(&ep->bdaddr, &sco_con->bdaddr, sizeof(sco_con->bdaddr));

	m = ng_hci_sync_command(NG_HCI_OCF_SETUP_SYNC_CON, sizeof(*cp));
	if (m == NULL) {
		ng_hci_free_con(sco_con);
		error = ENOBUFS;
		goto out;
	}

	cp = (ng_hci_setup_sync_con_cp *)(mtod(m, ng_hci_cmd_pkt_t *) + 1);
	cp->con_handle = htole16(acl_con->con_handle);
	cp->tx_bandwidth = htole32(ep->params.tx_bandwidth);
	cp->rx_bandwidth = htole32(ep->params.rx_bandwidth);
	cp->max_latency = htole16(ep->params.max_latency);
	cp->voice_setting = htole16(ep->params.voice_setting);
	cp->retrans_effort = ep->params.retrans_effort;
	cp->pkt_type = htole16(ep->params.pkt_type);

	NG_HCI_INFO(
"%s: %s - Setup_Synchronous_Connection, acl handle=%d, voice=%#x, " \
"pkt_type=%#x, latency=%d, retrans=%d\n",
		__func__, NG_NODE_NAME(unit->node), acl_con->con_handle,
		ep->params.voice_setting, ep->params.pkt_type,
		ep->params.max_latency, ep->params.retrans_effort);

	sco_con->flags |= NG_HCI_CON_NOTIFY_SCO;
	sco_con->state = NG_HCI_CON_W4_CONN_COMPLETE;
	ng_hci_con_timeout(sco_con);

	error = ng_hci_sync_queue_command(unit, m);
out:
	NG_FREE_ITEM(item);

	return (error);
}

/*
 * Answer a synchronous Connection_Request. con is in NG_HCI_CON_W4_LP_CON_RSP
 * with its timeout removed (ng_hci_lp_con_rsp() and ng_hci_sync_lp_con_rsp()).
 * params NULL means the defaults.
 */
int
ng_hci_sync_con_answer(ng_hci_unit_p unit, ng_hci_unit_con_p con, int status,
    const ng_hci_sync_params_t *params, hook_p hook)
{
	ng_hci_accept_sync_con_req_cp	*acc = NULL;
	ng_hci_reject_sync_con_req_cp	*rej = NULL;
	struct mbuf			*m = NULL;

	if (status == 0) {
		if (params == NULL)
			params = &ng_hci_sync_defaults;

		m = ng_hci_sync_command(NG_HCI_OCF_ACCEPT_SYNC_CON_REQ,
			sizeof(*acc));
		if (m == NULL)
			return (ENOBUFS);

		acc = (ng_hci_accept_sync_con_req_cp *)
			(mtod(m, ng_hci_cmd_pkt_t *) + 1);
		bcopy(&con->bdaddr, &acc->bdaddr, sizeof(acc->bdaddr));
		acc->tx_bandwidth = htole32(params->tx_bandwidth);
		acc->rx_bandwidth = htole32(params->rx_bandwidth);
		acc->max_latency = htole16(params->max_latency);
		acc->voice_setting = htole16(params->voice_setting);
		acc->retrans_effort = params->retrans_effort;
		acc->pkt_type = htole16(params->pkt_type);

		if (hook == unit->acl)
			con->flags |= NG_HCI_CON_NOTIFY_ACL;
		else
			con->flags |= NG_HCI_CON_NOTIFY_SCO;

		con->state = NG_HCI_CON_W4_CONN_COMPLETE;
		ng_hci_con_timeout(con);
	} else {
		m = ng_hci_sync_command(NG_HCI_OCF_REJECT_SYNC_CON_REQ,
			sizeof(*rej));
		if (m == NULL)
			return (ENOBUFS);

		rej = (ng_hci_reject_sync_con_req_cp *)
			(mtod(m, ng_hci_cmd_pkt_t *) + 1);
		bcopy(&con->bdaddr, &rej->bdaddr, sizeof(rej->bdaddr));
		if (status < NG_HCI_SYNC_REJECT_MIN ||
		    status > NG_HCI_SYNC_REJECT_MAX)
			status = NG_HCI_SYNC_REJECT_MIN;
		rej->reason = status;

		ng_hci_free_con(con);
	}

	return (ng_hci_sync_queue_command(unit, m));
}

/*
 * NGM_HCI_LP_SYNC_CON_RSP: answer with the caller's parameters. Mirrors
 * ng_hci_lp_con_rsp() in ng_hci_ulpi.c.
 */
int
ng_hci_sync_lp_con_rsp(ng_hci_unit_p unit, item_p item, hook_p hook)
{
	ng_hci_lp_sync_con_rsp_ep	*ep = NULL;
	ng_hci_unit_con_p		 con = NULL;
	int				 error = 0;

	if ((unit->state & NG_HCI_UNIT_READY) != NG_HCI_UNIT_READY) {
		NG_HCI_WARN(
"%s: %s - unit is not ready, state=%#x\n",
			__func__, NG_NODE_NAME(unit->node), unit->state);
		error = ENXIO;
		goto out;
	}

	if (NGI_MSG(item)->header.arglen != sizeof(*ep)) {
		NG_HCI_ALERT(
"%s: %s - invalid LP_SyncConnectRsp message size=%d\n",
			__func__, NG_NODE_NAME(unit->node),
			NGI_MSG(item)->header.arglen);
		error = EMSGSIZE;
		goto out;
	}

	ep = (ng_hci_lp_sync_con_rsp_ep *)(NGI_MSG(item)->data);

	con = ng_hci_sync_pending_con(unit, &ep->bdaddr,
			NG_HCI_CON_W4_LP_CON_RSP);
	if (con == NULL)
		con = ng_hci_sync_pending_con(unit, &ep->bdaddr,
				NG_HCI_CON_W4_CONN_COMPLETE);
	if (con == NULL) {
		/* Reject for non-existing connection is fine */
		error = (ep->status == 0)? ENOENT : 0;
		goto out;
	}

	if ((error = ng_hci_con_untimeout(con)) != 0)
		goto out;

	switch (con->state) {
	case NG_HCI_CON_W4_LP_CON_RSP:
		error = ng_hci_sync_con_answer(unit, con, ep->status,
				&ep->params, hook);
		break;

	case NG_HCI_CON_W4_CONN_COMPLETE:
		/* Already answered: join it, as ng_hci_lp_con_rsp() does */
		ng_hci_con_timeout(con);
		if (ep->status == 0)
			con->flags |= NG_HCI_CON_NOTIFY_SCO;
		else
			error = EPERM;
		break;
	}
out:
	NG_FREE_ITEM(item);

	return (error);
}

/* Synchronous_Connection_Complete. Mirrors con_compl() in ng_hci_evnt.c. */
int
ng_hci_sync_con_compl(ng_hci_unit_p unit, struct mbuf *event)
{
	ng_hci_sync_con_compl_ep	*ep = NULL;
	ng_hci_lp_sync_con_info_ep	 info;
	ng_hci_unit_con_p		 con = NULL;
	int				 error = 0;

	NG_HCI_M_PULLUP(event, sizeof(*ep));
	if (event == NULL)
		return (ENOBUFS);

	ep = mtod(event, ng_hci_sync_con_compl_ep *);

	NG_HCI_INFO(
"%s: %s - Synchronous_Connection_Complete, status=%#x, handle=%d, " \
"link_type=%d, air_mode=%d, interval=%d, window=%d, rx=%d, tx=%d\n",
		__func__, NG_NODE_NAME(unit->node), ep->status,
		NG_HCI_CON_HANDLE(le16toh(ep->con_handle)), ep->link_type,
		ep->air_mode, ep->tx_interval, ep->retrans_window,
		le16toh(ep->rx_pkt_len), le16toh(ep->tx_pkt_len));

	con = ng_hci_sync_pending_con(unit, &ep->bdaddr,
			NG_HCI_CON_W4_CONN_COMPLETE);

	if (con == NULL) {
		/* Not ours: set up through the raw hook, or timed out */
		if (ep->status != 0)
			goto out;

		con = ng_hci_new_con(unit, NG_HCI_LINK_SCO);
		if (con == NULL) {
			error = ENOMEM;
			goto out;
		}

		bcopy(&ep->bdaddr, &con->bdaddr, sizeof(con->bdaddr));
	} else if ((error = ng_hci_con_untimeout(con)) != 0)
		goto out;

	con->con_handle = NG_HCI_CON_HANDLE(le16toh(ep->con_handle));

	if (ep->status == 0 && (con->flags & NG_HCI_CON_NOTIFY_SCO)) {
		bzero(&info, sizeof(info));
		info.status = ep->status;
		info.changed = 0;
		info.con_handle = con->con_handle;
		bcopy(&ep->bdaddr, &info.bdaddr, sizeof(info.bdaddr));
		info.link_type = ep->link_type;
		info.air_mode = ep->air_mode;
		info.tx_interval = ep->tx_interval;
		info.retrans_window = ep->retrans_window;
		info.rx_pkt_len = le16toh(ep->rx_pkt_len);
		info.tx_pkt_len = le16toh(ep->tx_pkt_len);
		ng_hci_sync_send_info(unit, &info);
	}

	ng_hci_lp_con_cfm(con, ep->status);

	if (ep->status != 0)
		ng_hci_free_con(con);
	else
		con->state = NG_HCI_CON_OPEN;
out:
	NG_FREE_M(event);

	return (error);
}

/* Synchronous_Connection_Changed */
int
ng_hci_sync_con_changed(ng_hci_unit_p unit, struct mbuf *event)
{
	ng_hci_sync_con_changed_ep	*ep = NULL;
	ng_hci_lp_sync_con_info_ep	 info;
	ng_hci_unit_con_p		 con = NULL;
	u_int16_t			 h;

	NG_HCI_M_PULLUP(event, sizeof(*ep));
	if (event == NULL)
		return (ENOBUFS);

	ep = mtod(event, ng_hci_sync_con_changed_ep *);
	h = NG_HCI_CON_HANDLE(le16toh(ep->con_handle));

	NG_HCI_INFO(
"%s: %s - Synchronous_Connection_Changed, status=%#x, handle=%d, " \
"interval=%d, window=%d, rx=%d, tx=%d\n",
		__func__, NG_NODE_NAME(unit->node), ep->status, h,
		ep->tx_interval, ep->retrans_window,
		le16toh(ep->rx_pkt_len), le16toh(ep->tx_pkt_len));

	if (ep->status == 0) {
		con = ng_hci_con_by_handle(unit, h);
		if (con != NULL && con->link_type == NG_HCI_LINK_SCO &&
		    con->state == NG_HCI_CON_OPEN) {
			bzero(&info, sizeof(info));
			info.status = 0;
			info.changed = 1;
			info.con_handle = h;
			bcopy(&con->bdaddr, &info.bdaddr, sizeof(info.bdaddr));
			info.link_type = 0xff;
			info.air_mode = 0xff;
			info.tx_interval = ep->tx_interval;
			info.retrans_window = ep->retrans_window;
			info.rx_pkt_len = le16toh(ep->rx_pkt_len);
			info.tx_pkt_len = le16toh(ep->tx_pkt_len);
			ng_hci_sync_send_info(unit, &info);
		}
	}

	NG_FREE_M(event);

	return (0);
}

/*
 * Command_Status for the synchronous commands. A setup or accept the
 * controller refused never produces Synchronous_Connection_Complete, so fail
 * the waiting descriptor now. mcp holds the command's parameters; the caller
 * frees it.
 */
int
ng_hci_sync_command_status(ng_hci_unit_p unit, u_int16_t ocf, u_int8_t status,
    struct mbuf *mcp)
{
	ng_hci_unit_con_p	con = NULL, acl_con = NULL;
	bdaddr_t		bdaddr;
	u_int16_t		h;

	if (status == 0 || mcp == NULL)
		return (0);

	switch (ocf) {
	case NG_HCI_OCF_SETUP_SYNC_CON:
		if (mcp->m_pkthdr.len < (int)sizeof(h))
			return (EINVAL);
		m_copydata(mcp, 0, sizeof(h), (caddr_t) &h);
		acl_con = ng_hci_con_by_handle(unit,
				NG_HCI_CON_HANDLE(le16toh(h)));
		if (acl_con == NULL)
			return (ENOENT);
		bcopy(&acl_con->bdaddr, &bdaddr, sizeof(bdaddr));
		break;

	case NG_HCI_OCF_ACCEPT_SYNC_CON_REQ:
		if (mcp->m_pkthdr.len < (int)sizeof(bdaddr))
			return (EINVAL);
		m_copydata(mcp, 0, sizeof(bdaddr), (caddr_t) &bdaddr);
		break;

	default:
		return (0);
	}

	NG_HCI_ERR(
"%s: %s - synchronous command ocf=%#x failed, status=%#x, " \
"bdaddr=%x:%x:%x:%x:%x:%x\n",
		__func__, NG_NODE_NAME(unit->node), ocf, status,
		bdaddr.b[5], bdaddr.b[4], bdaddr.b[3],
		bdaddr.b[2], bdaddr.b[1], bdaddr.b[0]);

	con = ng_hci_sync_pending_con(unit, &bdaddr,
			NG_HCI_CON_W4_CONN_COMPLETE);
	if (con == NULL)
		return (0);

	/* Timed out already: the timeout message deals with it */
	if (ng_hci_con_untimeout(con) != 0)
		return (0);

	ng_hci_lp_con_cfm(con, status);
	ng_hci_free_con(con);

	return (0);
}

/*
 * After ng_hci_send_data() has handed SCO packets to the driver. Without SCO
 * flow control nothing ever completes them, so do not let the per-connection
 * count of packets in flight grow without bound.
 */
void
ng_hci_sync_sco_sent(ng_hci_unit_p unit)
{
	ng_hci_unit_con_p	con;

	LIST_FOREACH(con, &unit->con_list, next)
		if (con->link_type == NG_HCI_LINK_SCO)
			con->pending = 0;
}

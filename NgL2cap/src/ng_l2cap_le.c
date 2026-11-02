/*
 * NextBSD: ng_l2cap LE hardening (nextbsd/nextbsd-kernel#234). Not upstream.
 *
 * A peripheral asks for new connection parameters with an L2CAP Connection
 * Parameter Update Request on the LE signalling channel (CID 5). A mouse asks
 * for a short interval, 7.5 ms or so, and some peripherals disconnect when the
 * request is accepted but never applied, which is what releng/15.1 did. Once
 * ng_l2cap_evnt.c has accepted the request, ask ng_hci to apply it with
 * LE_Connection_Update (NGM_HCI_LP_LE_CON_UPDATE_REQ, NgHCI's
 * ng_hci_le_lp.h). The outcome is the LE Connection Update Complete event,
 * which ng_hci logs.
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
#include <netgraph/bluetooth/include/ng_l2cap.h>
#include <netgraph/bluetooth/l2cap/ng_l2cap_var.h>

#include "ng_hci_le_lp.h"
#include "ng_l2cap_le.h"

int
ng_l2cap_lp_le_con_update(ng_l2cap_con_p con, u_int16_t interval_min,
    u_int16_t interval_max, u_int16_t latency, u_int16_t timeout)
{
	ng_l2cap_p			 l2cap = con->l2cap;
	struct ng_mesg			*msg = NULL;
	ng_hci_lp_le_con_update_req_ep	*ep = NULL;
	int				 error = 0;

	if (con->linktype == NG_HCI_LINK_ACL || con->state != NG_L2CAP_CON_OPEN)
		return (EINVAL);

	/* Check if lower layer protocol is still connected */
	if (l2cap->hci == NULL || NG_HOOK_NOT_VALID(l2cap->hci)) {
		NG_L2CAP_ERR(
"%s: %s - hook \"%s\" is not connected or valid\n",
			__func__, NG_NODE_NAME(l2cap->node), NG_L2CAP_HOOK_HCI);

		return (ENOTCONN);
	}

	NG_MKMESSAGE(msg, NGM_HCI_COOKIE, NGM_HCI_LP_LE_CON_UPDATE_REQ,
		sizeof(*ep), M_NOWAIT);
	if (msg == NULL)
		return (ENOMEM);

	ep = (ng_hci_lp_le_con_update_req_ep *) (msg->data);
	ep->con_handle = con->con_handle;
	ep->interval_min = interval_min;
	ep->interval_max = interval_max;
	ep->latency = latency;
	ep->timeout = timeout;

	NG_SEND_MSG_HOOK(error, l2cap->node, msg, l2cap->hci, 0);

	return (error);
}

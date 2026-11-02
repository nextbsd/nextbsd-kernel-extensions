/*
 * NextBSD: SCO and eSCO parameters on Bluetooth SCO sockets
 * (nextbsd/nextbsd-kernel#237). Not upstream.
 *
 * The socket options (SO_SCO_SYNC_PARAMS, SO_SCO_SYNC_INFO) and their
 * messages to ng_hci (NGM_HCI_LP_SYNC_CON_REQ and _RSP, ng_hci_sync_lp.h
 * from NgHCI's src/) for ng_btsocket_sco.c, which
 * patches/0001-ng_btsocket_sco-synchronous-connections.patch makes call
 * these. The interface is described in ng_btsocket_sco_sync.h.
 *
 * Locking. The state lives behind each PCB (struct ng_btsocket_sco_pcb_ext)
 * and is guarded by the PCB mutex. ng_btsocket_sco_sync_ctloutput() takes it
 * itself, and copies to and from user space without it; the other functions
 * are called with it held.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bitstring.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/protosw.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/callout.h>

#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>
#include <netgraph/bluetooth/include/ng_bluetooth.h>
#include <netgraph/bluetooth/include/ng_hci.h>
#include <netgraph/bluetooth/include/ng_l2cap.h>
#include <netgraph/bluetooth/include/ng_btsocket.h>
#include <netgraph/bluetooth/include/ng_btsocket_sco.h>

#include "ng_hci_sync_lp.h"
#include "ng_btsocket_sco_sync.h"

_Static_assert(sizeof(struct sco_sync_params) == sizeof(ng_hci_sync_params_t),
    "sco_sync_params and ng_hci_sync_params_t differ");

static void
ng_btsocket_sco_sync_params_to_hci(const struct sco_sync_params *p,
    ng_hci_sync_params_t *h)
{
	h->tx_bandwidth = p->tx_bandwidth;
	h->rx_bandwidth = p->rx_bandwidth;
	h->max_latency = p->max_latency;
	h->voice_setting = p->voice_setting;
	h->pkt_type = p->pkt_type;
	h->retrans_effort = p->retrans_effort;
	h->reserved = 0;
}

/*
 * getsockopt/setsockopt at SOL_SCO for the NextBSD options. ENOPROTOOPT for
 * any other option, which ng_btsocket_sco_ctloutput() then handles. Called
 * without the PCB mutex.
 */
int
ng_btsocket_sco_sync_ctloutput(struct ng_btsocket_sco_pcb *pcb,
    struct sockopt *sopt)
{
	struct ng_btsocket_sco_sync	*sync = NG_BTSOCKET_SCO_SYNC(pcb);
	struct sco_sync_params		 params;
	struct sco_sync_info		 info;
	int				 error = 0;

	bzero(&params, sizeof(params));
	bzero(&info, sizeof(info));

	switch (sopt->sopt_name) {
	case SO_SCO_SYNC_PARAMS:
		if (sopt->sopt_dir == SOPT_SET) {
			error = sooptcopyin(sopt, &params, sizeof(params),
					sizeof(params));
			if (error != 0)
				break;
			if (params.voice_setting > 0x03ff) {
				error = EINVAL;
				break;
			}
			params.reserved = 0;

			mtx_lock(&pcb->pcb_mtx);
			if (pcb->state == NG_BTSOCKET_SCO_CONNECTING ||
			    pcb->state == NG_BTSOCKET_SCO_OPEN)
				error = EISCONN;
			else {
				sync->params = params;
				sync->params_set = 1;
			}
			mtx_unlock(&pcb->pcb_mtx);
		} else if (sopt->sopt_dir == SOPT_GET) {
			mtx_lock(&pcb->pcb_mtx);
			if (sync->params_set)
				params = sync->params;
			else
				error = ENOENT;
			mtx_unlock(&pcb->pcb_mtx);

			if (error == 0)
				error = sooptcopyout(sopt, &params,
						sizeof(params));
		} else
			error = EINVAL;
		break;

	case SO_SCO_SYNC_INFO:
		if (sopt->sopt_dir != SOPT_GET) {
			error = EINVAL;
			break;
		}

		mtx_lock(&pcb->pcb_mtx);
		if (pcb->state != NG_BTSOCKET_SCO_OPEN)
			error = ENOTCONN;
		else if (!sync->info_valid)
			error = ENOENT;
		else
			info = sync->info;
		mtx_unlock(&pcb->pcb_mtx);

		if (error == 0)
			error = sooptcopyout(sopt, &info, sizeof(info));
		break;

	default:
		error = ENOPROTOOPT;
		break;
	}

	return (error);
}

/*
 * The NGM_HCI_LP_SYNC_CON_REQ to send for a connect(), or NULL when the
 * socket set no parameters and the legacy NGM_HCI_LP_CON_REQ goes instead.
 * NULL too when no message could be allocated; the legacy request is better
 * than none.
 */
struct ng_mesg *
ng_btsocket_sco_sync_con_req(struct ng_btsocket_sco_pcb *pcb)
{
	struct ng_btsocket_sco_sync	*sync = NG_BTSOCKET_SCO_SYNC(pcb);
	ng_hci_lp_sync_con_req_ep	*ep = NULL;
	struct ng_mesg			*msg = NULL;

	mtx_assert(&pcb->pcb_mtx, MA_OWNED);

	sync->info_valid = 0;
	if (!sync->params_set)
		return (NULL);

	NG_MKMESSAGE(msg, NGM_HCI_COOKIE, NGM_HCI_LP_SYNC_CON_REQ,
		sizeof(*ep), M_NOWAIT);
	if (msg == NULL)
		return (NULL);

	ep = (ng_hci_lp_sync_con_req_ep *)(msg->data);
	bcopy(&pcb->dst, &ep->bdaddr, sizeof(ep->bdaddr));
	ep->reserved = 0;
	ng_btsocket_sco_sync_params_to_hci(&sync->params, &ep->params);

	return (msg);
}

/*
 * The NGM_HCI_LP_SYNC_CON_RSP answering a connection request on the
 * listening socket lpcb (NULL when nobody listens), or NULL for the legacy
 * NGM_HCI_LP_CON_RSP.
 */
struct ng_mesg *
ng_btsocket_sco_sync_con_rsp(struct ng_btsocket_sco_pcb *lpcb, bdaddr_p dst,
    int status)
{
	struct ng_btsocket_sco_sync	*sync;
	ng_hci_lp_sync_con_rsp_ep	*ep = NULL;
	struct ng_mesg			*msg = NULL;

	if (lpcb == NULL)
		return (NULL);

	mtx_assert(&lpcb->pcb_mtx, MA_OWNED);

	sync = NG_BTSOCKET_SCO_SYNC(lpcb);
	if (!sync->params_set)
		return (NULL);

	NG_MKMESSAGE(msg, NGM_HCI_COOKIE, NGM_HCI_LP_SYNC_CON_RSP,
		sizeof(*ep), M_NOWAIT);
	if (msg == NULL)
		return (NULL);

	ep = (ng_hci_lp_sync_con_rsp_ep *)(msg->data);
	ep->status = status;
	ep->reserved = 0;
	bcopy(dst, &ep->bdaddr, sizeof(ep->bdaddr));
	ng_btsocket_sco_sync_params_to_hci(&sync->params, &ep->params);

	return (msg);
}

/* A socket accepted on lpcb takes its parameters. Both PCBs locked. */
void
ng_btsocket_sco_sync_accepted(struct ng_btsocket_sco_pcb *pcb,
    struct ng_btsocket_sco_pcb *lpcb)
{
	struct ng_btsocket_sco_sync	*sync = NG_BTSOCKET_SCO_SYNC(pcb);
	struct ng_btsocket_sco_sync	*lsync = NG_BTSOCKET_SCO_SYNC(lpcb);

	sync->params = lsync->params;
	sync->params_set = lsync->params_set;
	sync->info_valid = 0;
}

/*
 * NGM_HCI_LP_SYNC_CON_INFO for this socket's link. A change carries no link
 * type or air mode; keep those from the completion.
 */
void
ng_btsocket_sco_sync_set_info(struct ng_btsocket_sco_pcb *pcb, const void *arg)
{
	struct ng_btsocket_sco_sync		*sync = NG_BTSOCKET_SCO_SYNC(pcb);
	const ng_hci_lp_sync_con_info_ep	*ep = arg;

	mtx_assert(&pcb->pcb_mtx, MA_OWNED);

	if (ep->status != 0)
		return;

	if (!ep->changed) {
		sync->info.link_type = ep->link_type;
		sync->info.air_mode = ep->air_mode;
	} else if (!sync->info_valid)
		return;

	sync->info.con_handle = ep->con_handle;
	sync->info.tx_interval = ep->tx_interval;
	sync->info.retrans_window = ep->retrans_window;
	sync->info.rx_pkt_len = ep->rx_pkt_len;
	sync->info.tx_pkt_len = ep->tx_pkt_len;
	sync->info_valid = 1;
}

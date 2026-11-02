/*
 * NextBSD: the LE connection-parameter message ng_l2cap sends ng_hci, and the
 * parameter check both use (nextbsd/nextbsd-kernel#234). Shared by NgHCI.kext
 * and NgL2cap.kext; NgL2cap's Makefile adds ../NgHCI/src to its include path.
 * Not upstream.
 *
 * NGM_HCI_LP_LE_CON_UPDATE_REQ: ng_l2cap has accepted an L2CAP Connection
 * Parameter Update Request on the LE signalling channel (CID 5) and asks ng_hci
 * to apply it with HCI LE_Connection_Update. It is sent on ng_l2cap's "hci"
 * hook, under NGM_HCI_COOKIE, and has no reply: the result arrives as the LE
 * Connection Update Complete event, which ng_hci logs and the raw hook sees.
 * The number is far above the upstream LP (1-10) and node (100-119) messages.
 */
#ifndef _NG_HCI_LE_LP_H_
#define _NG_HCI_LE_LP_H_

#define NGM_HCI_LP_LE_CON_UPDATE_REQ	0x4e420001 /* Upper -> HCI */
typedef struct {
	u_int16_t	con_handle;	/* LE connection handle */
	u_int16_t	interval_min;	/* units of 1.25 ms */
	u_int16_t	interval_max;	/* units of 1.25 ms */
	u_int16_t	latency;	/* connection events */
	u_int16_t	timeout;	/* supervision timeout, units of 10 ms */
} ng_hci_lp_le_con_update_req_ep;

/*
 * LE connection parameter limits, Core 5.x Vol 4 Part E 7.8.18 and Vol 3
 * Part A 4.20: interval 7.5 ms to 4 s, latency at most 499 events, supervision
 * timeout 100 ms to 32 s, and the timeout longer than
 * (1 + latency) * interval_max * 2. In spec units that last rule is
 * timeout * 10 > (1 + latency) * interval_max * 1.25 * 2, that is
 * timeout * 4 > (1 + latency) * interval_max.
 */
#define NG_HCI_LE_CON_INTERVAL_MIN	0x0006
#define NG_HCI_LE_CON_INTERVAL_MAX	0x0c80
#define NG_HCI_LE_CON_LATENCY_MAX	0x01f3
#define NG_HCI_LE_SUPERVISION_TIMO_MIN	0x000a
#define NG_HCI_LE_SUPERVISION_TIMO_MAX	0x0c80

static __inline int
ng_hci_le_con_params_valid(u_int16_t interval_min, u_int16_t interval_max,
    u_int16_t latency, u_int16_t timeout)
{
	if (interval_min < NG_HCI_LE_CON_INTERVAL_MIN ||
	    interval_max > NG_HCI_LE_CON_INTERVAL_MAX ||
	    interval_min > interval_max)
		return (0);
	if (latency > NG_HCI_LE_CON_LATENCY_MAX)
		return (0);
	if (timeout < NG_HCI_LE_SUPERVISION_TIMO_MIN ||
	    timeout > NG_HCI_LE_SUPERVISION_TIMO_MAX)
		return (0);
	if ((u_int32_t)timeout * 4 <=
	    (1 + (u_int32_t)latency) * (u_int32_t)interval_max)
		return (0);

	return (1);
}

#endif /* _NG_HCI_LE_LP_H_ */

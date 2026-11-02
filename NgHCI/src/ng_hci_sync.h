/*
 * NextBSD: ng_hci synchronous (SCO/eSCO) connections (nextbsd/nextbsd-kernel#237):
 * Setup_Synchronous_Connection, Accept_ and Reject_Synchronous_Connection_Request,
 * Synchronous_Connection_Complete and _Changed, and SCO data without SCO flow
 * control. Called from ng_hci_cmds.c, ng_hci_evnt.c, ng_hci_main.c and
 * ng_hci_ulpi.c by the patches in patches/. Include ng_hci_sync_lp.h first.
 * Not upstream.
 */
#ifndef _NG_HCI_SYNC_H_
#define _NG_HCI_SYNC_H_

/* Synchronous commands and events not in releng/15.1's ng_hci.h */
#define NG_HCI_OCF_SETUP_SYNC_CON		0x0028
#define NG_HCI_OCF_ACCEPT_SYNC_CON_REQ		0x0029
#define NG_HCI_OCF_REJECT_SYNC_CON_REQ		0x002a
#define NG_HCI_EVENT_SYNC_CON_COMPL		0x2c
#define NG_HCI_EVENT_SYNC_CON_CHANGED		0x2d

/*
 * SCO packets ng_hci_send_data() hands the driver per call. Controllers send
 * no Number_Of_Completed_Packets for SCO unless the host enables SCO flow
 * control, which nothing does, so the SCO pool is not charged (see
 * ng_hci_sync.c); this only bounds one pass.
 */
#define NG_HCI_SYNC_SEND_LIMIT			64

int	ng_hci_sync_lp_con_req(ng_hci_unit_p, item_p, hook_p);
int	ng_hci_sync_lp_con_rsp(ng_hci_unit_p, item_p, hook_p);
int	ng_hci_sync_con_answer(ng_hci_unit_p, ng_hci_unit_con_p, int,
	    const ng_hci_sync_params_t *, hook_p);
int	ng_hci_sync_con_compl(ng_hci_unit_p, struct mbuf *);
int	ng_hci_sync_con_changed(ng_hci_unit_p, struct mbuf *);
int	ng_hci_sync_command_status(ng_hci_unit_p, u_int16_t, u_int8_t,
	    struct mbuf *);
void	ng_hci_sync_sco_sent(ng_hci_unit_p);

#endif /* _NG_HCI_SYNC_H_ */

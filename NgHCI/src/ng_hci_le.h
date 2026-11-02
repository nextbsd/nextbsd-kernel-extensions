/*
 * NextBSD: ng_hci LE hardening (nextbsd/nextbsd-kernel#234): a separate LE
 * ACL buffer pool from LE_Read_Buffer_Size, LE_Connection_Update on behalf of
 * ng_l2cap, and the LE meta events releng/15.1 leaves as stubs. Called from
 * ng_hci_cmds.c, ng_hci_evnt.c, ng_hci_main.c and ng_hci_misc.c by
 * the patches in patches/. Not upstream.
 */
#ifndef _NG_HCI_LE_H_
#define _NG_HCI_LE_H_

/*
 * Buffer class of a connection: NG_HCI_LINK_SCO, NG_HCI_LINK_ACL, or
 * NG_HCI_BUFF_CLASS_LE for an LE link when the controller reported its own LE
 * pool. Without one, LE links share the ACL pool, as the Core spec says.
 */
#define NG_HCI_BUFF_CLASS_LE	NG_HCI_LINK_LE_PUBLIC

/* LE commands not in releng/15.1's ng_hci.h */
#define NG_HCI_OCF_LE_REMOTE_CONN_PARAM_REQ_REPLY	0x0020
#define NG_HCI_OCF_LE_REMOTE_CONN_PARAM_REQ_NEG_REPLY	0x0021

int	ng_hci_le_buffer_class(ng_hci_unit_p, int);
int	ng_hci_le_buffer_avail(ng_hci_unit_p);
void	ng_hci_le_buffer_use(ng_hci_unit_p, int);
void	ng_hci_le_buffer_free(ng_hci_unit_p, int);
void	ng_hci_le_buffer_set(ng_hci_unit_p, u_int16_t, u_int16_t);
void	ng_hci_le_buffer_reset(ng_hci_unit_p);
void	ng_hci_le_buffer_destroy(ng_hci_unit_p);

int	ng_hci_le_lp_con_update_req(ng_hci_unit_p, item_p, hook_p);
int	ng_hci_le_con_update_compl(ng_hci_unit_p, struct mbuf *);
int	ng_hci_le_read_remote_features_compl(ng_hci_unit_p, struct mbuf *);
int	ng_hci_le_remote_conn_param_req(ng_hci_unit_p, struct mbuf *);

#endif /* _NG_HCI_LE_H_ */

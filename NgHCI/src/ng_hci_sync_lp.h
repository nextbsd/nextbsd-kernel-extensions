/*
 * NextBSD: the synchronous-connection (SCO/eSCO) messages between ng_hci and
 * ng_btsocket_sco (nextbsd/nextbsd-kernel#237). Shared by NgHCI.kext and
 * NgBtsocket.kext; NgBtsocket's Makefile adds ../NgHCI/src to its include
 * path. Not upstream.
 *
 * releng/15.1's LP interface can only ask for a legacy SCO link
 * (NGM_HCI_LP_CON_REQ with NG_HCI_LINK_SCO, which ng_hci turns into
 * Add_SCO_Connection: HV1-HV3, CVSD, the controller's voice setting), so
 * there is no eSCO and no transparent air mode, which HFP's wide-band speech
 * (mSBC) needs. These messages carry the HCI synchronous-connection
 * parameters (Core 5.x Vol 4 Part E 7.1.26 and 7.1.27) instead:
 *
 * NGM_HCI_LP_SYNC_CON_REQ: open a SCO or eSCO link to bdaddr, over the open
 * ACL link to it, with Setup_Synchronous_Connection. The answer is the usual
 * NGM_HCI_LP_CON_CFM (link_type NG_HCI_LINK_SCO), preceded on success by
 * NGM_HCI_LP_SYNC_CON_INFO.
 *
 * NGM_HCI_LP_SYNC_CON_RSP: answer an NGM_HCI_LP_CON_IND for a SCO or eSCO
 * request with Accept_Synchronous_Connection_Request (status 0) and these
 * parameters, or Reject_Synchronous_Connection_Request (status is the reason,
 * 0x0d-0x0f). A plain NGM_HCI_LP_CON_RSP for such a request is answered with
 * the synchronous commands too, with the defaults below.
 *
 * NGM_HCI_LP_SYNC_CON_INFO: sent on the sco hook when a synchronous link
 * completes (Synchronous_Connection_Complete, before NGM_HCI_LP_CON_CFM) or
 * its parameters change (Synchronous_Connection_Changed; changed is 1 and
 * link_type and air_mode are not known, 0xff).
 *
 * All go under NGM_HCI_COOKIE. The numbers are far above the upstream LP
 * (1-10) and node (100-119) messages and follow NGM_HCI_LP_LE_CON_UPDATE_REQ
 * (ng_hci_le_lp.h). Both directions use host byte order.
 */
#ifndef _NG_HCI_SYNC_LP_H_
#define _NG_HCI_SYNC_LP_H_

/* HCI link types in Connection_Request and Synchronous_Connection_Complete */
#define NG_HCI_SYNC_LINK_SCO		0x00
#define NG_HCI_SYNC_LINK_ESCO		0x02	/* NG_HCI_LINK_LE_PUBLIC in ng_hci */

/* Air modes, Synchronous_Connection_Complete */
#define NG_HCI_SYNC_AIR_ULAW		0x00
#define NG_HCI_SYNC_AIR_ALAW		0x01
#define NG_HCI_SYNC_AIR_CVSD		0x02
#define NG_HCI_SYNC_AIR_TRANSPARENT	0x03

/* Voice_Setting: the input coding and sample size, and the air coding */
#define NG_HCI_SYNC_VOICE_16BIT		0x0020	/* input sample size 16 */
#define NG_HCI_SYNC_VOICE_AIR_MASK	0x0003	/* air coding format */
#define NG_HCI_SYNC_VOICE_CVSD		0x0060	/* linear, 16 bit, CVSD */
#define NG_HCI_SYNC_VOICE_TRANSPARENT	0x0063	/* linear, 16 bit, transparent */

/* Packet_Type for the synchronous commands; the EDR bits mean "do not use" */
#define NG_HCI_SYNC_PKT_HV1		0x0001
#define NG_HCI_SYNC_PKT_HV2		0x0002
#define NG_HCI_SYNC_PKT_HV3		0x0004
#define NG_HCI_SYNC_PKT_EV3		0x0008
#define NG_HCI_SYNC_PKT_EV4		0x0010
#define NG_HCI_SYNC_PKT_EV5		0x0020
#define NG_HCI_SYNC_PKT_NO_2EV3		0x0040
#define NG_HCI_SYNC_PKT_NO_3EV3		0x0080
#define NG_HCI_SYNC_PKT_NO_2EV5		0x0100
#define NG_HCI_SYNC_PKT_NO_3EV5		0x0200

/*
 * Defaults, for a plain NGM_HCI_LP_CON_RSP: 64 kbit/s each way, any latency
 * and retransmission effort, CVSD from 16-bit linear PCM, and every packet
 * type the controller supports.
 */
#define NG_HCI_SYNC_DEFAULT_BANDWIDTH	8000
#define NG_HCI_SYNC_DEFAULT_LATENCY	0xffff
#define NG_HCI_SYNC_DEFAULT_VOICE	NG_HCI_SYNC_VOICE_CVSD
#define NG_HCI_SYNC_DEFAULT_RETRANS	0xff
#define NG_HCI_SYNC_DEFAULT_PKT_TYPE	0x003f

/* Synchronous connection parameters, as the HCI commands take them */
typedef struct {
	u_int32_t	tx_bandwidth;	/* bytes/s */
	u_int32_t	rx_bandwidth;	/* bytes/s */
	u_int16_t	max_latency;	/* ms, 0x0004-0xfffe, 0xffff don't care */
	u_int16_t	voice_setting;	/* Voice_Setting */
	u_int16_t	pkt_type;	/* NG_HCI_SYNC_PKT_* */
	u_int8_t	retrans_effort;	/* 0 none, 1 power, 2 quality, 0xff */
	u_int8_t	reserved;
} ng_hci_sync_params_t;

#define NGM_HCI_LP_SYNC_CON_REQ		0x4e420002 /* Upper -> HCI */
typedef struct {
	bdaddr_t		bdaddr;	/* remote unit address */
	u_int16_t		reserved;
	ng_hci_sync_params_t	params;
} ng_hci_lp_sync_con_req_ep;

#define NGM_HCI_LP_SYNC_CON_RSP		0x4e420003 /* Upper -> HCI */
typedef struct {
	u_int8_t		status;	/* 0x00 accept, else reject reason */
	u_int8_t		reserved;
	bdaddr_t		bdaddr;	/* remote unit address */
	ng_hci_sync_params_t	params;	/* used when accepting */
} ng_hci_lp_sync_con_rsp_ep;

#define NGM_HCI_LP_SYNC_CON_INFO	0x4e420004 /* HCI -> Upper */
typedef struct {
	u_int8_t	status;		/* 0x00 success */
	u_int8_t	changed;	/* 1: Synchronous_Connection_Changed */
	u_int16_t	con_handle;	/* connection handle */
	bdaddr_t	bdaddr;		/* remote unit address */
	u_int8_t	link_type;	/* NG_HCI_SYNC_LINK_*, 0xff not known */
	u_int8_t	air_mode;	/* NG_HCI_SYNC_AIR_*, 0xff not known */
	u_int8_t	tx_interval;	/* slots */
	u_int8_t	retrans_window;	/* slots */
	u_int16_t	rx_pkt_len;	/* bytes, air packet */
	u_int16_t	tx_pkt_len;	/* bytes, air packet */
} ng_hci_lp_sync_con_info_ep;

#endif /* _NG_HCI_SYNC_LP_H_ */

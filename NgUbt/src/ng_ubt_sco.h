/*
 * NextBSD: the isochronous (SCO) alternate setting of a USB Bluetooth
 * controller (nextbsd/nextbsd-kernel#237). The netgraph messages are the
 * interface userland may use; bluetoothd normally needs neither. Not
 * upstream.
 *
 * Automatic. ng_ubt follows the controller's synchronous links from the HCI
 * events it carries up (Connection_Complete for SCO,
 * Synchronous_Connection_Complete, Disconnection_Complete) and the voice
 * setting from the commands it carries down (Write_Voice_Setting,
 * Setup_Synchronous_Connection, Accept_Synchronous_Connection_Request), and
 * sets interface 1's alternate setting the way the Bluetooth USB transport
 * (Core Vol 4 Part B 2.1.1) and other stacks do:
 *
 *	no link                         the setting chosen at attach (the
 *	                                largest wMaxPacketSize, as before)
 *	CVSD/u-law/A-law, 8-bit input   alt 1, 2 or 3 for 1, 2 or 3 links
 *	CVSD/u-law/A-law, 16-bit input  alt 2, 4 or 5 for 1, 2 or 3 links
 *	transparent (mSBC)              alt 6 if the interface has it, else 1
 *
 * A setting the interface lacks falls back to the attach setting. The change
 * runs in the USB explore process, as uaudio's does, since it reconfigures the
 * interface; the isochronous transfers are stopped, set up again for the new
 * packet size and restarted. The sysctl net.bluetooth.usb_isoc_auto_alt (1)
 * turns the automatic choice off.
 *
 * Manual. NGM_UBT_NODE_SET_ISOC_ALT (int32: -1 automatic, or an alternate
 * setting the interface has) on the ubtN node forces a setting, for a
 * controller the table above does not suit (some want alt 3 for mSBC).
 * NGM_UBT_NODE_GET_ISOC_ALT reports the state. ngctl names them
 * "set_isoc_alt" and "get_isoc_alt".
 */
#ifndef _NG_UBT_SCO_H_
#define _NG_UBT_SCO_H_

/* Under NGM_UBT_COOKIE, far above releng/15.1's messages (1-7) */
#define NGM_UBT_NODE_SET_ISOC_ALT	0x4e420001	/* int32_t */
#define NGM_UBT_NODE_GET_ISOC_ALT	0x4e420002	/* ng_ubt_node_isoc_alt_ep */

#define NG_UBT_ISOC_ALT_AUTO		(-1)

typedef struct {
	int32_t		mode;		/* NG_UBT_ISOC_ALT_AUTO or forced alt */
	int32_t		alt;		/* alternate setting now */
	int32_t		attach_alt;	/* setting with no voice link */
	int32_t		sco_links;	/* synchronous links seen */
	int32_t		air_mode;	/* 0 u-law 1 A-law 2 CVSD 3 transparent,
					   -1 no link */
	int32_t		voice_setting;	/* last Voice_Setting sent */
	u_int32_t	alt_mask;	/* settings interface 1 has, bit n = alt n */
} ng_ubt_node_isoc_alt_ep;

#ifdef _KERNEL
struct ubt_softc;
struct usb_device;
struct mbuf;

void	ubt_sco_attach(struct ubt_softc *, struct usb_device *,
	    const uint8_t *, uint8_t);
void	ubt_sco_detach(struct ubt_softc *);
void	ubt_sco_event(struct ubt_softc *, const uint8_t *, int);
void	ubt_sco_command(struct ubt_softc *, struct mbuf *);
int	ubt_sco_set_mode(struct ubt_softc *, int32_t);
int	ubt_sco_get(struct ubt_softc *, ng_ubt_node_isoc_alt_ep *);

/* In ng_ubt.c (patches/0002), which owns the transfer configuration */
int	ubt_isoc_set_alt(struct ubt_softc *, struct usb_device *,
	    const uint8_t *, uint8_t);
#endif /* _KERNEL */

#endif /* _NG_UBT_SCO_H_ */

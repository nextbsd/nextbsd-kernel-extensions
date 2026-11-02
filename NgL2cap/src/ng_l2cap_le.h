/*
 * NextBSD: ng_l2cap LE hardening (nextbsd/nextbsd-kernel#234). Called from
 * ng_l2cap_evnt.c by the patches in patches/. Not upstream.
 */
#ifndef _NG_L2CAP_LE_H_
#define _NG_L2CAP_LE_H_

/*
 * The smallest LE ACL data packet a controller may report in
 * LE_Read_Buffer_Size (Core 5.x Vol 4 Part E 7.8.2). ng_l2cap splits LE
 * frames at this size, so no fragment is ever too long for the LE pool.
 */
#define NG_L2CAP_LE_ACL_MIN_SIZE	27

int	ng_l2cap_lp_le_con_update(ng_l2cap_con_p, u_int16_t, u_int16_t,
	    u_int16_t, u_int16_t);

#endif /* _NG_L2CAP_LE_H_ */

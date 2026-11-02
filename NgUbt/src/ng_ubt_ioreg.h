/*
 * NextBSD: publish each USB Bluetooth controller in the IORegistry as an
 * IOBluetoothHostController entry (nextbsd-kernel-extensions#27). Called from
 * ubt_attach() and ubt_detach() by
 * patches/0001-ng_ubt-publish-IOBluetoothHostController.patch. Not upstream.
 */
#ifndef _NG_UBT_IOREG_H_
#define _NG_UBT_IOREG_H_

void	ubt_ioreg_publish(device_t);
void	ubt_ioreg_retire(device_t);

#endif /* _NG_UBT_IOREG_H_ */

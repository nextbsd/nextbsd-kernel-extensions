/*
 * NextBSD: publish each 802.11 radio in the IORegistry (nextbsd-userland#502).
 * Called from ieee80211_ifattach() and ieee80211_ifdetach() by
 * patches/0001-net80211-publish-IO80211Controller.patch. Not upstream.
 */
#ifndef _NET80211_IEEE80211_IOREG_H_
#define _NET80211_IEEE80211_IOREG_H_

struct ieee80211com;

void	ieee80211_ioreg_publish(struct ieee80211com *);
void	ieee80211_ioreg_retire(struct ieee80211com *);

#endif /* _NET80211_IEEE80211_IOREG_H_ */

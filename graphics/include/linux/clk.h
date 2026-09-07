/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * linux/clk.h -- over FreeBSD's clk(9) framework
 * (nextbsd-kernel-extensions#51).
 *
 * This header was deliberately empty for firmware KMS, and its own comment
 * recorded why and predicted this file:
 *
 *	the firmware owns the pixel clock and never exposes it, which is the
 *	whole reason this driver is tractable where full vc4 is not. [...] If a
 *	future consumer needs real clk support, this file is the place, and its
 *	emptiness is a measurement rather than an oversight.
 *
 * That consumer is the full vc4 KMS display pipeline, which programs the pixel
 * clock itself instead of asking the firmware. A compile probe of it measured
 * nine clk_* calls across five entry points, which is what this implements --
 * no more, so that the next gap is a compile error rather than a silent stub.
 *
 * FreeBSD has the whole framework already in dev/clk/clk.h; the work here is
 * spelling, plus two genuine differences called out at their definitions:
 * ownership of the returned handle, and what "min rate" means.
 */
#ifndef _LINUXKPI_LINUX_CLK_H_
#define	_LINUXKPI_LINUX_CLK_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/limits.h>	/* INT_MAX, for the ownership claim below */

#include <dev/clk/clk.h>
#include <dev/ofw/ofw_bus.h>

#include <linux/device.h>
#include <linux/err.h>

/*
 * Linux's struct clk is opaque and callers only ever hold the pointer, so it
 * can simply BE the FreeBSD clk_t. No wrapper struct, nothing to free beyond
 * the handle itself.
 */
struct clk;

/*
 * devm_clk_get() -- look up a clock by device-tree name.
 *
 * "devm" in Linux means the handle is released when the device is. FreeBSD's
 * devres does not track clk_t, so this does NOT auto-release: a caller that
 * takes a clock and never drops it leaks the handle for the module's lifetime.
 * That is survivable for a display driver which takes its clocks at attach and
 * holds them until detach, and it is the same bargain devm_rpi_firmware_get()
 * already makes in the fkms glue. Said plainly here so nobody assumes the
 * cleanup exists.
 *
 * Returns an ERR_PTR on failure, which is what Linux callers test with
 * IS_ERR().
 */
static inline struct clk *
devm_clk_get(struct device *dev, const char *id)
{
	clk_t clk;
	phandle_t node;
	int error;

	if (dev == NULL || dev->bsddev == NULL)
		return (ERR_PTR(-ENODEV));

	node = ofw_bus_get_node(dev->bsddev);
	if (node <= 0)
		return (ERR_PTR(-ENODEV));

	if (id == NULL)
		error = clk_get_by_ofw_index(dev->bsddev, node, 0, &clk);
	else
		error = clk_get_by_ofw_name(dev->bsddev, node, id, &clk);
	if (error != 0)
		return (ERR_PTR(-error));

	return ((struct clk *)clk);
}

static inline struct clk *
devm_clk_get_optional(struct device *dev, const char *id)
{
	struct clk *clk;

	clk = devm_clk_get(dev, id);
	if (IS_ERR(clk))
		return (NULL);
	return (clk);
}

/*
 * Linux splits prepare (may sleep) from enable (may not); FreeBSD's clk_enable
 * covers both, so the combined entry points are the ones that map cleanly and
 * the split ones are deliberately absent.
 */
static inline int
clk_prepare_enable(struct clk *clk)
{

	if (clk == NULL)
		return (0);
	return (-clk_enable((clk_t)clk));
}

static inline void
clk_disable_unprepare(struct clk *clk)
{

	if (clk == NULL)
		return;
	(void)clk_disable((clk_t)clk);
}

static inline unsigned long
clk_get_rate(struct clk *clk)
{
	uint64_t freq;

	if (clk == NULL)
		return (0);
	if (clk_get_freq((clk_t)clk, &freq) != 0)
		return (0);
	return ((unsigned long)freq);
}

/*
 * Set a rate the way Linux does (#51).
 *
 * FreeBSD refuses to retune a clock whose enable_cnt exceeds the count the
 * caller claims to hold -- clk_set_freq() asserts exactly one reference -- so
 * that one consumer cannot move a rate out from under another. vc4_hdmi holds
 * TWO references on the HDMI state machine clock, one taken in
 * vc5_hdmi_init_resources() and one in runtime resume, and then asks for a
 * rate. The guard fires and the modeset fails:
 *
 *	vc40: [drm] *ERROR* Failed to set HSM clock rate: -16
 *
 * with hw.clock.rpifw-m2mc.enable_cnt reading 2, measured on a Pi 500+. The
 * rate then stays 0, the HDMI PHY never gets a pixel clock, and the display
 * stays dark while everything else reports success.
 *
 * Linux has no such rule: clk_set_rate() simply sets the rate, and a driver
 * wanting the strict behaviour asks for it explicitly with
 * clk_set_rate_exclusive(). So the Linux contract is the permissive one, and
 * these are LinuxKPI entry points.
 *
 * The normal path is left exactly as it was -- clk_set_freq() first, one
 * reference asserted. Only EBUSY is handled, by reissuing through
 * clknode_set_freq() with an ownership count that covers every outstanding
 * reference. That is safe here because the references belong to one driver:
 * vc4 is the only consumer of these display clocks. It would NOT be safe on a
 * clock genuinely shared between drivers, which is what the guard is for, so
 * this deliberately does not lift the guard globally -- it only declines to
 * apply it to a caller that already holds everything.
 */
static inline int
lkpi_clk_set_freq(struct clk *clk, unsigned long rate, int flags)
{
	struct clknode *node;
	const char *name;
	int error;

	if (clk == NULL)
		return (0);

	error = clk_set_freq((clk_t)clk, (uint64_t)rate, flags);
	if (error != EBUSY)
		return (-error);

	name = clk_get_name((clk_t)clk);
	if (name == NULL)
		return (-error);
	node = clknode_find_by_name(name);
	if (node == NULL)
		return (-error);

	/*
	 * INT_MAX rather than a read of enable_cnt: the framework exposes no
	 * getter for it, and the test is "enable_cnt > enablecnt", so this
	 * says "however many references exist, they are mine" -- which is the
	 * claim being made, and it is true here.
	 */
	error = clknode_set_freq(node, (uint64_t)rate, flags, INT_MAX);
	return (-error);
}

/*
 * clk_set_min_rate() -- "at least this fast".
 *
 * Linux's version records a floor on a shared clock and lets the framework
 * arbitrate between consumers. FreeBSD has no per-consumer constraint, so this
 * sets the frequency directly with CLK_SET_ROUND_UP, which yields the nearest
 * achievable rate not below the request -- the same guarantee for a single
 * consumer, and vc4 is the only consumer of the pixel clock.
 *
 * With more than one consumer this would be wrong: the last caller would win
 * rather than the highest floor. Worth revisiting if a second driver ever
 * shares a clock with the display pipeline.
 */
static inline int
clk_set_min_rate(struct clk *clk, unsigned long rate)
{

	return (lkpi_clk_set_freq(clk, rate, CLK_SET_ROUND_UP));
}

static inline int
clk_set_rate(struct clk *clk, unsigned long rate)
{

	return (lkpi_clk_set_freq(clk, rate, CLK_SET_ROUND_ANY));
}

static inline void
clk_put(struct clk *clk)
{

	if (clk == NULL)
		return;
	(void)clk_release((clk_t)clk);
}

#endif /* _LINUXKPI_LINUX_CLK_H_ */

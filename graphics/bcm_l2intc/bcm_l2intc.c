/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Broadcom level-2 interrupt controller (nextbsd-kernel-extensions#51).
 *
 * WHY THIS EXISTS
 *
 * Every "irq 65535" in the vc4 KMS probe output is this driver being absent:
 *
 *	vc4_hvs0:  ... irq 5,6,7    on ofwbus0   -> vc4_hvs:  irq 65535
 *	vc4_hdmi0: ... irq 32,...36 on simplebus0 -> vc4_hdmi: irq 65535
 *	vc4_crtc0: ... irq 23       on simplebus0 -> vc4_crtc: irq 23   <- works
 *
 * measured on a Pi 500+. The pixelvalves work because their interrupts go
 * straight to the GIC. The HVS and both HDMI controllers do not: their
 * interrupt-parent is interrupt-controller@7d510600, whose compatible is
 * "brcm,bcm2711-l2-intc\0brcm,l2-intc\0", and FreeBSD has no driver for it --
 * the kernel knows brcm,bcm2835-armctrl-ic, brcm,bcm2836-armctrl-ic,
 * brcm,bcm2836-l1-intc and brcm,brahma-b15-gic, and nothing else.
 *
 * INTRNG resolves a device-tree interrupt lazily: ofw_bus_intr_to_rl() records
 * an unresolved map cookie at bus enumeration, and intr_activate_irq() looks up
 * the PIC only when the resource is allocated RF_ACTIVE. With no PIC registered
 * for that xref the lookup fails, bus_alloc_resource_any() returns NULL, and
 * the caller sees -ENXIO. That is why the cookies (5,6,7 / 32..36) are printed
 * by newbus and yet nothing can be allocated -- and it is also why this can be
 * a loadable module at all: registering the PIC after boot makes every later
 * activation resolve.
 *
 * What it costs to not have this: HDMI hotplug, and vblank. The HVS end-of-
 * frame interrupts ARE vblank on gen6 (vc6_hvs_eof_irq_handler), so without
 * this controller a KMS driver has no frame timing.
 *
 * TWO REGISTER LAYOUTS
 *
 * Not all of these controllers are the same, and picking the wrong layout
 * silently writes the mask to the wrong register. Upstream Linux
 * (drivers/irqchip/irq-brcmstb-l2.c) splits them: "brcm,l2-intc",
 * "brcm,hif-spi-l2-intc" and "brcm,upg-aux-aon-l2-intc" are the EDGE variant
 * (status 0x00, clear 0x08, mask_status 0x0c, mask_set 0x10, mask_clear 0x14);
 * "brcm,bcm7271-l2-intc" is the LEVEL variant, which has no clear register at
 * all and puts the masks 8 bytes lower (mask_status 0x04, set 0x08, clear
 * 0x0c).
 *
 * Both live on this SoC. The display controller at 7d510600 matches
 * "brcm,l2-intc" (edge); intc@7d508380 and intc@7d508400 are
 * "brcm,bcm7271-l2-intc" (level). Note that bcm2711-l2-intc is NOT in Linux's
 * match table either -- that node is matched on its "brcm,l2-intc" fallback,
 * which is exactly what this driver relies on too.
 */

#include "opt_platform.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>

#include <machine/bus.h>
#include <machine/intr.h>
#include <machine/resource.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "pic_if.h"

#define	BCM_L2_NIRQ	32

struct bcm_l2_layout {
	bus_size_t	status;
	int		clear;		/* < 0 when the register is absent */
	bus_size_t	mask_status;
	bus_size_t	mask_set;
	bus_size_t	mask_clear;
	bool		edge;
	const char	*name;
};

static const struct bcm_l2_layout bcm_l2_edge = {
	.status = 0x00, .clear = 0x08, .mask_status = 0x0c,
	.mask_set = 0x10, .mask_clear = 0x14, .edge = true, .name = "edge",
};

static const struct bcm_l2_layout bcm_l2_level = {
	.status = 0x00, .clear = -1, .mask_status = 0x04,
	.mask_set = 0x08, .mask_clear = 0x0c, .edge = false, .name = "level",
};

static struct ofw_compat_data compat_data[] = {
	{ "brcm,l2-intc",		(uintptr_t)&bcm_l2_edge },
	{ "brcm,hif-spi-l2-intc",	(uintptr_t)&bcm_l2_edge },
	{ "brcm,upg-aux-aon-l2-intc",	(uintptr_t)&bcm_l2_edge },
	{ "brcm,bcm7271-l2-intc",	(uintptr_t)&bcm_l2_level },
	{ NULL,				0 }
};

struct bcm_l2_irqsrc {
	struct intr_irqsrc		 isrc;
	u_int				 bit;
};

struct bcm_l2_softc {
	device_t			 dev;
	struct resource			*mem_res;
	struct resource			*irq_res;
	void				*intr_cookie;
	const struct bcm_l2_layout	*lay;
	struct bcm_l2_irqsrc		 isrcs[BCM_L2_NIRQ];
};

#define	RD4(sc, o)	bus_read_4((sc)->mem_res, (o))
#define	WR4(sc, o, v)	bus_write_4((sc)->mem_res, (o), (v))

static int
bcm_l2_intr(void *arg)
{
	struct bcm_l2_softc *sc = arg;
	struct trapframe *tf = curthread->td_intr_frame;
	uint32_t status;
	int i;

	status = RD4(sc, sc->lay->status) & ~RD4(sc, sc->lay->mask_status);
	if (status == 0)
		return (FILTER_STRAY);

	/*
	 * Acknowledge the whole latched set before dispatching anything.
	 *
	 * On an edge controller a line re-asserting while its handler runs
	 * must latch again, so the clear has to happen BEFORE the handler, not
	 * after it -- clearing in PIC_POST_FILTER would drop that edge. Linux
	 * gets this via handle_edge_irq(), which acks first for the same
	 * reason. The level variant has no clear register and needs none: the
	 * line simply stays asserted until the device is serviced.
	 */
	if (sc->lay->clear >= 0)
		WR4(sc, (bus_size_t)sc->lay->clear, status);

	while ((i = ffs(status)) != 0) {
		i--;
		status &= ~(1u << i);
		if (intr_isrc_dispatch(&sc->isrcs[i].isrc, tf) != 0) {
			/*
			 * Nothing is attached to this line. Mask it rather
			 * than let an unclaimed source livelock the parent
			 * GIC interrupt this controller is chained to.
			 */
			WR4(sc, sc->lay->mask_set, 1u << i);
			device_printf(sc->dev,
			    "stray irq %d masked (#51)\n", i);
		}
	}
	return (FILTER_HANDLED);
}

static void
bcm_l2_enable_intr(device_t dev, struct intr_irqsrc *isrc)
{
	struct bcm_l2_softc *sc = device_get_softc(dev);
	struct bcm_l2_irqsrc *bi = (struct bcm_l2_irqsrc *)isrc;

	WR4(sc, sc->lay->mask_clear, 1u << bi->bit);
}

static void
bcm_l2_disable_intr(device_t dev, struct intr_irqsrc *isrc)
{
	struct bcm_l2_softc *sc = device_get_softc(dev);
	struct bcm_l2_irqsrc *bi = (struct bcm_l2_irqsrc *)isrc;

	WR4(sc, sc->lay->mask_set, 1u << bi->bit);
}

static int
bcm_l2_map_intr(device_t dev, struct intr_map_data *data,
    struct intr_irqsrc **isrcp)
{
	struct bcm_l2_softc *sc = device_get_softc(dev);
	struct intr_map_data_fdt *daf;

	if (data->type != INTR_MAP_DATA_FDT)
		return (ENOTSUP);

	/*
	 * #interrupt-cells = <1> on every one of these nodes: a child names a
	 * single bit and nothing else, so a second cell would mean the DT does
	 * not describe the controller this driver thinks it does.
	 */
	daf = (struct intr_map_data_fdt *)data;
	if (daf->ncells != 1 || daf->cells[0] >= BCM_L2_NIRQ)
		return (EINVAL);

	*isrcp = &sc->isrcs[daf->cells[0]].isrc;
	return (0);
}

static int
bcm_l2_setup_intr(device_t dev, struct intr_irqsrc *isrc,
    struct resource *res, struct intr_map_data *data)
{

	return (0);
}

static int
bcm_l2_teardown_intr(device_t dev, struct intr_irqsrc *isrc,
    struct resource *res, struct intr_map_data *data)
{

	return (0);
}

static void
bcm_l2_pre_ithread(device_t dev, struct intr_irqsrc *isrc)
{

	bcm_l2_disable_intr(dev, isrc);
}

static void
bcm_l2_post_ithread(device_t dev, struct intr_irqsrc *isrc)
{

	bcm_l2_enable_intr(dev, isrc);
}

static void
bcm_l2_post_filter(device_t dev, struct intr_irqsrc *isrc)
{

	/*
	 * Deliberately empty: bcm_l2_intr() already acknowledged the edge
	 * before dispatching, and the level variant has nothing to acknowledge.
	 */
}

static int
bcm_l2_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);

	device_set_desc(dev, "Broadcom L2 interrupt controller");
	return (BUS_PROBE_DEFAULT);
}

static int
bcm_l2_attach(device_t dev)
{
	struct bcm_l2_softc *sc = device_get_softc(dev);
	phandle_t node;
	int error, i, rid;

	sc->dev = dev;
	sc->lay = (const struct bcm_l2_layout *)
	    ofw_bus_search_compatible(dev, compat_data)->ocd_data;

	rid = 0;
	sc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->mem_res == NULL) {
		device_printf(dev, "could not map registers\n");
		return (ENXIO);
	}

	rid = 0;
	sc->irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_ACTIVE);
	if (sc->irq_res == NULL) {
		device_printf(dev, "could not allocate the parent interrupt\n");
		error = ENXIO;
		goto fail_mem;
	}

	/* Start from a known state: everything masked, nothing latched. */
	WR4(sc, sc->lay->mask_set, 0xffffffffu);
	if (sc->lay->clear >= 0)
		WR4(sc, (bus_size_t)sc->lay->clear, 0xffffffffu);

	node = ofw_bus_get_node(dev);
	for (i = 0; i < BCM_L2_NIRQ; i++) {
		sc->isrcs[i].bit = i;
		error = intr_isrc_register(&sc->isrcs[i].isrc, dev, 0, "%s,%u",
		    device_get_nameunit(dev), i);
		if (error != 0) {
			device_printf(dev, "could not register irq %d: %d\n",
			    i, error);
			goto fail_isrc;
		}
	}

	if (intr_pic_register(dev, OF_xref_from_node(node)) == NULL) {
		device_printf(dev, "could not register the PIC\n");
		error = ENXIO;
		goto fail_isrc;
	}

	error = bus_setup_intr(dev, sc->irq_res, INTR_TYPE_MISC | INTR_MPSAFE,
	    bcm_l2_intr, NULL, sc, &sc->intr_cookie);
	if (error != 0) {
		device_printf(dev, "could not set up the parent interrupt: "
		    "%d\n", error);
		goto fail_pic;
	}

	if (bootverbose)
		device_printf(dev, "%s layout, %d lines (#51)\n",
		    sc->lay->name, BCM_L2_NIRQ);
	return (0);

fail_pic:
	intr_pic_deregister(dev, OF_xref_from_node(node));
fail_isrc:
	while (--i >= 0)
		intr_isrc_deregister(&sc->isrcs[i].isrc);
	bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq_res);
fail_mem:
	bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem_res);
	return (error);
}

static int
bcm_l2_detach(device_t dev)
{

	/*
	 * Refused on purpose. Consumers hold interrupts resolved through this
	 * PIC; tearing it out from under them would leave those mappings
	 * pointing at freed irqsrcs.
	 */
	return (EBUSY);
}

static device_method_t bcm_l2_methods[] = {
	DEVMETHOD(device_probe,		bcm_l2_probe),
	DEVMETHOD(device_attach,	bcm_l2_attach),
	DEVMETHOD(device_detach,	bcm_l2_detach),

	DEVMETHOD(pic_map_intr,		bcm_l2_map_intr),
	DEVMETHOD(pic_enable_intr,	bcm_l2_enable_intr),
	DEVMETHOD(pic_disable_intr,	bcm_l2_disable_intr),
	DEVMETHOD(pic_setup_intr,	bcm_l2_setup_intr),
	DEVMETHOD(pic_teardown_intr,	bcm_l2_teardown_intr),
	DEVMETHOD(pic_pre_ithread,	bcm_l2_pre_ithread),
	DEVMETHOD(pic_post_ithread,	bcm_l2_post_ithread),
	DEVMETHOD(pic_post_filter,	bcm_l2_post_filter),

	DEVMETHOD_END
};

static driver_t bcm_l2_driver = {
	"bcm_l2intc",
	bcm_l2_methods,
	sizeof(struct bcm_l2_softc),
};

/*
 * BUS_PASS_INTERRUPT so that, on a kernel where this is compiled in, it comes
 * up before the devices that need it. Loaded as a module the pass no longer
 * gates anything -- the bus is already past it -- which is why the vc4 kext
 * must be loaded AFTER this one.
 */
EARLY_DRIVER_MODULE(bcm_l2intc, simplebus, bcm_l2_driver, 0, 0,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_MIDDLE);
EARLY_DRIVER_MODULE(bcm_l2intc_ofw, ofwbus, bcm_l2_driver, 0, 0,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_MIDDLE);
MODULE_VERSION(bcm_l2intc, 1);

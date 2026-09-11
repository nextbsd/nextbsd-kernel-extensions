/*-
 * SPDX-License-Identifier: MIT
 *
 * dev.vc4_crtc.<n>.state -- the pixelvalve's interrupt registers.
 *
 * Why this exists: the desktop wedges with X blocked forever in
 * drm_atomic_helper_wait_for_fences(), and dev.v3d.0.state shows V3D idle --
 * nothing queued, nothing active, registers static, no fault. So the fence
 * being waited on is not V3D's; it belongs to the display side. Meanwhile the
 * CRTC interrupt counter in vmstat -ia stops advancing.
 *
 * Page-flip completion on vc4 runs off that interrupt:
 *
 *	vc4_crtc_irq_handler() -- PV_INTSTAT & PV_INT_VFP_START
 *	  -> vc4_crtc_handle_vblank() -> drm_crtc_handle_vblank()
 *
 * so if it stops, flips never complete, their fences never signal, and the
 * next atomic commit blocks forever. These three registers say which half is
 * at fault:
 *
 *   PV_INTEN    VFP_START clear means the hardware was told to stop raising
 *               the interrupt -- a driver/vblank-refcount bug.
 *   PV_INTSTAT  VFP_START set while the counter is frozen means the hardware
 *               DID raise it and the CPU never took or acked it -- a delivery
 *               bug, not a display one.
 *   PV_STAT     the pixelvalve's own view of where it is in the frame.
 *
 * Separate file from vc4_crtc_newbus.c on purpose: that one includes
 * vc4_newbus.h, which pulls <sys/rman.h> and FreeBSD's struct resource, while
 * vc4_drv.h pulls <linux/ioport.h> and a different struct of the same name.
 * Mixing them is the collision the linuxkpi-changes skill documents.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>

#include <linux/device.h>
#include <linux/io.h>

#include <drm/drm_crtc.h>

#include "vc4_drv.h"
#include "vc4_regs.h"

int vc4_crtc_sysctl_state(SYSCTL_HANDLER_ARGS);

int
vc4_crtc_sysctl_state(SYSCTL_HANDLER_ARGS)
{
	struct device *dev = arg1;
	struct vc4_crtc *vc4_crtc;
	struct drm_crtc *crtc;
	struct sbuf sb;
	uint32_t inten, intstat, stat, control;
	int error;

	if (dev == NULL)
		return (ENXIO);
	vc4_crtc = dev_get_drvdata(dev);
	if (vc4_crtc == NULL || vc4_crtc->regs == NULL)
		return (ENXIO);
	crtc = &vc4_crtc->base;

	control = readl(vc4_crtc->regs + PV_CONTROL);
	inten   = readl(vc4_crtc->regs + PV_INTEN);
	intstat = readl(vc4_crtc->regs + PV_INTSTAT);
	stat    = readl(vc4_crtc->regs + PV_STAT);

	sbuf_new_for_sysctl(&sb, NULL, 512, req);
	sbuf_printf(&sb, "\n");
	sbuf_printf(&sb, "PV_CONTROL 0x%08x\n", control);
	sbuf_printf(&sb, "PV_INTEN   0x%08x  VFP_START %s\n", inten,
	    (inten & PV_INT_VFP_START) ? "ENABLED" : "disabled");
	sbuf_printf(&sb, "PV_INTSTAT 0x%08x  VFP_START %s\n", intstat,
	    (intstat & PV_INT_VFP_START) ? "PENDING (raised, not taken)" : "clear");
	sbuf_printf(&sb, "PV_STAT    0x%08x\n", stat);
	sbuf_printf(&sb, "crtc enabled=%d active=%d event=%s\n",
	    crtc->enabled,
	    crtc->state != NULL ? crtc->state->active : -1,
	    (crtc->state != NULL && crtc->state->event != NULL) ?
	    "PENDING" : "none");

	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

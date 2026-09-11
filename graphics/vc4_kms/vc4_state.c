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
#include <drm/drm_plane.h>
#include <linux/dma-fence.h>

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

	/*
	 * Name the fence each plane is holding.
	 *
	 * drm_atomic_helper_wait_for_fences() blocks on plane_state->fence,
	 * which drm_gem_plane_helper_prepare_fb() takes from the framebuffer's
	 * dma_resv. When the desktop wedges, dev.v3d.0.state reports
	 * outstanding=0 on every queue -- every v3d job has completed -- so
	 * whatever is being waited on is NOT an outstanding v3d job fence. If
	 * it were, it would already be signalled and the wait would return.
	 *
	 * So print who owns it. driver/timeline come from the fence ops, which
	 * is how a v3d fence, a vc4 fence, a dma_fence_array/chain container
	 * and a syncobj stub tell themselves apart. "signalled=1" here with a
	 * thread still asleep in dma_fence_default_wait() would instead mean a
	 * lost wakeup rather than a fence that never fired.
	 */
	{
		struct drm_plane *plane;

		drm_for_each_plane(plane, crtc->dev) {
			struct dma_fence *f;

			if (plane->state == NULL)
				continue;
			f = plane->state->fence;
			if (f == NULL)
				continue;
			sbuf_printf(&sb,
			    "plane[%u] fence driver=%s timeline=%s ctx=%llu seqno=%llu signalled=%d\n",
			    plane->base.id,
			    f->ops != NULL && f->ops->get_driver_name != NULL ?
			    f->ops->get_driver_name(f) : "?",
			    f->ops != NULL && f->ops->get_timeline_name != NULL ?
			    f->ops->get_timeline_name(f) : "?",
			    (unsigned long long)f->context,
			    (unsigned long long)f->seqno,
			    dma_fence_is_signaled(f));
		}
	}

	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

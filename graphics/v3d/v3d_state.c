/*-
 * SPDX-License-Identifier: MIT
 *
 * dev.v3d.0.state -- read what the GPU and its schedulers are actually doing.
 *
 * The Pi wedges intermittently: v3d interrupt counters stop, a job's fence
 * never signals, X blocks forever in an atomic commit, and nothing is logged
 * because drm_sched's timeout handler is never reached. Telling the possible
 * causes apart needs the state at the moment it is stuck, and v3d_debugfs.c is
 * not vendored here (CONFIG_DEBUG_FS is off, same as vc4_kms), so read it out
 * directly.
 *
 * This lives in its own file on purpose. v3d_newbus.c handles newbus
 * resources and therefore sees FreeBSD's struct resource from <sys/rman.h>;
 * v3d_drv.h drags in <linux/ioport.h>, which declares a different struct of
 * the same name. Keeping the two apart is the rule in the linuxkpi-changes
 * skill, and mixing them is what broke i915 and amdgpu before.
 *
 * What the fields mean:
 *   pending      NONEMPTY means a job is queued with the scheduler, so
 *                drm_sched_start_timeout() should have armed the TDR. Empty
 *                while X waits on a fence means the job was never submitted --
 *                a very different bug from the GPU hanging.
 *   active       the job the queue believes is on the hardware.
 *   CTnCA/CTnRA  CLE addresses. Sample twice: moving means the GPU is looping
 *                in a control list, static means it is idle.
 *   INT_STS      a set bit with a frozen interrupt counter means the GPU
 *                raised a completion the CPU never took -- delivery, not
 *                execution.
 *   ERR_STAT     non-zero means the GPU faulted.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>

#include <linux/device.h>

#include "v3d_drv.h"
#include "v3d_regs.h"	/* register offsets: v3d_drv.h does not pull these in */

int v3d_sysctl_state(SYSCTL_HANDLER_ARGS);

int
v3d_sysctl_state(SYSCTL_HANDLER_ARGS)
{
	struct device *dev = arg1;
	struct drm_device *drm;
	struct v3d_dev *v3d;
	struct sbuf sb;
	int error, q;

	if (dev == NULL)
		return (ENXIO);
	drm = dev_get_drvdata(dev);
	if (drm == NULL)
		return (ENXIO);
	v3d = to_v3d_dev(drm);

	sbuf_new_for_sysctl(&sb, NULL, 1024, req);

	sbuf_printf(&sb, "\n");
	sbuf_printf(&sb, "hub  INT_STS 0x%08x\n", V3D_READ(V3D_HUB_INT_STS));
	sbuf_printf(&sb, "core INT_STS 0x%08x\n",
	    V3D_CORE_READ(0, V3D_CTL_INT_STS));
	sbuf_printf(&sb, "ERR_STAT     0x%08x\n",
	    V3D_CORE_READ(0, V3D_ERR_STAT));

	for (q = 0; q < V3D_MAX_QUEUES; q++) {
		struct v3d_queue_state *qs = &v3d->queue[q];

		sbuf_printf(&sb, "q%d emit=%llu active=%-3s pending=%s",
		    q, (unsigned long long)qs->emit_seqno,
		    qs->active_job != NULL ? "yes" : "no",
		    list_empty(&qs->sched.pending_list) ? "empty" : "NONEMPTY");
		if (q == V3D_BIN || q == V3D_RENDER)
			sbuf_printf(&sb, " CTnCA=0x%08x CTnRA=0x%08x",
			    V3D_CORE_READ(0, V3D_CLE_CTNCA(q)),
			    V3D_CORE_READ(0, V3D_CLE_CTNRA(q)));
		sbuf_printf(&sb, "\n");
	}

	error = sbuf_finish(&sb);
	sbuf_delete(&sb);
	return (error);
}

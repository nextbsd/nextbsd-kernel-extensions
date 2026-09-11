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

#include <drm/gpu_scheduler.h>
#include <drm/spsc_queue.h>

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
	int error, q, i;

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

		uint64_t done = qs->stats.jobs_completed;

		/*
		 * emit vs done is the number that matters. emit_seqno counts
		 * jobs whose fence has been created (and attached to the BO's
		 * dma_resv); jobs_completed counts those that finished. A gap
		 * with pending=empty means the jobs are sitting UNSUBMITTED in
		 * the scheduler's entity queue -- drm_sched only moves a job
		 * to pending_list once its submit worker pushes it to the
		 * hardware. That state produces exactly what we see: no
		 * interrupts, no armed timeout (it only arms for pending
		 * jobs), no messages, and a fence that never signals.
		 */
		sbuf_printf(&sb,
		    "q%d emit=%llu done=%llu outstanding=%lld active=%-3s pending=%s",
		    q, (unsigned long long)qs->emit_seqno,
		    (unsigned long long)done,
		    (long long)(qs->emit_seqno - done),
		    qs->active_job != NULL ? "yes" : "no",
		    list_empty(&qs->sched.pending_list) ? "empty" : "NONEMPTY");
		sbuf_printf(&sb, " credit=%d ready=%d paused=%d",
		    atomic_read(&qs->sched.credit_count),
		    qs->sched.ready, qs->sched.pause_submit);

		/*
		 * The state of the scheduler's own submit work item. This is
		 * THE discriminator for the captured wedge, where the render
		 * queue sits at jobs=107 dep=0 stop=0 credit=0 ready=1
		 * paused=0 -- i.e. drm_sched_select_entity() would return that
		 * entity at once, so the only remaining explanation is that
		 * drm_sched_run_job_work() is never invoked.
		 *
		 * LinuxKPI work states (linux_work.c):
		 *   0 IDLE   not queued, not running
		 *   1 TIMER  delayed work timer pending
		 *   2 TASK   queued on the taskqueue
		 *   3 EXEC   callback executing
		 *   4 CANCEL cancel requested
		 *
		 * With jobs waiting:
		 *   IDLE -> the queue_work() wakeup was dropped; drm_sched is
		 *           correct and LinuxKPI lost it. drm_sched_run_job_work()
		 *           re-queues itself only while running, so one lost
		 *           wakeup stalls the queue permanently.
		 *   TASK -> queued but the taskqueue thread never ran it.
		 *   EXEC -> the worker is stuck inside the callback.
		 */
		sbuf_printf(&sb, " runwork=%d freework=%d",
		    atomic_read(&qs->sched.work_run_job.state),
		    atomic_read(&qs->sched.work_free_job.state));

		/*
		 * Walk the run-queues.
		 *
		 * The captured wedge shows an UNSIGNALLED drm_sched fence whose
		 * seqno is ahead of anything v3d has emitted to hardware: the
		 * job was armed (fence created and attached to the BOs, so
		 * every waiter blocks) but never submitted. pending_list is
		 * therefore empty, no interrupt fires, and drm_sched arms no
		 * timeout -- which is why the hang is completely silent.
		 *
		 * drm_sched_run_job_work() returns WITHOUT re-queueing itself
		 * when drm_sched_select_entity() yields NULL, so one entity
		 * holding jobs while invisible to the run-queue stalls the
		 * scheduler forever.
		 *
		 * "rq[i] ents=N jobs=M" is the discriminator: jobs>0 with the
		 * scheduler idle means they are queued and not being picked up.
		 * ents=0 while a fence is unsignalled means the entity fell out
		 * of the run-queue entirely.
		 */
		for (i = 0; i < qs->sched.num_rqs; i++) {
			struct drm_sched_rq *rq = qs->sched.sched_rq[i];
			struct drm_sched_entity *ent;
			int nents = 0;
			unsigned int njobs = 0;
			int ndep = 0, nstop = 0;

			if (rq == NULL)
				continue;
			spin_lock(&rq->lock);
			list_for_each_entry(ent, &rq->entities, list) {
				nents++;
				njobs += spsc_queue_count(&ent->job_queue);
				if (ent->dependency != NULL)
					ndep++;
				if (ent->stopped)
					nstop++;
			}
			spin_unlock(&rq->lock);
			if (nents != 0 || njobs != 0)
				sbuf_printf(&sb,
				    " rq[%d]{ents=%d jobs=%u dep=%d stop=%d}",
				    i, nents, njobs, ndep, nstop);
		}
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

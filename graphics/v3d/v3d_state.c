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
#include <sys/taskqueue.h>
#include <sys/callout.h>
#include <sys/kernel.h>

#include <linux/device.h>
#include <linux/workqueue.h>

#include <drm/gpu_scheduler.h>
#include <linux/dma-fence.h>
#include <drm/spsc_queue.h>

#include "v3d_drv.h"
#include "v3d_regs.h"	/* register offsets: v3d_drv.h does not pull these in */

extern unsigned long v3d_wd_kicks;

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
	sbuf_printf(&sb, "watchdog kicks %lu\n", v3d_wd_kicks);

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
		 *   3 EXEC   callback executing -- note this is also the normal
		 *             RESTING state: linux_work_fn() exits its loop by
		 *             falling through to "goto done" without resetting
		 *             to IDLE, so EXEC on an idle queue is expected and
		 *             is NOT evidence of anything.
		 *   4 CANCEL cancel requested
		 *
		 * With jobs waiting:
		 *   IDLE -> the queue_work() wakeup was dropped; drm_sched is
		 *           correct and LinuxKPI lost it. drm_sched_run_job_work()
		 *           re-queues itself only while running, so one lost
		 *           wakeup stalls the queue permanently.
		 *   TASK -> claims to be queued. The second number, ta_pending,
		 *           is what makes this conclusive: it is the underlying
		 *           struct task's pending count, so
		 *             TASK with ta_pending=0  => NOT on any taskqueue.
		 *           The only path in linux_queue_work_on() that sets
		 *           TASK without calling taskqueue_enqueue() is the
		 *           linux_work_exec_unblock() shortcut, which returns
		 *           true and relies on the running linux_work_fn loop
		 *           to re-run the callback.
		 *             TASK with ta_pending>0  => really queued, and the
		 *           taskqueue thread is at fault instead.
		 */
		sbuf_printf(&sb, " runwork=%d/%d freework=%d/%d",
		    atomic_read(&qs->sched.work_run_job.state),
		    qs->sched.work_run_job.work_task.ta_pending,
		    atomic_read(&qs->sched.work_free_job.state),
		    qs->sched.work_free_job.work_task.ta_pending);

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
				struct dma_fence *d = ent->dependency;

				nents++;
				njobs += spsc_queue_count(&ent->job_queue);
				if (ent->stopped)
					nstop++;
				if (d == NULL)
					continue;
				ndep++;
				/*
				 * Name the dependency. An entity parked on an
				 * unsignalled dependency is drm_sched working
				 * as designed -- drm_sched_entity_pop_job()
				 * registers a callback and returns NULL, and
				 * the scheduler waits for drm_sched_wakeup().
				 * But if that fence never signals, or its
				 * callback is lost, the entity stalls forever
				 * with jobs piling up behind it, and nothing
				 * is wrongly queued so the taskqueue watchdog
				 * cannot see it.
				 *
				 * signalled=1 here means the fence DID fire and
				 * the wakeup callback was lost -- a different
				 * bug from a fence that never fires, and a
				 * different fix.
				 */
				sbuf_printf(&sb,
				    "\n    q%d dep fence %s/%s ctx %llu seqno %llu signalled=%d",
				    q,
				    (d->ops != NULL &&
				     d->ops->get_driver_name != NULL) ?
				    d->ops->get_driver_name(d) : "?",
				    (d->ops != NULL &&
				     d->ops->get_timeline_name != NULL) ?
				    d->ops->get_timeline_name(d) : "?",
				    (unsigned long long)d->context,
				    (unsigned long long)d->seqno,
				    dma_fence_is_signaled(d));
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

/*
 * dev.v3d.0.kick -- re-kick the scheduler submit taskqueues.
 *
 * The wedge is: drm_sched's submit work item sits on its taskqueue with
 * ta_pending=1 while the dedicated thread is idle -- i.e. the work is
 * genuinely queued and the wakeup that should have dispatched it was lost.
 * It is self-perpetuating, because taskqueue_enqueue_locked() only kicks the
 * thread on the 0->1 transition:
 *
 *	if (task->ta_pending) { task->ta_pending++; TQ_UNLOCK(queue); return; }
 *
 * so every later queue_work() bumps a counter and wakes nothing.
 *
 * taskqueue_unblock() is the one exported call that breaks that: it clears
 * TQ_FLAGS_BLOCKED *and* re-issues tq_enqueue() when the queue is non-empty.
 * So writing 1 here distinguishes the two remaining candidates, and does it
 * without patching the kernel:
 *
 *   desktop recovers  the task really was queued with no one coming to run it
 *                     -- a lost wakeup (or a queue left blocked). This is then
 *                     also a usable recovery mechanism.
 *   nothing happens   the task is not actually dispatchable and the fault is
 *                     elsewhere.
 */
int v3d_sysctl_kick(SYSCTL_HANDLER_ARGS);

int
v3d_sysctl_kick(SYSCTL_HANDLER_ARGS)
{
	struct device *dev = arg1;
	struct drm_device *drm;
	struct v3d_dev *v3d;
	int error, val, q;

	val = 0;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val == 0)
		return (0);

	if (dev == NULL)
		return (ENXIO);
	drm = dev_get_drvdata(dev);
	if (drm == NULL)
		return (ENXIO);
	v3d = to_v3d_dev(drm);

	for (q = 0; q < V3D_MAX_QUEUES; q++) {
		struct v3d_queue_state *qs = &v3d->queue[q];
		struct workqueue_struct *wq = qs->sched.submit_wq;

		printf("V3DKICK: q%d runwork=%d/%d freework=%d/%d wq=%p tq=%p\n",
		    q,
		    atomic_read(&qs->sched.work_run_job.state),
		    qs->sched.work_run_job.work_task.ta_pending,
		    atomic_read(&qs->sched.work_free_job.state),
		    qs->sched.work_free_job.work_task.ta_pending,
		    wq, (wq != NULL) ? wq->taskqueue : NULL);

		if (wq != NULL && wq->taskqueue != NULL)
			taskqueue_unblock(wq->taskqueue);
	}
	printf("V3DKICK: unblocked all submit taskqueues\n");

	return (0);
}

/*
 * Watchdog for the lost taskqueue wakeup (nextbsd#450).
 *
 * Measured signature, captured live while wedged:
 *
 *   q1 render: jobs=80 dep=0 stop=0 credit=0 ready=1 paused=0 runwork=2/1
 *   v3d_render taskqueue thread: IDLE in taskqueue_thread_loop
 *
 * runwork=2/1 is WORK_ST_TASK with ta_pending=1 -- drm_sched's submit work is
 * genuinely enqueued and nothing ever dispatches it. It is self-perpetuating
 * because taskqueue_enqueue_locked() only kicks the thread on the 0->1
 * transition:
 *
 *	if (task->ta_pending) { task->ta_pending++; TQ_UNLOCK(queue); return (0); }
 *
 * so every later queue_work() bumps a counter and wakes nobody. And
 * drm_sched_run_job_work() re-queues itself only while running, so the queue
 * never restarts: one dropped wakeup starves the GPU permanently. Every
 * downstream symptom -- unsignalled fences, X blocked forever in
 * v3d_wait_bo_ioctl or drm_atomic_helper_wait_for_fences, frozen interrupt
 * counters, no drm_sched timeout (it arms only for pending_list jobs, and the
 * job never got that far), and total silence in dmesg -- follows from it.
 *
 * taskqueue_unblock() re-issues tq_enqueue() for a non-empty queue, which is
 * exactly the missing wakeup. Verified on the hardware: a single kick drained
 * 1137 render and 1062 bin jobs in four seconds and fired 2195 interrupts.
 *
 * This is a recovery mechanism for a kernel-level defect, not a fix for it --
 * the lost wakeup itself still needs finding in the taskqueue dispatch path.
 * It is deliberately loud so it can never rot into silently papering over the
 * bug: every kick is printed and counted, and the count is in dev.v3d.0.state.
 *
 * A legitimately queued work item is dispatched in microseconds, so requiring
 * the state to persist across three one-second samples cannot fire on a
 * healthy system.
 */
#define	V3D_WORK_ST_TASK	2	/* enum is private to linux_work.c */
#define	V3D_WD_STRIKES		3

static struct callout	v3d_wd_callout;
static struct device   *v3d_wd_dev;
static int		v3d_wd_strikes[V3D_MAX_QUEUES];
unsigned long		v3d_wd_kicks;

static void
v3d_watchdog(void *arg __unused)
{
	struct drm_device *drm;
	struct v3d_dev *v3d;
	int q;

	if (v3d_wd_dev == NULL)
		goto rearm;
	drm = dev_get_drvdata(v3d_wd_dev);
	if (drm == NULL)
		goto rearm;
	v3d = to_v3d_dev(drm);

	for (q = 0; q < V3D_MAX_QUEUES; q++) {
		struct v3d_queue_state *qs = &v3d->queue[q];
		struct workqueue_struct *wq = qs->sched.submit_wq;

		if (atomic_read(&qs->sched.work_run_job.state) !=
		    V3D_WORK_ST_TASK ||
		    qs->sched.work_run_job.work_task.ta_pending == 0) {
			v3d_wd_strikes[q] = 0;
			continue;
		}

		if (++v3d_wd_strikes[q] < V3D_WD_STRIKES)
			continue;

		v3d_wd_kicks++;
		printf("V3DWD: q%d submit work queued (ta_pending=%d) with an idle "
		    "taskqueue for %ds -- lost wakeup, re-kicking (kick #%lu)\n",
		    q, qs->sched.work_run_job.work_task.ta_pending,
		    v3d_wd_strikes[q], v3d_wd_kicks);

		if (wq != NULL && wq->taskqueue != NULL)
			taskqueue_unblock(wq->taskqueue);

		v3d_wd_strikes[q] = 0;
	}

rearm:
	callout_reset(&v3d_wd_callout, hz, v3d_watchdog, NULL);
}

void v3d_watchdog_start(struct device *dev);
void v3d_watchdog_stop(void);

void
v3d_watchdog_start(struct device *dev)
{
	v3d_wd_dev = dev;
	memset(v3d_wd_strikes, 0, sizeof(v3d_wd_strikes));
	callout_init(&v3d_wd_callout, 1);
	callout_reset(&v3d_wd_callout, hz, v3d_watchdog, NULL);
}

void
v3d_watchdog_stop(void)
{
	v3d_wd_dev = NULL;
	callout_drain(&v3d_wd_callout);
}

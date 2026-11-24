// SPDX-License-Identifier: GPL-2.0
/*
 * lkl_sync - kernel concurrency experiments, run on demand from sysfs.
 *
 *   echo "spin 4 100000" > /sys/kernel/lkl_sync/run   # name threads iters
 *   cat /sys/kernel/lkl_sync/result                     # key=value report
 *
 * Each experiment starts N kernel threads behind a start barrier, runs a
 * workload, and checks an invariant. The write fails with -EIO if the
 * invariant is violated; the report is in `result` either way.
 *
 *   spin       spin_lock_bh in threads + spin_lock in a timer (softirq);
 *              counter must equal thread increments + timer ticks
 *   atomic     atomic64_inc; total must be exact
 *   racy       unlocked READ_ONCE/WRITE_ONCE increment: demonstrates lost
 *              updates (reported, never "fails")
 *   mutex      mutual exclusion checked with an occupancy counter
 *   rcu        readers check a two-field invariant while a writer
 *              publishes new versions (rcu_assign_pointer + kfree_rcu)
 *   waitq      producers/consumers over a bounded buffer and two wait
 *              queues; the checksum of consumed values must match
 *   workqueue  N work items on a private workqueue, then delayed work
 *              with half cancelled by cancel_delayed_work_sync
 *
 * Detector demonstrations, refused (-EOPNOTSUPP) unless the matching
 * debug option is built in, because on a normal kernel they could
 * really deadlock or corrupt memory:
 *
 *   lockdep_softirq  take a softirq-used spinlock without disabling BHs
 *                    (needs CONFIG_PROVE_LOCKING)
 *   kasan_rcu_uaf    rcu experiment with kfree() instead of kfree_rcu()
 *                    (needs CONFIG_KASAN)
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "lkl_compat.h"

#define MAX_THREADS	32U
#define MAX_ITERS	10000000UL
#define RUN_TIMEOUT	(120 * HZ)
#define RESULT_LEN	512
#define RCU_SUM		1000000UL
#define WQ_CAP		16
#define MAX_WORK_ITEMS	100000UL
#define NR_DWORKS	10U

struct exp;

struct worker {
	struct task_struct *task;
	struct exp *e;
	unsigned int id;
};

struct exp {
	const char *name;
	unsigned int nthreads;
	unsigned long iters;
	int (*thread_fn)(struct exp *e, unsigned int id);

	atomic_t go;			/* start barrier */
	wait_queue_head_t go_wq;
	atomic_t abort;
	atomic_t remaining;		/* threads still running thread_fn */
	struct completion done;
	atomic_t failed;		/* thread_fn returned non-zero */

	/* spin */
	spinlock_t lock;
	u64 counter;			/* under lock */
	u64 ticks;			/* under lock */
	struct timer_list timer;
	bool timer_on;

	/* atomic / racy / mutex */
	atomic64_t acounter;
	unsigned long racy;
	struct mutex mtx;		/* the lock under test in "mutex" */
	atomic_t inside;
	atomic_t violations;

	/* rcu */
	struct rcu_cfg __rcu *cfg;
	atomic_t writer_done;
	atomic64_t reads;
	bool rcu_bug;

	/* waitq */
	spinlock_t qlock;
	unsigned long qbuf[WQ_CAP];
	unsigned int qhead, qlen;
	wait_queue_head_t not_empty, not_full;
	unsigned int producers;
	unsigned long to_consume;	/* under qlock */
	u64 sum;			/* under qlock */
};

struct rcu_cfg {
	unsigned long a, b;		/* invariant: a + b == RCU_SUM */
	struct rcu_head rcu;
};

static DEFINE_MUTEX(run_lock);		/* one experiment at a time */
static char result[RESULT_LEN];		/* under run_lock */
static struct kobject *sync_kobj;

static bool aborted(struct exp *e)
{
	return atomic_read(&e->abort);
}

/* ---- thread harness ------------------------------------------------------ */

static int worker_main(void *arg)
{
	struct worker *w = arg;
	struct exp *e = w->e;

	/* Sleep, don't spin: spinning could starve the thread creating us. */
	wait_event(e->go_wq, atomic_read(&e->go) || kthread_should_stop());
	if (!aborted(e) && !kthread_should_stop() &&
	    e->thread_fn(e, w->id))
		atomic_inc(&e->failed);
	if (atomic_dec_and_test(&e->remaining))
		complete(&e->done);

	/* Stay until kthread_stop(): it must never meet an exited task. */
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop())
			schedule();
		__set_current_state(TASK_RUNNING);
	}
	return 0;
}

/* Wake every sleeper so an aborted run can finish. */
static void wake_all(struct exp *e)
{
	wake_up_all(&e->not_empty);
	wake_up_all(&e->not_full);
}

static int run_workers(struct exp *e)
{
	struct worker *ws;
	unsigned int i, started = 0;
	int ret = 0;

	ws = kcalloc(e->nthreads, sizeof(*ws), GFP_KERNEL);
	if (!ws)
		return -ENOMEM;
	atomic_set(&e->remaining, e->nthreads);
	init_completion(&e->done);

	for (i = 0; i < e->nthreads; i++) {
		ws[i].e = e;
		ws[i].id = i;
		ws[i].task = kthread_run(worker_main, &ws[i], "lkl_sync/%u", i);
		if (IS_ERR(ws[i].task)) {
			ret = PTR_ERR(ws[i].task);
			break;
		}
		started++;
	}
	if (ret) {
		/* Threads that never ran thread_fn still count down. */
		atomic_set(&e->abort, 1);
		atomic_sub(e->nthreads - started, &e->remaining);
	}
	atomic_set(&e->go, 1);
	wake_up_all(&e->go_wq);

	if (started && !wait_for_completion_timeout(&e->done, RUN_TIMEOUT)) {
		pr_err("%s: timed out, aborting\n", e->name);
		atomic_set(&e->abort, 1);
		wake_all(e);
		wait_for_completion(&e->done);
		ret = -ETIMEDOUT;
	}
	for (i = 0; i < started; i++)
		kthread_stop(ws[i].task);
	kfree(ws);
	if (!ret && atomic_read(&e->failed))
		ret = -EIO;
	return ret;
}

/* ---- spin: process context vs. softirq --------------------------------------- */

static void spin_timer_fn(struct timer_list *t)
{
	struct exp *e = lkl_timer_container_of(e, t, timer);

	spin_lock(&e->lock);		/* softirq context: BHs already off */
	e->counter++;
	e->ticks++;
	spin_unlock(&e->lock);
	if (READ_ONCE(e->timer_on))
		mod_timer(&e->timer, jiffies + 1);
}

static int spin_thread(struct exp *e, unsigned int id)
{
	unsigned long i;

	for (i = 0; i < e->iters && !aborted(e); i++) {
		/* _bh: the timer takes this lock in softirq context. */
		spin_lock_bh(&e->lock);
		e->counter++;
		spin_unlock_bh(&e->lock);
		if (!(i & 1023))
			cond_resched();
	}
	return 0;
}

static void spin_timer_start(struct exp *e)
{
	timer_setup(&e->timer, spin_timer_fn, 0);
	WRITE_ONCE(e->timer_on, true);
	mod_timer(&e->timer, jiffies + 1);
}

static void spin_timer_stop(struct exp *e)
{
	WRITE_ONCE(e->timer_on, false);
	timer_shutdown_sync(&e->timer);
}

/* ---- atomic / racy / mutex ------------------------------------------------- */

static int atomic_thread(struct exp *e, unsigned int id)
{
	unsigned long i;

	for (i = 0; i < e->iters && !aborted(e); i++) {
		atomic64_inc(&e->acounter);
		if (!(i & 1023))
			cond_resched();
	}
	return 0;
}

static int racy_thread(struct exp *e, unsigned int id)
{
	unsigned long i;

	/*
	 * A read-modify-write without a lock: concurrent increments can be
	 * lost. READ_ONCE/WRITE_ONCE make the race well-defined (no torn
	 * accesses, no compiler tricks) but do not make it atomic.
	 */
	for (i = 0; i < e->iters && !aborted(e); i++) {
		WRITE_ONCE(e->racy, READ_ONCE(e->racy) + 1);
		if (!(i & 1023))
			cond_resched();
	}
	return 0;
}

static int mutex_thread(struct exp *e, unsigned int id)
{
	unsigned long i;

	for (i = 0; i < e->iters && !aborted(e); i++) {
		mutex_lock(&e->mtx);
		if (atomic_inc_return(&e->inside) != 1)
			atomic_inc(&e->violations);
		atomic64_inc(&e->acounter);
		atomic_dec(&e->inside);
		mutex_unlock(&e->mtx);
		if (!(i & 255))
			cond_resched();
	}
	return 0;
}

/* ---- rcu --------------------------------------------------------------------- */

static int rcu_reader(struct exp *e)
{
	while (!atomic_read(&e->writer_done) && !aborted(e)) {
		struct rcu_cfg *c;

		rcu_read_lock();
		c = rcu_dereference(e->cfg);
		/* Read twice: a freed or half-updated object breaks this. */
		if (READ_ONCE(c->a) + READ_ONCE(c->b) != RCU_SUM ||
		    READ_ONCE(c->a) + READ_ONCE(c->b) != RCU_SUM)
			atomic_inc(&e->violations);
		rcu_read_unlock();
		atomic64_inc(&e->reads);
		cond_resched();
	}
	return 0;
}

static int rcu_writer(struct exp *e)
{
	unsigned long i;

	for (i = 0; i < e->iters && !aborted(e); i++) {
		struct rcu_cfg *old, *nc = kmalloc(sizeof(*nc), GFP_KERNEL);

		if (!nc)
			break;
		nc->a = get_random_u32() % RCU_SUM;
		nc->b = RCU_SUM - nc->a;
		/* Single writer: no lock needed around the update. */
		old = rcu_replace_pointer(e->cfg, nc, true);
		if (e->rcu_bug)
			kfree(old);		/* BUG on purpose: readers may hold it */
		else
			kfree_rcu(old, rcu);	/* freed after a grace period */
		if (!(i & 63))
			cond_resched();
	}
	atomic_set(&e->writer_done, 1);
	return 0;
}

static int rcu_thread(struct exp *e, unsigned int id)
{
	return id == 0 ? rcu_writer(e) : rcu_reader(e);
}

/* ---- waitq: bounded buffer -------------------------------------------------- */

static int waitq_producer(struct exp *e, unsigned int id)
{
	unsigned long v;

	for (v = 1; v <= e->iters; v++) {
		spin_lock(&e->qlock);
		while (e->qlen == WQ_CAP && !aborted(e)) {
			spin_unlock(&e->qlock);
			wait_event(e->not_full,
				   READ_ONCE(e->qlen) < WQ_CAP || aborted(e));
			spin_lock(&e->qlock);
		}
		if (aborted(e)) {
			spin_unlock(&e->qlock);
			return 0;
		}
		e->qbuf[(e->qhead + e->qlen) % WQ_CAP] = v;
		WRITE_ONCE(e->qlen, e->qlen + 1);
		spin_unlock(&e->qlock);
		wake_up(&e->not_empty);
	}
	return 0;
}

static bool waitq_can_take(struct exp *e)
{
	return READ_ONCE(e->qlen) > 0 || READ_ONCE(e->to_consume) == 0 ||
	       aborted(e);
}

static int waitq_consumer(struct exp *e, unsigned int id)
{
	for (;;) {
		spin_lock(&e->qlock);
		while (!e->qlen && e->to_consume && !aborted(e)) {
			spin_unlock(&e->qlock);
			wait_event(e->not_empty, waitq_can_take(e));
			spin_lock(&e->qlock);
		}
		if (!e->to_consume || aborted(e)) {
			spin_unlock(&e->qlock);
			return 0;
		}
		e->sum += e->qbuf[e->qhead];
		e->qhead = (e->qhead + 1) % WQ_CAP;
		WRITE_ONCE(e->qlen, e->qlen - 1);
		WRITE_ONCE(e->to_consume, e->to_consume - 1);
		if (!e->to_consume) {
			spin_unlock(&e->qlock);
			wake_up_all(&e->not_empty);	/* release other consumers */
			return 0;
		}
		spin_unlock(&e->qlock);
		wake_up(&e->not_full);
	}
}

static int waitq_thread(struct exp *e, unsigned int id)
{
	return id < e->producers ? waitq_producer(e, id) :
				   waitq_consumer(e, id);
}

/* ---- workqueue --------------------------------------------------------------- */

struct lkl_work {
	struct work_struct work;
	atomic_t *count;
};

struct lkl_dwork {
	struct delayed_work dwork;
	atomic_t *count;
};

static void lkl_work_fn(struct work_struct *w)
{
	atomic_inc(container_of(w, struct lkl_work, work)->count);
}

static void lkl_dwork_fn(struct work_struct *w)
{
	struct delayed_work *dw = to_delayed_work(w);

	atomic_inc(container_of(dw, struct lkl_dwork, dwork)->count);
}

static int run_workqueue(unsigned long n, char *res, size_t len)
{
	struct workqueue_struct *wq;
	struct lkl_work *works;
	struct lkl_dwork *dworks;
	atomic_t count = ATOMIC_INIT(0), dcount = ATOMIC_INIT(0);
	unsigned int cancelled = 0, i;
	unsigned long j;
	int ret = 0;

	works = kvcalloc(n, sizeof(*works), GFP_KERNEL);
	dworks = kcalloc(NR_DWORKS, sizeof(*dworks), GFP_KERNEL);
	if (!works || !dworks) {
		kvfree(works);
		kfree(dworks);
		return -ENOMEM;
	}
	wq = alloc_workqueue("lkl_sync", WQ_UNBOUND, 0);
	if (!wq) {
		kvfree(works);
		kfree(dworks);
		return -ENOMEM;
	}
	for (j = 0; j < n; j++) {
		INIT_WORK(&works[j].work, lkl_work_fn);
		works[j].count = &count;
		queue_work(wq, &works[j].work);
	}
	flush_workqueue(wq);	/* every queued item has run */

	for (i = 0; i < NR_DWORKS; i++) {
		INIT_DELAYED_WORK(&dworks[i].dwork, lkl_dwork_fn);
		dworks[i].count = &dcount;
		queue_delayed_work(wq, &dworks[i].dwork, msecs_to_jiffies(200));
	}
	for (i = 0; i < NR_DWORKS; i += 2)
		cancelled += cancel_delayed_work_sync(&dworks[i].dwork);
	flush_workqueue(wq);
	/* flush does not wait for timers still pending: wait explicitly. */
	for (i = 1; i < NR_DWORKS; i += 2)
		flush_delayed_work(&dworks[i].dwork);

	if (atomic_read(&count) != n || cancelled != NR_DWORKS / 2 ||
	    atomic_read(&dcount) != NR_DWORKS / 2)
		ret = -EIO;
	scnprintf(res, len, "work_items=%lu executed=%d delayed=%u cancelled=%u delayed_executed=%d",
		  n, atomic_read(&count), NR_DWORKS, cancelled, atomic_read(&dcount));
	destroy_workqueue(wq);
	kfree(dworks);
	kvfree(works);
	return ret;
}

/* ---- detector demonstrations -------------------------------------------------- */

/*
 * Take a lock that the timer uses in softirq context, from process
 * context, without disabling bottom halves. If the timer fired on this
 * CPU while we held it, it would spin forever. lockdep reports the
 * inconsistent {SOFTIRQ-ON-W} -> {IN-SOFTIRQ-W} usage when we acquire.
 */
static int demo_lockdep_softirq(struct exp *e, char *res, size_t len)
{
	spin_timer_start(e);
	msleep(50);			/* the timer has now used the lock in softirq */
	spin_lock(&e->lock);		/* BUG on purpose: should be spin_lock_bh */
	e->counter++;
	spin_unlock(&e->lock);
	spin_timer_stop(e);
	scnprintf(res, len, "lockdep_softirq: unsafe acquisition done; expect a lockdep report");
	return 0;
}

/* ---- dispatch ---------------------------------------------------------------- */

static int run_experiment(const char *name, unsigned int nthreads,
			  unsigned long iters, char *res, size_t len)
{
	struct exp *e;
	struct rcu_cfg *c;
	u64 t0, elapsed_us;
	int ret = 0;

	if (nthreads < 1 || nthreads > MAX_THREADS || iters < 1 ||
	    iters > MAX_ITERS)
		return -EINVAL;
	if (!strcmp(name, "workqueue"))
		return iters > MAX_WORK_ITEMS ? -EINVAL :
					       run_workqueue(iters, res, len);

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;
	e->name = name;
	e->nthreads = nthreads;
	e->iters = iters;
	spin_lock_init(&e->lock);
	spin_lock_init(&e->qlock);
	mutex_init(&e->mtx);
	init_waitqueue_head(&e->not_empty);
	init_waitqueue_head(&e->not_full);
	init_waitqueue_head(&e->go_wq);
	t0 = ktime_get_ns();

	if (!strcmp(name, "spin")) {
		e->thread_fn = spin_thread;
		spin_timer_start(e);
		ret = run_workers(e);
		spin_timer_stop(e);
		if (!ret && e->counter != (u64)nthreads * iters + e->ticks)
			ret = -EIO;
		scnprintf(res, len, "counter=%llu expected=%llu timer_ticks=%llu",
			  e->counter, (u64)nthreads * iters + e->ticks, e->ticks);
	} else if (!strcmp(name, "atomic")) {
		e->thread_fn = atomic_thread;
		ret = run_workers(e);
		if (!ret && atomic64_read(&e->acounter) != (s64)nthreads * iters)
			ret = -EIO;
		scnprintf(res, len, "counter=%lld expected=%llu",
			  atomic64_read(&e->acounter), (u64)nthreads * iters);
	} else if (!strcmp(name, "racy")) {
		e->thread_fn = racy_thread;
		ret = run_workers(e);
		scnprintf(res, len, "counter=%lu expected=%llu lost=%llu",
			  e->racy, (u64)nthreads * iters,
			  (u64)nthreads * iters - e->racy);
	} else if (!strcmp(name, "mutex")) {
		e->thread_fn = mutex_thread;
		ret = run_workers(e);
		if (!ret && (atomic_read(&e->violations) ||
			     atomic64_read(&e->acounter) != (s64)nthreads * iters))
			ret = -EIO;
		scnprintf(res, len, "counter=%lld expected=%llu exclusion_violations=%d",
			  atomic64_read(&e->acounter), (u64)nthreads * iters,
			  atomic_read(&e->violations));
	} else if (!strcmp(name, "rcu") || !strcmp(name, "kasan_rcu_uaf")) {
		if (nthreads < 2) {
			ret = -EINVAL;		/* one writer + at least one reader */
			goto out;
		}
		e->rcu_bug = !strcmp(name, "kasan_rcu_uaf");
		if (e->rcu_bug && !IS_ENABLED(CONFIG_KASAN)) {
			ret = -EOPNOTSUPP;
			goto out;
		}
		c = kmalloc(sizeof(*c), GFP_KERNEL);
		if (!c) {
			ret = -ENOMEM;
			goto out;
		}
		c->a = RCU_SUM;
		c->b = 0;
		RCU_INIT_POINTER(e->cfg, c);
		e->thread_fn = rcu_thread;
		ret = run_workers(e);
		/* All readers are gone: free the last version directly. */
		kfree(rcu_dereference_protected(e->cfg, true));
		rcu_barrier();		/* every kfree_rcu callback has run */
		if (!ret && atomic_read(&e->violations))
			ret = -EIO;
		scnprintf(res, len, "updates=%lu reads=%lld invariant_violations=%d",
			  iters, atomic64_read(&e->reads),
			  atomic_read(&e->violations));
	} else if (!strcmp(name, "waitq")) {
		if (nthreads < 2) {
			ret = -EINVAL;		/* at least one of each */
			goto out;
		}
		e->producers = nthreads / 2;
		e->to_consume = e->producers * iters;
		e->thread_fn = waitq_thread;
		ret = run_workers(e);
		/* Each producer sends 1..iters. */
		if (!ret && e->sum != (u64)e->producers * iters * (iters + 1) / 2)
			ret = -EIO;
		scnprintf(res, len, "producers=%u consumers=%u items=%llu sum=%llu expected_sum=%llu",
			  e->producers, nthreads - e->producers,
			  (u64)e->producers * iters, e->sum,
			  (u64)e->producers * iters * (iters + 1) / 2);
	} else if (!strcmp(name, "lockdep_softirq")) {
		if (!IS_ENABLED(CONFIG_PROVE_LOCKING))
			ret = -EOPNOTSUPP;
		else
			ret = demo_lockdep_softirq(e, res, len);
	} else {
		ret = -EINVAL;
	}
out:
	elapsed_us = (ktime_get_ns() - t0) / 1000;
	if (ret != -EINVAL && ret != -EOPNOTSUPP) {
		size_t used = strlen(res);

		scnprintf(res + used, len - used, " elapsed_us=%llu", elapsed_us);
	}
	mutex_destroy(&e->mtx);
	kfree(e);
	return ret;
}

/* ---- sysfs ---------------------------------------------------------------------- */

static ssize_t run_store(struct kobject *k, struct kobj_attribute *a,
			 const char *buf, size_t len)
{
	char name[32], detail[RESULT_LEN - 128];
	unsigned int threads;
	unsigned long iters;
	int ret;

	if (sscanf(buf, "%31s %u %lu", name, &threads, &iters) != 3)
		return -EINVAL;
	if (mutex_lock_interruptible(&run_lock))
		return -ERESTARTSYS;
	detail[0] = '\0';
	ret = run_experiment(name, threads, iters, detail, sizeof(detail));
	scnprintf(result, sizeof(result), "experiment=%s threads=%u iterations=%lu ok=%d error=%d %s\n",
		  name, threads, iters, ret == 0, ret, detail);
	mutex_unlock(&run_lock);
	pr_info("%s", result);
	return ret ? ret : len;
}

static ssize_t result_show(struct kobject *k, struct kobj_attribute *a,
			   char *buf)
{
	ssize_t n;

	mutex_lock(&run_lock);
	n = sysfs_emit(buf, "%s", result[0] ? result : "none\n");
	mutex_unlock(&run_lock);
	return n;
}

static ssize_t detectors_show(struct kobject *k, struct kobj_attribute *a,
			      char *buf)
{
	return sysfs_emit(buf, "lockdep=%d kasan=%d debug_atomic_sleep=%d\n",
			  IS_ENABLED(CONFIG_PROVE_LOCKING),
			  IS_ENABLED(CONFIG_KASAN),
			  IS_ENABLED(CONFIG_DEBUG_ATOMIC_SLEEP));
}

static struct kobj_attribute run_attr = __ATTR_WO(run);
static struct kobj_attribute result_attr = __ATTR_RO(result);
static struct kobj_attribute detectors_attr = __ATTR_RO(detectors);

static struct attribute *sync_attrs[] = {
	&run_attr.attr,
	&result_attr.attr,
	&detectors_attr.attr,
	NULL,
};

static const struct attribute_group sync_group = {
	.attrs = sync_attrs,
};

static int __init lkl_sync_init(void)
{
	int ret;

	sync_kobj = kobject_create_and_add("lkl_sync", kernel_kobj);
	if (!sync_kobj)
		return -ENOMEM;
	ret = sysfs_create_group(sync_kobj, &sync_group);
	if (ret) {
		kobject_put(sync_kobj);
		return ret;
	}
	pr_info("loaded: lockdep=%d kasan=%d\n", IS_ENABLED(CONFIG_PROVE_LOCKING),
		IS_ENABLED(CONFIG_KASAN));
	return 0;
}

/*
 * A running experiment holds a sysfs active reference, so removing the
 * group below waits for it to finish.
 */
static void __exit lkl_sync_exit(void)
{
	sysfs_remove_group(sync_kobj, &sync_group);
	kobject_put(sync_kobj);
	pr_info("unloaded\n");
}

module_init(lkl_sync_init);
module_exit(lkl_sync_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jakub Witkowski");
MODULE_DESCRIPTION("Linux kernel lab: concurrency experiments (spinlocks, mutexes, atomics, RCU, wait queues, workqueues)");
MODULE_VERSION("1.0");

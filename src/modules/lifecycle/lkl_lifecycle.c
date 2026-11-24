// SPDX-License-Identifier: GPL-2.0
/*
 * lkl_lifecycle - initialisation and cleanup done properly.
 *
 * init acquires four resources in order:
 *
 *   1. state      kzalloc'd context
 *   2. samples    kmalloc_array'd history buffer
 *   3. sysfs      /sys/kernel/lkl_lifecycle/{ticks,samples}
 *   4. timer      self-rearming timer that counts ticks
 *
 * Any failure releases exactly what was acquired, in reverse order, via
 * goto labels. The fail_at=N parameter makes step N fail on purpose, so
 * tests can prove every unwind path: the load must fail with -ENOMEM and
 * the log must show steps 1..N-1 released in reverse order.
 *
 * Teardown order matters: the timer is shut down before the memory it
 * touches is freed, and sysfs is removed before the state its show()
 * callbacks read.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/timer.h>

#include "lkl_compat.h"

#define NR_STEPS	4

static unsigned int fail_at;
module_param(fail_at, uint, 0444);
MODULE_PARM_DESC(fail_at, "Make init step N (1-4) fail; 0 = no failure");

static unsigned int tick_ms = 50;
module_param(tick_ms, uint, 0444);
MODULE_PARM_DESC(tick_ms, "Timer period in ms (10..10000, default 50)");

static unsigned int history = 64;
module_param(history, uint, 0444);
MODULE_PARM_DESC(history, "Samples kept (1..4096, default 64)");

struct lkl_state {
	struct timer_list timer;
	atomic_t ticks;
	unsigned long *samples;		/* jiffies of recent ticks, a ring */
	unsigned int nsamples;
	struct kobject *kobj;
};

static struct lkl_state *st;

/* Return true (and log) if step @n should fail on purpose. */
static bool inject(unsigned int n)
{
	if (fail_at != n)
		return false;
	pr_warn("step %u: injected failure\n", n);
	return true;
}

static void lkl_tick(struct timer_list *t)
{
	struct lkl_state *s = lkl_timer_container_of(s, t, timer);
	int n = atomic_inc_return(&s->ticks);

	/*
	 * Timer callbacks never run concurrently with themselves, so this
	 * slot write needs no lock; readers tolerate a stale sample.
	 */
	WRITE_ONCE(s->samples[(unsigned int)(n - 1) % s->nsamples], jiffies);
	mod_timer(&s->timer, jiffies + msecs_to_jiffies(tick_ms));
}

static ssize_t ticks_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&st->ticks));
}

static ssize_t samples_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%u\n", st->nsamples);
}

static struct kobj_attribute ticks_attr = __ATTR_RO(ticks);
static struct kobj_attribute samples_attr = __ATTR_RO(samples);

static struct attribute *lkl_attrs[] = {
	&ticks_attr.attr,
	&samples_attr.attr,
	NULL,
};

static const struct attribute_group lkl_group = {
	.attrs = lkl_attrs,
};

static int __init lkl_lifecycle_init(void)
{
	int ret;

	if (fail_at > NR_STEPS || tick_ms < 10 || tick_ms > 10000 ||
	    history < 1 || history > 4096) {
		pr_err("invalid parameters: fail_at=%u tick_ms=%u history=%u\n",
		       fail_at, tick_ms, history);
		return -EINVAL;
	}

	/* Step 1: state. */
	st = inject(1) ? NULL : kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st) {
		ret = -ENOMEM;
		goto err_out;
	}
	pr_info("step 1: state allocated\n");

	/* Step 2: sample buffer (kmalloc_array checks n * size overflow). */
	st->nsamples = history;
	st->samples = inject(2) ? NULL :
		      kmalloc_array(history, sizeof(*st->samples), GFP_KERNEL);
	if (!st->samples) {
		ret = -ENOMEM;
		goto err_free_state;
	}
	pr_info("step 2: %u samples allocated\n", history);

	/* Step 3: sysfs directory under /sys/kernel. */
	st->kobj = inject(3) ? NULL :
		   kobject_create_and_add(KBUILD_MODNAME, kernel_kobj);
	if (!st->kobj) {
		ret = -ENOMEM;
		goto err_free_samples;
	}
	ret = sysfs_create_group(st->kobj, &lkl_group);
	if (ret)
		goto err_put_kobj;
	pr_info("step 3: sysfs group created\n");

	/* Step 4: the timer. Arm it last, once everything it uses exists. */
	if (inject(4)) {
		ret = -ENOMEM;
		goto err_remove_group;
	}
	timer_setup(&st->timer, lkl_tick, 0);
	mod_timer(&st->timer, jiffies + msecs_to_jiffies(tick_ms));
	pr_info("step 4: timer armed (%u ms)\n", tick_ms);

	pr_info("loaded\n");
	return 0;

err_remove_group:
	sysfs_remove_group(st->kobj, &lkl_group);
	pr_info("undo 3: sysfs group removed\n");
err_put_kobj:
	kobject_put(st->kobj);
err_free_samples:
	kfree(st->samples);
	pr_info("undo 2: samples freed\n");
err_free_state:
	kfree(st);
	st = NULL;
	pr_info("undo 1: state freed\n");
err_out:
	pr_err("init failed: %d\n", ret);
	return ret;
}

static void __exit lkl_lifecycle_exit(void)
{
	/* Reverse order of init. Stop the timer first: it writes samples. */
	timer_shutdown_sync(&st->timer);
	pr_info("undo 4: timer stopped after %d ticks\n",
		atomic_read(&st->ticks));
	sysfs_remove_group(st->kobj, &lkl_group);
	kobject_put(st->kobj);
	pr_info("undo 3: sysfs group removed\n");
	kfree(st->samples);
	pr_info("undo 2: samples freed\n");
	kfree(st);
	pr_info("undo 1: state freed\n");
	pr_info("unloaded\n");
}

module_init(lkl_lifecycle_init);
module_exit(lkl_lifecycle_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jakub Witkowski");
MODULE_DESCRIPTION("Linux kernel lab: ordered init/cleanup with failure injection");
MODULE_VERSION("1.0");

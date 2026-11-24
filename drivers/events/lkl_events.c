// SPDX-License-Identifier: GPL-2.0
/*
 * lkl_events - one event log, exposed through procfs, sysfs and debugfs.
 *
 * The module keeps a ring of the most recent events (text + timestamp +
 * sequence number) and shows the main ways a kernel component talks to
 * user space:
 *
 * procfs  /proc/lkl_lab/
 *   events    seq_file listing, one event per line, oldest first
 *   summary   single_open() overview
 *   control   write "add TEXT", "burst N" or "clear" (clear also
 *             restarts sequence numbers at 0)
 *   snapshot  binary records (include/uapi/linux/lkl_events.h), frozen at
 *             open() so all reads on one file description agree
 *
 * sysfs   /sys/kernel/lkl_lab/
 *   count      events currently held (RO)
 *   capacity   ring size, 1..4096; resizing keeps the newest events (RW)
 *   threshold  0 = off; alert raises when count >= threshold (RW)
 *   alert      0/1; pollable: sysfs_notify() wakes poll(POLLPRI) (RO)
 *   debug/     dropped, last_seq - only with debug_attrs=1 (is_visible)
 *
 * debugfs /sys/kernel/debug/lkl_lab/
 *   adds       u64 counter
 *   ring       internal ring state
 *
 * Locking: one mutex protects the ring and all counters. seq_file's
 * start()/stop() hold it for the duration of each read() chunk.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <linux/lkl_events.h>

#define CAP_MIN		1U
#define CAP_MAX		4096U
#define CONTROL_MAX	128
#define BURST_MAX	100000U

static unsigned int init_capacity = 256;
module_param(init_capacity, uint, 0444);
MODULE_PARM_DESC(init_capacity, "Initial ring size, 1-4096 (default 256)");

static bool debug_attrs;
module_param(debug_attrs, bool, 0444);
MODULE_PARM_DESC(debug_attrs, "Expose /sys/kernel/lkl_lab/debug/");

static DEFINE_MUTEX(ev_lock);	/* protects everything below */
static struct lkl_event_record *ring;	/* slot for seq s: s % cap */
static unsigned int cap;
static u64 total;		/* seq of the next event */
static u64 dropped;		/* discarded: overwritten, resized away, cleared */
static unsigned int threshold;
static bool alert;

static u64 dbg_adds;		/* debugfs counter */

static struct kobject *ev_kobj;
static struct proc_dir_entry *proc_dir;
static struct dentry *dbg_dir;

/* Number of events currently held. */
static unsigned int ev_count(void)
{
	lockdep_assert_held(&ev_lock);
	return total < cap ? (unsigned int)total : cap;
}

static u64 oldest_seq(void)
{
	return total - ev_count();
}

/* Re-evaluate the alert; notify pollers on a change. Called locked. */
static void update_alert(void)
{
	bool now;

	lockdep_assert_held(&ev_lock);
	now = threshold && ev_count() >= threshold;
	if (now != alert) {
		alert = now;
		/* Safe under a mutex: sysfs_notify may sleep, not spin. */
		sysfs_notify(ev_kobj, NULL, "alert");
	}
}

/* Add one event. Non-printable bytes become '?'. Called locked. */
static void add_event_locked(const char *text, size_t len)
{
	struct lkl_event_record *r = &ring[total % cap];
	size_t i;

	if (total >= cap)
		dropped++;
	memset(r, 0, sizeof(*r));
	r->seq = total;
	r->timestamp_ns = ktime_get_ns();
	len = min_t(size_t, len, LKL_EVENT_TEXT_LEN - 1);
	for (i = 0; i < len; i++)
		r->text[i] = isprint(text[i]) ? text[i] : '?';
	total++;
	dbg_adds++;
}

/* ---- sysfs ------------------------------------------------------------- */

static ssize_t count_show(struct kobject *k, struct kobj_attribute *a,
			  char *buf)
{
	unsigned int n;

	mutex_lock(&ev_lock);
	n = ev_count();
	mutex_unlock(&ev_lock);
	return sysfs_emit(buf, "%u\n", n);
}

static ssize_t capacity_show(struct kobject *k, struct kobj_attribute *a,
			     char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(cap));
}

/*
 * Resize keeping the newest min(count, new) events. Each kept event moves
 * to slot seq % new, so the seq-to-slot mapping stays valid.
 */
static ssize_t capacity_store(struct kobject *k, struct kobj_attribute *a,
			      const char *buf, size_t len)
{
	struct lkl_event_record *nring, *old;
	unsigned int ncap, keep;
	u64 s;
	int ret;

	ret = kstrtouint(buf, 0, &ncap);
	if (ret)
		return ret;
	if (ncap < CAP_MIN || ncap > CAP_MAX)
		return -EINVAL;
	nring = kvcalloc(ncap, sizeof(*nring), GFP_KERNEL);
	if (!nring)
		return -ENOMEM;

	mutex_lock(&ev_lock);
	keep = min(ev_count(), ncap);
	dropped += ev_count() - keep;
	for (s = total - keep; s < total; s++)
		nring[s % ncap] = ring[s % cap];
	old = ring;
	ring = nring;
	WRITE_ONCE(cap, ncap);
	update_alert();
	mutex_unlock(&ev_lock);
	kvfree(old);
	return len;
}

static ssize_t threshold_show(struct kobject *k, struct kobj_attribute *a,
			      char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(threshold));
}

static ssize_t threshold_store(struct kobject *k, struct kobj_attribute *a,
			       const char *buf, size_t len)
{
	unsigned int v;
	int ret = kstrtouint(buf, 0, &v);

	if (ret)
		return ret;
	if (v > CAP_MAX)
		return -EINVAL;
	mutex_lock(&ev_lock);
	WRITE_ONCE(threshold, v);
	update_alert();
	mutex_unlock(&ev_lock);
	return len;
}

static ssize_t alert_show(struct kobject *k, struct kobj_attribute *a,
			  char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(alert));
}

static ssize_t dropped_show(struct kobject *k, struct kobj_attribute *a,
			    char *buf)
{
	u64 v;

	mutex_lock(&ev_lock);
	v = dropped;
	mutex_unlock(&ev_lock);
	return sysfs_emit(buf, "%llu\n", v);
}

static ssize_t last_seq_show(struct kobject *k, struct kobj_attribute *a,
			     char *buf)
{
	u64 t;

	mutex_lock(&ev_lock);
	t = total;
	mutex_unlock(&ev_lock);
	if (!t)
		return sysfs_emit(buf, "none\n");
	return sysfs_emit(buf, "%llu\n", t - 1);
}

static struct kobj_attribute count_attr = __ATTR_RO(count);
static struct kobj_attribute capacity_attr = __ATTR_RW(capacity);
static struct kobj_attribute threshold_attr = __ATTR_RW(threshold);
static struct kobj_attribute alert_attr = __ATTR_RO(alert);
static struct kobj_attribute dropped_attr = __ATTR_RO(dropped);
static struct kobj_attribute last_seq_attr = __ATTR_RO(last_seq);

static struct attribute *main_attrs[] = {
	&count_attr.attr,
	&capacity_attr.attr,
	&threshold_attr.attr,
	&alert_attr.attr,
	NULL,
};

static const struct attribute_group main_group = {
	.attrs = main_attrs,
};

static struct attribute *debug_attrs_list[] = {
	&dropped_attr.attr,
	&last_seq_attr.attr,
	NULL,
};

/* Hide every debug attribute unless enabled at load time. */
static umode_t debug_visible(struct kobject *k, struct attribute *attr, int n)
{
	return debug_attrs ? attr->mode : 0;
}

static const struct attribute_group debug_group = {
	.name = "debug",		/* becomes a subdirectory */
	.attrs = debug_attrs_list,
	.is_visible = debug_visible,
};

static const struct attribute_group *ev_groups[] = {
	&main_group,
	&debug_group,
	NULL,
};

/* ---- procfs: events (seq_file) ------------------------------------------ */

/*
 * The seq_file position is an event sequence number, not an index. A
 * read() split across calls resumes at the next unseen seq, so events
 * added between calls never cause duplicates or shifts; events that were
 * overwritten in between are skipped.
 */
static void *events_start(struct seq_file *m, loff_t *pos)
{
	mutex_lock(&ev_lock);
	if (*pos < 0 || (u64)*pos < oldest_seq())
		*pos = oldest_seq();
	if ((u64)*pos >= total)
		return NULL;
	return &ring[*pos % cap];
}

static void *events_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return (u64)*pos < total ? &ring[*pos % cap] : NULL;
}

static void events_stop(struct seq_file *m, void *v)
{
	mutex_unlock(&ev_lock);
}

static int events_show(struct seq_file *m, void *v)
{
	const struct lkl_event_record *r = v;

	seq_printf(m, "%llu %llu %s\n", r->seq, r->timestamp_ns, r->text);
	return 0;
}

static const struct seq_operations events_seq_ops = {
	.start = events_start,
	.next = events_next,
	.stop = events_stop,
	.show = events_show,
};

static int events_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &events_seq_ops);
}

static const struct proc_ops events_proc_ops = {
	.proc_open = events_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = seq_release,
};

/* ---- procfs: summary --------------------------------------------------------- */

static int summary_show(struct seq_file *m, void *v)
{
	mutex_lock(&ev_lock);
	seq_printf(m, "count %u\ncapacity %u\ntotal %llu\ndropped %llu\nthreshold %u\nalert %d\n",
		   ev_count(), cap, total, dropped, threshold, alert);
	mutex_unlock(&ev_lock);
	return 0;
}

/* ---- procfs: control ------------------------------------------------------------ */

static ssize_t control_write(struct file *file, const char __user *ubuf,
			     size_t len, loff_t *ppos)
{
	unsigned int n, i;
	char *cmd, *arg;
	int ret = 0;

	if (len == 0 || len > CONTROL_MAX)
		return -EINVAL;
	cmd = memdup_user_nul(ubuf, len);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);
	strim(cmd);			/* drop the newline from echo */
	arg = strchr(cmd, ' ');
	if (arg)
		*arg++ = '\0';

	mutex_lock(&ev_lock);
	if (!strcmp(cmd, "add") && arg && *arg) {
		add_event_locked(arg, strlen(arg));
	} else if (!strcmp(cmd, "burst") && arg && !kstrtouint(arg, 0, &n) &&
		   n <= BURST_MAX) {
		char text[LKL_EVENT_TEXT_LEN];

		for (i = 0; i < n; i++) {
			int l = scnprintf(text, sizeof(text), "burst-%u", i);

			add_event_locked(text, l);
		}
	} else if (!strcmp(cmd, "clear") && !arg) {
		dropped += ev_count();
		total = 0;
		memset(ring, 0, cap * sizeof(*ring));
	} else {
		ret = -EINVAL;
	}
	if (!ret)
		update_alert();
	mutex_unlock(&ev_lock);
	kfree(cmd);
	return ret ? ret : len;
}

static const struct proc_ops control_proc_ops = {
	.proc_write = control_write,
	.proc_lseek = noop_llseek,
};

/* ---- procfs: snapshot (binary, frozen at open) --------------------------------- */

struct snapshot {
	size_t len;
	u8 data[];
};

static int snapshot_open(struct inode *inode, struct file *file)
{
	struct lkl_events_header *h;
	struct lkl_event_record *out;
	struct snapshot *snap;
	unsigned int n, i;

	mutex_lock(&ev_lock);
	n = ev_count();
	snap = kvzalloc(struct_size(snap, data, sizeof(*h) + n * sizeof(*out)),
			GFP_KERNEL);
	if (!snap) {
		mutex_unlock(&ev_lock);
		return -ENOMEM;
	}
	snap->len = sizeof(*h) + n * sizeof(*out);
	h = (struct lkl_events_header *)snap->data;
	h->magic = LKL_EVENTS_MAGIC;
	h->version = LKL_EVENTS_VERSION;
	h->record_size = sizeof(*out);
	h->count = n;
	h->capacity = cap;
	h->total = total;
	out = (struct lkl_event_record *)(h + 1);
	for (i = 0; i < n; i++)
		out[i] = ring[(oldest_seq() + i) % cap];
	mutex_unlock(&ev_lock);
	file->private_data = snap;
	return 0;
}

static ssize_t snapshot_read(struct file *file, char __user *buf, size_t len,
			     loff_t *ppos)
{
	struct snapshot *snap = file->private_data;

	return simple_read_from_buffer(buf, len, ppos, snap->data, snap->len);
}

static int snapshot_release(struct inode *inode, struct file *file)
{
	kvfree(file->private_data);
	return 0;
}

static const struct proc_ops snapshot_proc_ops = {
	.proc_open = snapshot_open,
	.proc_read = snapshot_read,
	.proc_lseek = default_llseek,
	.proc_release = snapshot_release,
};

/* ---- debugfs -------------------------------------------------------------------- */

static int ring_dbg_show(struct seq_file *m, void *v)
{
	mutex_lock(&ev_lock);
	seq_printf(m, "cap %u total %llu oldest_seq %llu head_slot %llu\n", cap,
		   total, oldest_seq(), total % cap);
	mutex_unlock(&ev_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ring_dbg);

/* ---- load / unload --------------------------------------------------------------- */

static int __init lkl_events_init(void)
{
	int ret;

	if (init_capacity < CAP_MIN || init_capacity > CAP_MAX) {
		pr_err("init_capacity=%u out of range %u..%u\n", init_capacity,
		       CAP_MIN, CAP_MAX);
		return -EINVAL;
	}
	cap = init_capacity;
	ring = kvcalloc(cap, sizeof(*ring), GFP_KERNEL);
	if (!ring)
		return -ENOMEM;

	ev_kobj = kobject_create_and_add("lkl_lab", kernel_kobj);
	if (!ev_kobj) {
		ret = -ENOMEM;
		goto err_ring;
	}
	ret = sysfs_create_groups(ev_kobj, ev_groups);
	if (ret)
		goto err_kobj;

	proc_dir = proc_mkdir("lkl_lab", NULL);
	if (!proc_dir ||
	    !proc_create("events", 0444, proc_dir, &events_proc_ops) ||
	    !proc_create_single("summary", 0444, proc_dir, summary_show) ||
	    !proc_create("control", 0200, proc_dir, &control_proc_ops) ||
	    !proc_create("snapshot", 0400, proc_dir, &snapshot_proc_ops)) {
		ret = -ENOMEM;
		goto err_proc;
	}

	/* debugfs is optional: its helpers handle errors internally. */
	dbg_dir = debugfs_create_dir("lkl_lab", NULL);
	debugfs_create_u64("adds", 0444, dbg_dir, &dbg_adds);
	debugfs_create_file("ring", 0444, dbg_dir, NULL, &ring_dbg_fops);

	pr_info("loaded: capacity %u, debug attributes %s\n", cap,
		debug_attrs ? "on" : "off");
	return 0;

err_proc:
	proc_remove(proc_dir);		/* removes whatever was created */
	sysfs_remove_groups(ev_kobj, ev_groups);
err_kobj:
	kobject_put(ev_kobj);
err_ring:
	kvfree(ring);
	return ret;
}

static void __exit lkl_events_exit(void)
{
	/*
	 * Remove every user-visible entry point before freeing the ring.
	 * proc_remove() and debugfs_remove() wait for in-flight callers.
	 */
	debugfs_remove(dbg_dir);
	proc_remove(proc_dir);
	sysfs_remove_groups(ev_kobj, ev_groups);
	kobject_put(ev_kobj);
	kvfree(ring);
	pr_info("unloaded\n");
}

module_init(lkl_events_init);
module_exit(lkl_events_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jakub Witkowski");
MODULE_DESCRIPTION("Linux kernel lab: procfs, sysfs and debugfs interfaces for an event log");
MODULE_VERSION("1.0");

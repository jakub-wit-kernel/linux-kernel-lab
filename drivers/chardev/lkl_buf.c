// SPDX-License-Identifier: GPL-2.0
/*
 * lkl_buf - a seekable, memory-backed character device.
 *
 * /dev/lkl_buf0 .. /dev/lkl_buf<N-1> each behave like a fixed-capacity
 * file: data written at the file position can be read back, the size
 * grows as data is written past the end, gaps read back as zeros, and
 * lseek supports SEEK_SET/CUR/END/DATA/HOLE. O_APPEND and O_TRUNC work
 * as for regular files.
 *
 * ioctls use extensible structs (see include/uapi/linux/lkl_buf.h): the
 * caller's struct size comes from _IOC_SIZE(cmd), so older and newer
 * user-space binaries keep working as the structs grow.
 *
 * Invariant: every byte in [size, capacity) is zero. Paths that shrink the
 * size (truncate, clear, O_TRUNC) scrub what they cut off, and
 * copy_from_user() zero-fills whatever it fails to copy. Growing the size
 * therefore never needs a memset, and a sparse write or a grow-by-truncate
 * reads back zeros, never stale data.
 *
 * Locking: one mutex per device protects data, size and stats. It is held
 * across copy_{to,from}_user, which may fault and sleep; that is safe
 * because the driver has no mmap, so a fault can never re-enter it.
 * Holding it for the whole read or write also makes each read() and
 * write() atomic with respect to other callers on the same device.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include <linux/lkl_buf.h>

#include "lkl_compat.h"

#define LKLBUF_NAME		"lkl_buf"
#define LKLBUF_MAX_DEVS		16U
#define LKLBUF_MIN_CAPACITY	4096U
#define LKLBUF_MAX_CAPACITY	(16U << 20)

static unsigned int nr_devs = 2;
module_param(nr_devs, uint, 0444);
MODULE_PARM_DESC(nr_devs, "Number of devices (1-16, default 2)");

static unsigned int capacity = 64 * 1024;
module_param(capacity, uint, 0444);
MODULE_PARM_DESC(capacity, "Bytes per device (4 KiB - 16 MiB, default 64 KiB)");

struct lklbuf_dev {
	struct mutex lock;		/* protects data, size, stats */
	u8 *data;
	size_t size;			/* end of file */
	size_t capacity;
	struct lklbuf_stats stats;
	atomic_t open_count;
	unsigned int minor;
	struct cdev cdev;
	struct device *device;
};

static dev_t lklbuf_devt;
static struct class *lklbuf_class;
static struct lklbuf_dev *lklbuf_devs;

/* Move end-of-file forward to @end; the bytes there are already zero. */
static void lklbuf_extend(struct lklbuf_dev *d, size_t end)
{
	lockdep_assert_held(&d->lock);
	if (end > d->size)
		d->size = end;
}

/* Move end-of-file back to @end, scrubbing what is cut off. */
static void lklbuf_shrink(struct lklbuf_dev *d, size_t end)
{
	lockdep_assert_held(&d->lock);
	if (end < d->size) {
		memset(d->data + end, 0, d->size - end);
		d->size = end;
	}
}

static int lklbuf_open(struct inode *inode, struct file *filp)
{
	struct lklbuf_dev *d = container_of(inode->i_cdev, struct lklbuf_dev,
					    cdev);

	filp->private_data = d;
	if ((filp->f_flags & O_ACCMODE) != O_RDONLY &&
	    (filp->f_flags & O_TRUNC)) {
		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;
		lklbuf_shrink(d, 0);
		mutex_unlock(&d->lock);
	}
	atomic_inc(&d->open_count);
	return 0;
}

static int lklbuf_release(struct inode *inode, struct file *filp)
{
	struct lklbuf_dev *d = filp->private_data;

	atomic_dec(&d->open_count);
	return 0;
}

static ssize_t lklbuf_read(struct file *filp, char __user *buf, size_t count,
			   loff_t *ppos)
{
	struct lklbuf_dev *d = filp->private_data;
	size_t n, left;
	ssize_t ret;

	if (*ppos < 0)
		return -EINVAL;
	if (mutex_lock_interruptible(&d->lock))
		return -ERESTARTSYS;
	if ((size_t)*ppos >= d->size) {
		ret = 0;			/* end of file */
		goto out;
	}
	n = min_t(size_t, count, d->size - *ppos);
	left = copy_to_user(buf, d->data + *ppos, n);
	if (left == n && n) {
		ret = -EFAULT;
		goto out;
	}
	n -= left;			/* short copy: report what was read */
	*ppos += n;
	d->stats.reads++;
	d->stats.bytes_read += n;
	ret = n;
out:
	mutex_unlock(&d->lock);
	return ret;
}

static ssize_t lklbuf_write(struct file *filp, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct lklbuf_dev *d = filp->private_data;
	size_t pos, n, left;
	ssize_t ret;

	if (count == 0)
		return 0;
	if (!(filp->f_flags & O_APPEND) && *ppos < 0)
		return -EINVAL;
	if (mutex_lock_interruptible(&d->lock))
		return -ERESTARTSYS;
	pos = (filp->f_flags & O_APPEND) ? d->size : (size_t)*ppos;
	if (pos >= d->capacity) {
		ret = -ENOSPC;
		goto out;
	}
	n = min_t(size_t, count, d->capacity - pos);	/* short at capacity */
	/* On a fault the uncopied tail is zero-filled: invariant holds. */
	left = copy_from_user(d->data + pos, buf, n);
	if (left == n) {
		ret = -EFAULT;
		goto out;
	}
	n -= left;
	lklbuf_extend(d, pos + n);
	*ppos = pos + n;
	d->stats.writes++;
	d->stats.bytes_written += n;
	ret = n;
out:
	mutex_unlock(&d->lock);
	return ret;
}

static loff_t lklbuf_llseek(struct file *filp, loff_t off, int whence)
{
	struct lklbuf_dev *d = filp->private_data;
	loff_t base, pos;

	if (mutex_lock_interruptible(&d->lock))
		return -ERESTARTSYS;
	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = filp->f_pos;
		break;
	case SEEK_END:
		base = d->size;
		break;
	case SEEK_DATA:		/* all data is "data": [0, size) */
	case SEEK_HOLE:		/* the only hole is the implicit one at size */
		if (off < 0 || off >= (loff_t)d->size) {
			pos = -ENXIO;
			goto out;
		}
		pos = whence == SEEK_DATA ? off : (loff_t)d->size;
		goto set;
	default:
		pos = -EINVAL;
		goto out;
	}
	if (check_add_overflow(base, off, &pos) || pos < 0 ||
	    pos > (loff_t)d->capacity) {
		pos = -EINVAL;
		goto out;
	}
set:
	filp->f_pos = pos;
	d->stats.seeks++;
out:
	mutex_unlock(&d->lock);
	return pos;
}

/*
 * Copy an output struct to a caller whose struct is @usize bytes: copy
 * what both sides know and zero any tail the caller's newer struct has.
 */
static int copy_struct_out(void __user *dst, size_t usize, const void *src,
			   size_t ksize)
{
	if (copy_to_user(dst, src, min(usize, ksize)))
		return -EFAULT;
	if (usize > ksize && clear_user(dst + ksize, usize - ksize))
		return -EFAULT;
	return 0;
}

static long ioc_get_info(struct lklbuf_dev *d, void __user *uarg, size_t usize)
{
	struct lklbuf_info info = {
		.api_version = LKLBUF_API_VERSION,
		.minor = d->minor,
		.capacity = d->capacity,
		.open_count = atomic_read(&d->open_count),
	};

	if (usize < LKLBUF_INFO_SIZE_V0)
		return -EINVAL;
	if (mutex_lock_interruptible(&d->lock))
		return -ERESTARTSYS;
	info.size = d->size;
	d->stats.ioctls++;
	mutex_unlock(&d->lock);
	return copy_struct_out(uarg, usize, &info, sizeof(info));
}

static long ioc_fill(struct lklbuf_dev *d, const void __user *uarg,
		     size_t usize)
{
	struct lklbuf_fill f;
	u64 end;
	int ret;

	if (usize < LKLBUF_FILL_SIZE_V0)
		return -EINVAL;
	/* Zero-extends shorter structs; -E2BIG for unknown non-zero tails. */
	ret = copy_struct_from_user(&f, sizeof(f), uarg, usize);
	if (ret)
		return ret;
	if (memchr_inv(f.__pad, 0, sizeof(f.__pad)))
		return -EINVAL;
	if (check_add_overflow(f.offset, f.length, &end) || end > d->capacity)
		return -EINVAL;

	if (mutex_lock_interruptible(&d->lock))
		return -ERESTARTSYS;
	memset(d->data + f.offset, f.byte, f.length);
	lklbuf_extend(d, end);
	d->stats.ioctls++;
	mutex_unlock(&d->lock);
	return 0;
}

static long ioc_truncate(struct lklbuf_dev *d, const u64 __user *uarg)
{
	u64 new_size;

	if (get_user(new_size, uarg))
		return -EFAULT;
	if (new_size > d->capacity)
		return -EINVAL;
	if (mutex_lock_interruptible(&d->lock))
		return -ERESTARTSYS;
	if (new_size < d->size)
		lklbuf_shrink(d, new_size);
	else
		lklbuf_extend(d, new_size);
	d->stats.ioctls++;
	mutex_unlock(&d->lock);
	return 0;
}

static long lklbuf_ioctl(struct file *filp, unsigned int cmd,
			 unsigned long arg)
{
	struct lklbuf_dev *d = filp->private_data;
	void __user *uarg = (void __user *)arg;
	struct lklbuf_stats st;

	if (_IOC_TYPE(cmd) != LKLBUF_IOC_MAGIC)
		return -ENOTTY;

	/*
	 * Extensible ioctls match on number and direction only; the size
	 * bits carry the caller's struct size. Fixed ones match exactly.
	 */
	switch (_IOC_NR(cmd)) {
	case _IOC_NR(LKLBUF_IOC_GET_INFO):
		if (_IOC_DIR(cmd) != _IOC_READ)
			return -ENOTTY;
		return ioc_get_info(d, uarg, _IOC_SIZE(cmd));
	case _IOC_NR(LKLBUF_IOC_FILL):
		if (_IOC_DIR(cmd) != _IOC_WRITE)
			return -ENOTTY;
		return ioc_fill(d, uarg, _IOC_SIZE(cmd));
	case _IOC_NR(LKLBUF_IOC_GET_STATS):
		if (_IOC_DIR(cmd) != _IOC_READ)
			return -ENOTTY;
		if (_IOC_SIZE(cmd) < LKLBUF_STATS_SIZE_V0)
			return -EINVAL;
		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;
		d->stats.ioctls++;
		st = d->stats;
		mutex_unlock(&d->lock);
		return copy_struct_out(uarg, _IOC_SIZE(cmd), &st, sizeof(st));
	}

	switch (cmd) {
	case LKLBUF_IOC_TRUNCATE:
		return ioc_truncate(d, uarg);
	case LKLBUF_IOC_CLEAR:
		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;
		lklbuf_shrink(d, 0);
		d->stats.ioctls++;
		mutex_unlock(&d->lock);
		return 0;
	case LKLBUF_IOC_RESET_STATS:
		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;
		memset(&d->stats, 0, sizeof(d->stats));
		mutex_unlock(&d->lock);
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations lklbuf_fops = {
	.owner		= THIS_MODULE,
	.open		= lklbuf_open,
	.release	= lklbuf_release,
	.read		= lklbuf_read,
	.write		= lklbuf_write,
	.llseek		= lklbuf_llseek,
	.unlocked_ioctl	= lklbuf_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static int lklbuf_dev_setup(struct lklbuf_dev *d, unsigned int minor)
{
	dev_t devt = MKDEV(MAJOR(lklbuf_devt), minor);
	int ret;

	d->minor = minor;
	d->capacity = capacity;
	mutex_init(&d->lock);
	d->data = kvzalloc(capacity, GFP_KERNEL);
	if (!d->data) {
		ret = -ENOMEM;
		goto err_mutex;
	}

	cdev_init(&d->cdev, &lklbuf_fops);
	d->cdev.owner = THIS_MODULE;
	ret = cdev_add(&d->cdev, devt, 1);
	if (ret)
		goto err_free;

	/* After cdev_add: the node only appears once open() can succeed. */
	d->device = device_create(lklbuf_class, NULL, devt, d,
				  LKLBUF_NAME "%u", minor);
	if (IS_ERR(d->device)) {
		ret = PTR_ERR(d->device);
		goto err_cdev;
	}
	return 0;

err_cdev:
	cdev_del(&d->cdev);
err_free:
	kvfree(d->data);
err_mutex:
	mutex_destroy(&d->lock);
	return ret;
}

static void lklbuf_dev_teardown(struct lklbuf_dev *d)
{
	device_destroy(lklbuf_class, d->cdev.dev);
	cdev_del(&d->cdev);
	kvfree(d->data);
	mutex_destroy(&d->lock);
}

static int __init lklbuf_init(void)
{
	unsigned int i;
	int ret;

	if (nr_devs < 1 || nr_devs > LKLBUF_MAX_DEVS ||
	    capacity < LKLBUF_MIN_CAPACITY || capacity > LKLBUF_MAX_CAPACITY) {
		pr_err("invalid parameters: nr_devs=%u capacity=%u\n", nr_devs,
		       capacity);
		return -EINVAL;
	}

	ret = alloc_chrdev_region(&lklbuf_devt, 0, nr_devs, LKLBUF_NAME);
	if (ret)
		return ret;

	lklbuf_class = lkl_class_create(LKLBUF_NAME);
	if (IS_ERR(lklbuf_class)) {
		ret = PTR_ERR(lklbuf_class);
		goto err_region;
	}

	lklbuf_devs = kcalloc(nr_devs, sizeof(*lklbuf_devs), GFP_KERNEL);
	if (!lklbuf_devs) {
		ret = -ENOMEM;
		goto err_class;
	}

	for (i = 0; i < nr_devs; i++) {
		ret = lklbuf_dev_setup(&lklbuf_devs[i], i);
		if (ret)
			goto err_devs;
	}
	pr_info("loaded: %u devices, major %d, capacity %u bytes\n", nr_devs,
		MAJOR(lklbuf_devt), capacity);
	return 0;

err_devs:
	while (i--)
		lklbuf_dev_teardown(&lklbuf_devs[i]);
	kfree(lklbuf_devs);
err_class:
	class_destroy(lklbuf_class);
err_region:
	unregister_chrdev_region(lklbuf_devt, nr_devs);
	pr_err("init failed: %d\n", ret);
	return ret;
}

/* Open files pin the module (fops.owner), so no I/O can race with this. */
static void __exit lklbuf_exit(void)
{
	unsigned int i;

	for (i = 0; i < nr_devs; i++)
		lklbuf_dev_teardown(&lklbuf_devs[i]);
	kfree(lklbuf_devs);
	class_destroy(lklbuf_class);
	unregister_chrdev_region(lklbuf_devt, nr_devs);
	pr_info("unloaded\n");
}

module_init(lklbuf_init);
module_exit(lklbuf_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jakub Witkowski");
MODULE_DESCRIPTION("Linux kernel lab: seekable buffer character device with extensible ioctls");
MODULE_VERSION("1.0");

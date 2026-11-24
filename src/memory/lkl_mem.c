// SPDX-License-Identifier: GPL-2.0
/*
 * lkl_mem - kernel memory allocation experiments and an mmap device.
 *
 * Experiments run on request, one at a time:
 *
 *   echo "cache 10000" > /sys/kernel/lkl_mem/run     # name argument
 *   cat /sys/kernel/lkl_mem/result
 *
 *   kmalloc MAXSIZE  kmalloc() sizes up to MAXSIZE: size class chosen
 *                    (kmalloc_size_roundup), minimum alignment, natural
 *                    alignment of power-of-two sizes
 *   cache N          a kmem_cache with a constructor and 64-byte
 *                    alignment: N objects, constructor state preserved
 *                    across free/alloc
 *   pages ORDER      alloc_pages(): buddy blocks are aligned to their size
 *   vmalloc BYTES    vmalloc(): virtually contiguous, physically scattered
 *
 * Detector demonstrations, refused (-EOPNOTSUPP) without the detector:
 *
 *   leak N           kmalloc() N objects and lose them (CONFIG_DEBUG_KMEMLEAK)
 *   kasan_oob 1      write one byte past a kmalloc(13) (CONFIG_KASAN)
 *
 * /dev/lkl_mmap is a vmalloc'ed buffer (mmap_pages pages) that can be
 * read, written, and mapped shared into user space. Pages are mapped on
 * first touch by a .fault handler; /sys/kernel/lkl_mem/mmap_faults counts
 * the faults.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/kobject.h>
#include <linux/log2.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/vmalloc.h>

#define RESULT_LEN	512
#define MAX_KMALLOC	(1UL << 20)
#define MAX_OBJECTS	100000UL
#define MAX_ORDER_LAB	8UL		/* 1 MiB blocks with 4 KiB pages */
#define PAGE_BLOCKS	4
#define MAX_VMALLOC	(64UL << 20)
#define MAX_LEAKS	64UL
#define OBJ_ALIGN	64
#define CTOR_MAGIC	0xc7041e57U
#define USED_MAGIC	0x05ed05edU

static unsigned int mmap_pages = 16;
module_param(mmap_pages, uint, 0444);
MODULE_PARM_DESC(mmap_pages, "Size of /dev/lkl_mmap in pages (1-1024, default 16)");

static DEFINE_MUTEX(run_lock);		/* one experiment at a time */
static char result[RESULT_LEN];		/* under run_lock */
static struct kobject *mem_kobj;

static void *mbuf;			/* /dev/lkl_mmap backing store */
static size_t mbuf_size;
static atomic64_t mmap_faults;

/* ---- kmalloc --------------------------------------------------------------- */

static int exp_kmalloc(unsigned long max, char *res, size_t len)
{
	static const size_t sizes[] = {
		1, 8, 13, 64, 100, 200, 1000, 4096, 5000, 65536, 200000,
		MAX_KMALLOC,
	};
	size_t used = 0, i;
	int bad = 0;

	if (max < 1 || max > MAX_KMALLOC)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(sizes) && sizes[i] <= max; i++) {
		size_t sz = sizes[i], bucket = kmalloc_size_roundup(sz);
		unsigned long addr;
		u8 *p = kmalloc(sz, GFP_KERNEL);

		if (!p)
			return -ENOMEM;
		addr = (unsigned long)p;
		if (bucket < sz || !IS_ALIGNED(addr, ARCH_KMALLOC_MINALIGN))
			bad++;
		/* kmalloc() guarantees natural alignment for powers of two. */
		if (is_power_of_2(sz) && !IS_ALIGNED(addr, sz))
			bad++;
		memset(p, 0xa5, sz);
		if (memchr_inv(p, 0xa5, sz))
			bad++;
		kfree(p);
		used += scnprintf(res + used, len - used, "%s%zu:%zu",
				  used ? "," : "size:class=", sz, bucket);
	}
	scnprintf(res + used, len - used, " violations=%d", bad);
	return bad ? -EIO : 0;
}

/* ---- kmem_cache with a constructor ------------------------------------------ */

struct lkl_obj {
	u32 magic;
	u32 id;
	u8 payload[52];
};

/*
 * Runs when a slab is populated, not on every allocation: objects must be
 * returned to the constructed state before they are freed.
 */
static void lkl_obj_ctor(void *p)
{
	struct lkl_obj *o = p;

	o->magic = CTOR_MAGIC;
	o->id = 0;
	memset(o->payload, 0, sizeof(o->payload));
}

static int exp_cache(unsigned long n, char *res, size_t len)
{
	struct kmem_cache *cache;
	struct lkl_obj **objs;
	unsigned long i, misaligned = 0, not_constructed = 0;
	int round, ret = 0;

	if (n < 1 || n > MAX_OBJECTS)
		return -EINVAL;
	objs = kvmalloc_array(n, sizeof(*objs), GFP_KERNEL);
	if (!objs)
		return -ENOMEM;
	cache = kmem_cache_create("lkl_mem_obj", sizeof(struct lkl_obj),
				  OBJ_ALIGN, 0, lkl_obj_ctor);
	if (!cache) {
		kvfree(objs);
		return -ENOMEM;
	}
	/* Two rounds: the second reuses objects the first one freed. */
	for (round = 0; round < 2 && !ret; round++) {
		for (i = 0; i < n; i++) {
			objs[i] = kmem_cache_alloc(cache, GFP_KERNEL);
			if (!objs[i]) {
				ret = -ENOMEM;
				break;
			}
			if (!IS_ALIGNED((unsigned long)objs[i], OBJ_ALIGN))
				misaligned++;
			if (objs[i]->magic != CTOR_MAGIC || objs[i]->id)
				not_constructed++;
			objs[i]->magic = USED_MAGIC;
			objs[i]->id = i + 1;
			memset(objs[i]->payload, 0x5a, sizeof(objs[i]->payload));
		}
		while (i--) {
			lkl_obj_ctor(objs[i]);	/* back to constructed state */
			kmem_cache_free(cache, objs[i]);
		}
	}
	scnprintf(res, len, "objects=%lu rounds=2 object_size=%u align=%d misaligned=%lu not_constructed=%lu",
		  n, kmem_cache_size(cache), OBJ_ALIGN, misaligned,
		  not_constructed);
	kmem_cache_destroy(cache);
	kvfree(objs);
	if (!ret && (misaligned || not_constructed))
		ret = -EIO;
	return ret;
}

/* ---- buddy allocator ------------------------------------------------------- */

static int exp_pages(unsigned long order, char *res, size_t len)
{
	struct page *pages[PAGE_BLOCKS];
	unsigned long misaligned = 0, corrupt = 0;
	size_t bytes = PAGE_SIZE << order;
	int i, got = 0;

	if (order > MAX_ORDER_LAB)
		return -EINVAL;
	for (i = 0; i < PAGE_BLOCKS; i++) {
		pages[i] = alloc_pages(GFP_KERNEL | __GFP_NOWARN, order);
		if (!pages[i])
			break;
		got++;
		/* A block of 2^order pages starts on a 2^order pfn boundary. */
		if (page_to_pfn(pages[i]) & ((1UL << order) - 1))
			misaligned++;
		memset(page_address(pages[i]), 0x3c + i, bytes);
	}
	for (i = 0; i < got; i++) {
		if (memchr_inv(page_address(pages[i]), 0x3c + i, bytes))
			corrupt++;
		__free_pages(pages[i], order);
	}
	if (got < PAGE_BLOCKS)
		return -ENOMEM;
	scnprintf(res, len, "order=%lu block_bytes=%zu blocks=%d misaligned=%lu corrupt=%lu",
		  order, bytes, PAGE_BLOCKS, misaligned, corrupt);
	return misaligned || corrupt ? -EIO : 0;
}

/* ---- vmalloc --------------------------------------------------------------- */

static int exp_vmalloc(unsigned long bytes, char *res, size_t len)
{
	unsigned long npages, i, runs = 1, prev;
	u8 *p;
	int ret = 0;

	if (bytes < 1 || bytes > MAX_VMALLOC)
		return -EINVAL;
	p = vmalloc(bytes);
	if (!p)
		return -ENOMEM;
	npages = DIV_ROUND_UP(bytes, PAGE_SIZE);
	/* Count physically contiguous runs behind the virtual range. */
	prev = vmalloc_to_pfn(p);
	for (i = 1; i < npages; i++) {
		unsigned long pfn = vmalloc_to_pfn(p + i * PAGE_SIZE);

		if (pfn != prev + 1)
			runs++;
		prev = pfn;
	}
	memset(p, 0x5a, bytes);
	if (!is_vmalloc_addr(p) || memchr_inv(p, 0x5a, bytes))
		ret = -EIO;
	scnprintf(res, len, "bytes=%lu pages=%lu physical_runs=%lu vmalloc_addr=%d",
		  bytes, npages, runs, is_vmalloc_addr(p));
	vfree(p);
	return ret;
}

/* ---- detector demonstrations ------------------------------------------------- */

/* noinline: kmemleak's backtrace should name this function. */
static noinline void lkl_mem_leak_one(unsigned long i)
{
	char *p = kmalloc(64, GFP_KERNEL);

	if (p)
		snprintf(p, 64, "LKLLEAK-%04lu", i);
	/* BUG on purpose: p is dropped without kfree(). */
}

static int demo_leak(unsigned long n, char *res, size_t len)
{
	unsigned long i;

	if (!IS_ENABLED(CONFIG_DEBUG_KMEMLEAK))
		return -EOPNOTSUPP;
	if (n < 1 || n > MAX_LEAKS)
		return -EINVAL;
	for (i = 0; i < n; i++)
		lkl_mem_leak_one(i);
	scnprintf(res, len, "leaked=%lu bytes_each=64; echo scan > /sys/kernel/debug/kmemleak", n);
	return 0;
}

static noinline int demo_kasan_oob(char *res, size_t len)
{
	u8 *p;
	size_t idx = 13;

	if (!IS_ENABLED(CONFIG_KASAN))
		return -EOPNOTSUPP;
	p = kmalloc(13, GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	/* Hide the index from the compiler, or -Warray-bounds catches it. */
	OPTIMIZER_HIDE_VAR(idx);
	p[idx] = 1;		/* BUG on purpose: one byte past the end */
	kfree(p);
	scnprintf(res, len, "kasan_oob: wrote p[13] of kmalloc(13); expect a KASAN report");
	return 0;
}

/* ---- /dev/lkl_mmap ------------------------------------------------------------- */

static vm_fault_t lkl_mmap_fault(struct vm_fault *vmf)
{
	struct page *page;

	/* vmf->pgoff already includes the mapping's file offset. */
	if (vmf->pgoff >= mbuf_size >> PAGE_SHIFT)
		return VM_FAULT_SIGBUS;
	page = vmalloc_to_page(mbuf + (vmf->pgoff << PAGE_SHIFT));
	get_page(page);			/* the core drops it on unmap */
	vmf->page = page;
	atomic64_inc(&mmap_faults);
	return 0;
}

static const struct vm_operations_struct lkl_mmap_vm_ops = {
	.fault = lkl_mmap_fault,
};

static int lkl_mmap_mmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long npages = mbuf_size >> PAGE_SHIFT;
	unsigned long len = vma_pages(vma);

	/* Private (copy-on-write) mappings of this buffer make no sense. */
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (vma->vm_pgoff >= npages || len > npages - vma->vm_pgoff)
		return -EINVAL;
	vma->vm_ops = &lkl_mmap_vm_ops;
	return 0;
}

static ssize_t lkl_mmap_read(struct file *f, char __user *buf, size_t count,
			     loff_t *ppos)
{
	return simple_read_from_buffer(buf, count, ppos, mbuf, mbuf_size);
}

static ssize_t lkl_mmap_write(struct file *f, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	if (count && *ppos >= mbuf_size)
		return -ENOSPC;
	return simple_write_to_buffer(mbuf, mbuf_size, ppos, buf, count);
}

static loff_t lkl_mmap_llseek(struct file *f, loff_t off, int whence)
{
	return fixed_size_llseek(f, off, whence, mbuf_size);
}

static const struct file_operations lkl_mmap_fops = {
	.owner = THIS_MODULE,
	.read = lkl_mmap_read,
	.write = lkl_mmap_write,
	.llseek = lkl_mmap_llseek,
	.mmap = lkl_mmap_mmap,
};

static struct miscdevice lkl_mmap_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "lkl_mmap",
	.fops = &lkl_mmap_fops,
	.mode = 0600,
};

/* ---- sysfs -------------------------------------------------------------------- */

static int run_experiment(const char *name, unsigned long arg, char *res,
			  size_t len)
{
	if (!strcmp(name, "kmalloc"))
		return exp_kmalloc(arg, res, len);
	if (!strcmp(name, "cache"))
		return exp_cache(arg, res, len);
	if (!strcmp(name, "pages"))
		return exp_pages(arg, res, len);
	if (!strcmp(name, "vmalloc"))
		return exp_vmalloc(arg, res, len);
	if (!strcmp(name, "leak"))
		return demo_leak(arg, res, len);
	if (!strcmp(name, "kasan_oob"))
		return demo_kasan_oob(res, len);
	return -EINVAL;
}

static ssize_t run_store(struct kobject *k, struct kobj_attribute *a,
			 const char *buf, size_t len)
{
	char name[32], detail[RESULT_LEN - 96];
	unsigned long arg;
	int ret;

	if (sscanf(buf, "%31s %lu", name, &arg) != 2)
		return -EINVAL;
	if (mutex_lock_interruptible(&run_lock))
		return -ERESTARTSYS;
	detail[0] = '\0';
	ret = run_experiment(name, arg, detail, sizeof(detail));
	scnprintf(result, sizeof(result), "experiment=%s arg=%lu ok=%d error=%d %s\n",
		  name, arg, ret == 0, ret, detail);
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
	return sysfs_emit(buf, "kmemleak=%d kasan=%d\n",
			  IS_ENABLED(CONFIG_DEBUG_KMEMLEAK),
			  IS_ENABLED(CONFIG_KASAN));
}

static ssize_t mmap_faults_show(struct kobject *k, struct kobj_attribute *a,
				char *buf)
{
	return sysfs_emit(buf, "%lld\n", atomic64_read(&mmap_faults));
}

static struct kobj_attribute run_attr = __ATTR_WO(run);
static struct kobj_attribute result_attr = __ATTR_RO(result);
static struct kobj_attribute detectors_attr = __ATTR_RO(detectors);
static struct kobj_attribute mmap_faults_attr = __ATTR_RO(mmap_faults);

static struct attribute *mem_attrs[] = {
	&run_attr.attr,
	&result_attr.attr,
	&detectors_attr.attr,
	&mmap_faults_attr.attr,
	NULL,
};

static const struct attribute_group mem_group = {
	.attrs = mem_attrs,
};

static int __init lkl_mem_init(void)
{
	int ret;

	if (mmap_pages < 1 || mmap_pages > 1024) {
		pr_err("mmap_pages must be 1-1024, got %u\n", mmap_pages);
		return -EINVAL;
	}
	mbuf_size = (size_t)mmap_pages << PAGE_SHIFT;
	mbuf = vmalloc_user(mbuf_size);		/* zeroed, mappable */
	if (!mbuf)
		return -ENOMEM;

	mem_kobj = kobject_create_and_add("lkl_mem", kernel_kobj);
	if (!mem_kobj) {
		ret = -ENOMEM;
		goto err_buf;
	}
	ret = sysfs_create_group(mem_kobj, &mem_group);
	if (ret)
		goto err_kobj;
	ret = misc_register(&lkl_mmap_dev);
	if (ret)
		goto err_group;
	pr_info("loaded: /dev/lkl_mmap %zu bytes, kmemleak=%d kasan=%d\n",
		mbuf_size, IS_ENABLED(CONFIG_DEBUG_KMEMLEAK),
		IS_ENABLED(CONFIG_KASAN));
	return 0;

err_group:
	sysfs_remove_group(mem_kobj, &mem_group);
err_kobj:
	kobject_put(mem_kobj);
err_buf:
	vfree(mbuf);
	return ret;
}

/*
 * Open files and live mappings hold the module (fops.owner), so no
 * mapping can outlive mbuf.
 */
static void __exit lkl_mem_exit(void)
{
	misc_deregister(&lkl_mmap_dev);
	sysfs_remove_group(mem_kobj, &mem_group);
	kobject_put(mem_kobj);
	vfree(mbuf);
	pr_info("unloaded after %lld mmap faults\n",
		atomic64_read(&mmap_faults));
}

module_init(lkl_mem_init);
module_exit(lkl_mem_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jakub Witkowski");
MODULE_DESCRIPTION("Linux kernel lab: memory allocation experiments and an mmap device");
MODULE_VERSION("1.0");

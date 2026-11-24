// SPDX-License-Identifier: GPL-2.0
/*
 * lkl_logging - the kernel logging toolbox, each tool observable in tests.
 *
 *   - severity levels through pr_err .. pr_info, with a pr_fmt prefix
 *   - pr_debug, compiled in but off until dynamic debug enables it
 *     (e.g. "insmod lkl_logging.ko dyndbg=+p")
 *   - rate limiting with a private ratelimit_state, so a test can predict
 *     exactly how many messages get through
 *   - pr_info_once inside a loop
 *   - print_hex_dump for binary data
 *
 * Writing N to /sys/module/lkl_logging/parameters/emit fires a burst of N
 * rate-limited messages at runtime and logs how many were emitted and
 * suppressed.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>

#define BURST_LIMIT	5		/* messages allowed per interval */
#define BURST_INTERVAL	(60 * HZ)	/* long enough to be deterministic */
#define EMIT_MAX	10000

/*
 * Our own state instead of the shared printk_ratelimit(), so other kernel
 * activity cannot consume our budget and the counts stay predictable.
 */
static DEFINE_RATELIMIT_STATE(burst_rs, BURST_INTERVAL, BURST_LIMIT);
static DEFINE_MUTEX(emit_lock);

static void emit_burst(unsigned int n)
{
	unsigned int i, emitted = 0, suppressed = 0;

	mutex_lock(&emit_lock);
	for (i = 0; i < n; i++) {
		if (__ratelimit(&burst_rs)) {
			pr_info("burst message %u of %u\n", i + 1, n);
			emitted++;
		} else {
			suppressed++;
		}
	}
	mutex_unlock(&emit_lock);
	pr_info("burst done: requested=%u emitted=%u suppressed=%u\n", n,
		emitted, suppressed);
}

static int emit_set(const char *val, const struct kernel_param *kp)
{
	unsigned int n;
	int ret = kstrtouint(val, 0, &n);

	if (ret)
		return ret;
	if (n > EMIT_MAX)
		return -ERANGE;
	emit_burst(n);
	return 0;
}

static const struct kernel_param_ops emit_ops = {
	.set = emit_set,
	.get = NULL,		/* write-only trigger */
};
module_param_cb(emit, &emit_ops, NULL, 0200);
MODULE_PARM_DESC(emit, "Write N to emit a burst of N rate-limited messages");

static unsigned int burst = 20;
module_param(burst, uint, 0444);
MODULE_PARM_DESC(burst, "Messages in the load-time burst (default 20)");

static int __init lkl_logging_init(void)
{
	static const u8 sample[24] = {
		'l', 'k', 'l', 0x00, 0xde, 0xad, 0xbe, 0xef,
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		'h', 'e', 'x', '-', 'd', 'u', 'm', 'p',
	};
	int i;

	if (burst > EMIT_MAX)
		return -EINVAL;

	/* One line per severity (KERN_ERR .. KERN_INFO). */
	pr_err("level demo: error\n");
	pr_warn("level demo: warning\n");
	pr_notice("level demo: notice\n");
	pr_info("level demo: info\n");
	/* Only visible with dynamic debug enabled for this module. */
	pr_debug("level demo: debug (dynamic debug is on)\n");

	for (i = 0; i < 3; i++)
		pr_info_once("printed once although called 3 times\n");

	print_hex_dump(KERN_INFO, KBUILD_MODNAME ": sample: ",
		       DUMP_PREFIX_OFFSET, 16, 1, sample, sizeof(sample), true);

	emit_burst(burst);
	pr_info("loaded\n");
	return 0;
}

static void __exit lkl_logging_exit(void)
{
	pr_info("unloaded\n");
}

module_init(lkl_logging_init);
module_exit(lkl_logging_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jakub Witkowski");
MODULE_DESCRIPTION("Linux kernel lab: logging levels, rate limiting, dynamic debug, hex dumps");
MODULE_VERSION("1.0");

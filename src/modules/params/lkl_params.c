// SPDX-License-Identifier: GPL-2.0
/*
 * lkl_params - module parameters, from the simple to the validated.
 *
 *   count      int, 0644      plain parameter, writable at runtime
 *   name       charp, 0444    string, read-only after load
 *   verbose    bool, 0644
 *   ports      int array      up to 8 values; each must be 1..65535
 *   threshold  uint, 0644     custom ops: accepts only 1..100
 *   mode       enum, 0644     custom ops: "fast" | "safe" | "debug"
 *
 * Writes through /sys/module/lkl_params/parameters/<name> are serialised
 * by the kernel (kernel_param_lock), so the setters need no locking of
 * their own. Code that reads a parameter while it can change uses
 * READ_ONCE so the compiler cannot tear or cache the load.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#define LKL_MAX_PORTS	8

static int count = 3;
module_param(count, int, 0644);
MODULE_PARM_DESC(count, "Plain int parameter (default 3)");

static char *name = "lab";
module_param(name, charp, 0444);
MODULE_PARM_DESC(name, "Instance name (read-only after load)");

static bool verbose;
module_param(verbose, bool, 0644);
MODULE_PARM_DESC(verbose, "Log every parameter change");

static int ports[LKL_MAX_PORTS];
static int nports;
module_param_array(ports, int, &nports, 0444);
MODULE_PARM_DESC(ports, "Up to 8 TCP ports, each 1..65535");

/* ---- threshold: an unsigned int restricted to 1..100 --------------------- */

#define THRESHOLD_MIN	1
#define THRESHOLD_MAX	100

static unsigned int threshold = 50;

static int threshold_set(const char *val, const struct kernel_param *kp)
{
	unsigned int v;
	int ret;

	ret = kstrtouint(val, 0, &v);
	if (ret)
		return ret;
	if (v < THRESHOLD_MIN || v > THRESHOLD_MAX) {
		pr_warn("threshold %u rejected: must be %d..%d\n", v,
			THRESHOLD_MIN, THRESHOLD_MAX);
		return -ERANGE;
	}
	if (READ_ONCE(verbose))
		pr_info("threshold %u -> %u\n", *(unsigned int *)kp->arg, v);
	/* Hand the store to the generic setter for the real write. */
	return param_set_uint(val, kp);
}

static const struct kernel_param_ops threshold_ops = {
	.set = threshold_set,
	.get = param_get_uint,
};
module_param_cb(threshold, &threshold_ops, &threshold, 0644);
MODULE_PARM_DESC(threshold, "Alert threshold, 1..100 (default 50)");

/* ---- mode: a string-backed enum ------------------------------------------- */

enum lkl_mode { MODE_FAST, MODE_SAFE, MODE_DEBUG };

static const char *const mode_names[] = {
	[MODE_FAST] = "fast",
	[MODE_SAFE] = "safe",
	[MODE_DEBUG] = "debug",
};

static enum lkl_mode mode = MODE_SAFE;

static int mode_set(const char *val, const struct kernel_param *kp)
{
	/* sysfs_match_string ignores a trailing newline from "echo". */
	int i = sysfs_match_string(mode_names, val);

	if (i < 0) {
		/* Log without the trailing newline; never modify @val. */
		pr_warn("mode '%.*s' rejected: expected fast, safe or debug\n",
			(int)strcspn(val, "\n"), val);
		return -EINVAL;
	}
	if (READ_ONCE(verbose))
		pr_info("mode %s -> %s\n", mode_names[*(enum lkl_mode *)kp->arg],
			mode_names[i]);
	WRITE_ONCE(*(enum lkl_mode *)kp->arg, (enum lkl_mode)i);
	return 0;
}

static int mode_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%s\n", mode_names[*(enum lkl_mode *)kp->arg]);
}

static const struct kernel_param_ops mode_ops = {
	.set = mode_set,
	.get = mode_get,
};
module_param_cb(mode, &mode_ops, &mode, 0644);
MODULE_PARM_DESC(mode, "Operating mode: fast, safe (default) or debug");

/* ---- load / unload ------------------------------------------------------------ */

static int __init lkl_params_init(void)
{
	int i;

	/*
	 * Array elements arrive through the stock int setter, so range
	 * checks happen here. Failing init aborts the load with this error.
	 */
	for (i = 0; i < nports; i++) {
		if (ports[i] < 1 || ports[i] > 65535) {
			pr_err("ports[%d]=%d out of range 1..65535\n", i,
			       ports[i]);
			return -EINVAL;
		}
	}
	pr_info("loaded: name=%s count=%d verbose=%d threshold=%u mode=%s nports=%d\n",
		name, count, verbose, threshold, mode_names[mode], nports);
	for (i = 0; i < nports; i++)
		pr_info("port[%d]=%d\n", i, ports[i]);
	return 0;
}

static void __exit lkl_params_exit(void)
{
	pr_info("unloaded: final count=%d threshold=%u mode=%s\n",
		READ_ONCE(count), READ_ONCE(threshold),
		mode_names[READ_ONCE(mode)]);
}

module_init(lkl_params_init);
module_exit(lkl_params_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jakub Witkowski");
MODULE_DESCRIPTION("Linux kernel lab: module parameters with validation");
MODULE_VERSION("1.0");

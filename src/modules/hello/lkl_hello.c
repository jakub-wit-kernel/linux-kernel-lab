// SPDX-License-Identifier: GPL-2.0
/*
 * lkl_hello - the smallest useful loadable module.
 *
 * Shows the module skeleton every other module in the lab builds on:
 * __init/__exit sections, pr_fmt-prefixed logging, and module metadata
 * that modinfo can read.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/utsname.h>

/* Set at load time; read at unload to report how long we were loaded. */
static unsigned long loaded_at;

/*
 * __init: the code lives in .init.text and is freed once loading
 * succeeds. Returning non-zero aborts the load and nothing else runs.
 */
static int __init lkl_hello_init(void)
{
	loaded_at = jiffies;
	pr_info("loaded on %s %s (HZ=%d)\n", init_utsname()->sysname,
		init_utsname()->release, HZ);
	return 0;
}

/* __exit: dropped entirely when the code is built into the kernel. */
static void __exit lkl_hello_exit(void)
{
	pr_info("unloaded after %u ms\n",
		jiffies_to_msecs(jiffies - loaded_at));
}

module_init(lkl_hello_init);
module_exit(lkl_hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jakub Witkowski");
MODULE_DESCRIPTION("Linux kernel lab: minimal loadable module");
MODULE_VERSION("1.0");

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * lkl_compat.h - shims for kernel API changes the lab depends on.
 *
 * Supported range: Linux 6.2 and later (6.2 added timer_shutdown_sync()).
 *
 * Shims use feature detection (#ifdef on the new API) rather than
 * kernel version-number comparisons, which break on distribution kernels that
 * backport an API without bumping the version.
 */
#ifndef LKL_COMPAT_H
#define LKL_COMPAT_H

#include <linux/device.h>
#include <linux/timer.h>

/* 6.16 renamed from_timer() to timer_container_of(); both are macros. */
#ifdef timer_container_of
#define lkl_timer_container_of(var, t, field)	timer_container_of(var, t, field)
#else
#define lkl_timer_container_of(var, t, field)	from_timer(var, t, field)
#endif

/*
 * 6.4 dropped the owner argument of class_create(). Before 6.4 it was a
 * macro wrapping __class_create(), so its presence identifies the old API.
 */
#ifdef class_create
#define lkl_class_create(name)	class_create(THIS_MODULE, name)
#else
#define lkl_class_create(name)	class_create(name)
#endif

#endif /* LKL_COMPAT_H */

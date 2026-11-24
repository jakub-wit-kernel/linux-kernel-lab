/* SPDX-License-Identifier: GPL-2.0 */
/*
 * lkl_compat.h - small shims for kernel API changes the lab depends on.
 *
 * Supported range: Linux 5.15 and later. Each shim names the release that
 * introduced the change so it can be dropped when the floor moves.
 */
#ifndef LKL_COMPAT_H
#define LKL_COMPAT_H

#include <linux/timer.h>
#include <linux/version.h>

/*
 * 6.2 added timer_shutdown_sync(): like del_timer_sync() but also
 * prevents the timer from being re-armed, which is what teardown wants.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
#define lkl_timer_shutdown_sync(t)	timer_shutdown_sync(t)
#else
#define lkl_timer_shutdown_sync(t)	del_timer_sync(t)
#endif

/* 6.16 renamed from_timer() to timer_container_of(). */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#define lkl_timer_container_of(var, t, field)	timer_container_of(var, t, field)
#else
#define lkl_timer_container_of(var, t, field)	from_timer(var, t, field)
#endif

#endif /* LKL_COMPAT_H */

/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * lkl_events - binary format of /proc/lkl_lab/snapshot.
 *
 * The file is one header followed by `count` fixed-size records, oldest
 * first. The snapshot is taken when the file is opened, so every read on
 * that file description sees the same consistent data.
 */
#ifndef _UAPI_LINUX_LKL_EVENTS_H
#define _UAPI_LINUX_LKL_EVENTS_H

#include <linux/types.h>

#define LKL_EVENTS_MAGIC	0x56454b4cU	/* "LKEV", little-endian */
#define LKL_EVENTS_VERSION	1
#define LKL_EVENT_TEXT_LEN	48

struct lkl_events_header {
	__u32 magic;		/* LKL_EVENTS_MAGIC */
	__u16 version;		/* LKL_EVENTS_VERSION */
	__u16 record_size;	/* sizeof(struct lkl_event_record) */
	__u32 count;		/* records that follow */
	__u32 capacity;		/* ring capacity when taken */
	__u64 total;		/* events ever added (seq of the next one) */
};

struct lkl_event_record {
	__u64 seq;		/* 0, 1, 2, ... in order of addition */
	__u64 timestamp_ns;	/* CLOCK_MONOTONIC */
	char text[LKL_EVENT_TEXT_LEN];	/* NUL-terminated */
};

#endif /* _UAPI_LINUX_LKL_EVENTS_H */

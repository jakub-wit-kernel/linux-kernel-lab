/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * lkl_buf - user-space API of the seekable buffer character device.
 *
 * /dev/lkl_bufN behaves like a fixed-capacity file: read/write at the file
 * position, lseek (incl. SEEK_DATA/SEEK_HOLE), O_APPEND and O_TRUNC.
 *
 * Extensible ioctl structs
 * ------------------------
 * Structs only ever grow at the end, and every new field must treat zero
 * as "old behaviour". The kernel takes the caller's struct size from the
 * size bits of the ioctl number (_IOC_SIZE), so binaries built against
 * older or newer versions of this header keep working:
 *
 *   input  (_IOW): a shorter struct is zero-extended; a longer struct is
 *                  accepted only if the bytes the kernel does not know
 *                  are zero, otherwise -E2BIG.
 *   output (_IOR): the kernel copies what both sides know and zeroes any
 *                  extra tail the caller provided.
 *
 * Each struct documents its minimum size (the original v0 layout).
 */
#ifndef _UAPI_LINUX_LKL_BUF_H
#define _UAPI_LINUX_LKL_BUF_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define LKLBUF_API_VERSION	1
#define LKLBUF_IOC_MAGIC	0xB8

/* Output. v0 = up to and including `size` (24 bytes). */
struct lklbuf_info {
	__u32 api_version;	/* LKLBUF_API_VERSION of the driver */
	__u32 minor;		/* N in /dev/lkl_bufN */
	__u64 capacity;		/* bytes the device can hold */
	__u64 size;		/* current data size (end of file) */
	/* v1 */
	__u32 open_count;	/* open file descriptions right now */
	__u32 __reserved;
};
#define LKLBUF_INFO_SIZE_V0	24

/* Input. v0 = offset + length (16 bytes); v1 added `byte` (v0: zero). */
struct lklbuf_fill {
	__u64 offset;
	__u64 length;
	/* v1 */
	__u8 byte;		/* value to write; v0 callers get 0 */
	__u8 __pad[7];		/* must be zero */
};
#define LKLBUF_FILL_SIZE_V0	16

/* Output. Monotonic counters until LKLBUF_IOC_RESET_STATS. */
struct lklbuf_stats {
	__u64 reads;
	__u64 writes;
	__u64 bytes_read;
	__u64 bytes_written;
	__u64 seeks;
	__u64 ioctls;
};
#define LKLBUF_STATS_SIZE_V0	48

/* Fill a struct lklbuf_info. */
#define LKLBUF_IOC_GET_INFO	_IOR(LKLBUF_IOC_MAGIC, 1, struct lklbuf_info)
/* Set [offset, offset + length) to `byte`, extending the size if needed. */
#define LKLBUF_IOC_FILL		_IOW(LKLBUF_IOC_MAGIC, 2, struct lklbuf_fill)
/* Set the data size (like ftruncate); growing zero-fills. */
#define LKLBUF_IOC_TRUNCATE	_IOW(LKLBUF_IOC_MAGIC, 3, __u64)
/* Discard all data (size 0); the old contents are zeroed. */
#define LKLBUF_IOC_CLEAR	_IO(LKLBUF_IOC_MAGIC, 4)
/* Fill a struct lklbuf_stats. */
#define LKLBUF_IOC_GET_STATS	_IOR(LKLBUF_IOC_MAGIC, 5, struct lklbuf_stats)
/* Zero the counters. */
#define LKLBUF_IOC_RESET_STATS	_IO(LKLBUF_IOC_MAGIC, 6)

#endif /* _UAPI_LINUX_LKL_BUF_H */

// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include "lklbuf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Layout checks: the UAPI must not change size or field offsets by accident. */
_Static_assert(sizeof(struct lklbuf_info) == 32, "lklbuf_info layout");
_Static_assert(__builtin_offsetof(struct lklbuf_info, open_count) ==
	       LKLBUF_INFO_SIZE_V0, "v1 fields start after the v0 layout");
_Static_assert(sizeof(struct lklbuf_fill) == 24, "lklbuf_fill layout");
_Static_assert(__builtin_offsetof(struct lklbuf_fill, byte) ==
	       LKLBUF_FILL_SIZE_V0, "v1 fields start after the v0 layout");
_Static_assert(sizeof(struct lklbuf_stats) == LKLBUF_STATS_SIZE_V0,
	       "lklbuf_stats layout");

static int ioctl_errno(int fd, unsigned long cmd, void *arg)
{
	int saved = errno, ret = 0;

	if (ioctl(fd, cmd, arg) < 0)
		ret = -errno;
	errno = saved;
	return ret;
}

int lklbuf_path(unsigned int index, char *buf, size_t len)
{
	const char *prefix = getenv("LKLBUF_DEV_PREFIX");
	int n = snprintf(buf, len, "%s%u", prefix ? prefix : "/dev/lkl_buf",
			 index);

	return n < 0 || (size_t)n >= len ? -ENAMETOOLONG : 0;
}

int lklbuf_open(unsigned int index, int flags)
{
	char path[256];
	int saved = errno, fd, ret = lklbuf_path(index, path, sizeof(path));

	if (ret)
		return ret;
	fd = open(path, flags | O_CLOEXEC);
	if (fd < 0)
		fd = -errno;
	errno = saved;
	return fd;
}

int lklbuf_info(int fd, struct lklbuf_info *out)
{
	return ioctl_errno(fd, LKLBUF_IOC_GET_INFO, out);
}

int lklbuf_stats(int fd, struct lklbuf_stats *out)
{
	return ioctl_errno(fd, LKLBUF_IOC_GET_STATS, out);
}

int lklbuf_reset_stats(int fd)
{
	return ioctl_errno(fd, LKLBUF_IOC_RESET_STATS, NULL);
}

int lklbuf_fill(int fd, uint64_t offset, uint64_t length, uint8_t byte)
{
	struct lklbuf_fill f = { .offset = offset, .length = length,
				 .byte = byte };

	return ioctl_errno(fd, LKLBUF_IOC_FILL, &f);
}

int lklbuf_truncate(int fd, uint64_t size)
{
	__u64 v = size;

	return ioctl_errno(fd, LKLBUF_IOC_TRUNCATE, &v);
}

int lklbuf_clear(int fd)
{
	return ioctl_errno(fd, LKLBUF_IOC_CLEAR, NULL);
}

int lklbuf_parse_size(const char *s, uint64_t *out)
{
	uint64_t v = 0, mult = 1;
	const char *p = s;

	if (!s || *s < '0' || *s > '9')
		return -EINVAL;
	for (; *p >= '0' && *p <= '9'; p++) {
		unsigned int digit = (unsigned int)(*p - '0');

		if (v > (UINT64_MAX - digit) / 10)
			return -ERANGE;
		v = v * 10 + digit;
	}
	switch (*p) {
	case '\0':
		break;
	case 'k': case 'K':
		mult = 1ull << 10;
		p++;
		break;
	case 'm': case 'M':
		mult = 1ull << 20;
		p++;
		break;
	case 'g': case 'G':
		mult = 1ull << 30;
		p++;
		break;
	default:
		return -EINVAL;
	}
	if (*p)
		return -EINVAL;
	if (v > UINT64_MAX / mult)
		return -ERANGE;
	*out = v * mult;
	return 0;
}

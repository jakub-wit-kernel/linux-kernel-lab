/* SPDX-License-Identifier: GPL-2.0 */
/*
 * liblklbuf - thin, typed wrappers around the lkl_buf ioctls.
 *
 * Every function returns 0 (or a non-negative value) on success and a
 * negative errno on failure; errno itself is left untouched, so the
 * library composes cleanly with callers that keep their own errno.
 */
#ifndef LKLBUF_H
#define LKLBUF_H

#include <stddef.h>
#include <stdint.h>

#include <linux/lkl_buf.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "/dev/lkl_buf<index>", or "$LKLBUF_DEV_PREFIX<index>" if set. */
int lklbuf_path(unsigned int index, char *buf, size_t len);
/* open(2) the device; returns an fd or -errno. */
int lklbuf_open(unsigned int index, int flags);

int lklbuf_info(int fd, struct lklbuf_info *out);
int lklbuf_stats(int fd, struct lklbuf_stats *out);
int lklbuf_reset_stats(int fd);
int lklbuf_fill(int fd, uint64_t offset, uint64_t length, uint8_t byte);
int lklbuf_truncate(int fd, uint64_t size);
int lklbuf_clear(int fd);

/*
 * Parse a byte count: decimal digits with an optional K/M/G suffix
 * (powers of 1024, case-insensitive). Rejects empty strings, signs,
 * trailing junk and overflow. Returns 0 or -EINVAL / -ERANGE.
 */
int lklbuf_parse_size(const char *s, uint64_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LKLBUF_H */

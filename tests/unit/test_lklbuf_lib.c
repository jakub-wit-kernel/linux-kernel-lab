// SPDX-License-Identifier: GPL-2.0
/* liblklbuf logic that needs no kernel: size parsing and device paths. */
#define _GNU_SOURCE
#define KT_IMPLEMENTATION
#include "kt.h"

#include <stdint.h>

#include "lklbuf.h"

KT_TEST(parse_plain_and_suffixed_sizes)
{
	uint64_t v;

	KT_EQ(lklbuf_parse_size("0", &v), 0);
	KT_EQ(v, 0);
	KT_EQ(lklbuf_parse_size("4096", &v), 0);
	KT_EQ(v, 4096);
	KT_EQ(lklbuf_parse_size("4K", &v), 0);
	KT_EQ(v, 4096);
	KT_EQ(lklbuf_parse_size("64k", &v), 0);
	KT_EQ(v, 65536);
	KT_EQ(lklbuf_parse_size("16M", &v), 0);
	KT_EQ(v, 16u << 20);
	KT_EQ(lklbuf_parse_size("2g", &v), 0);
	KT_EQ(v, 2ull << 30);
	KT_EQ(lklbuf_parse_size("18446744073709551615", &v), 0);
	KT_ASSERT(v == UINT64_MAX);
}

KT_TEST(parse_rejects_junk_and_overflow)
{
	static const char *const bad[] = { "", "-1", "+5", " 5", "5 ", "5KB",
					   "K", "0x10", "1.5M", "12a" };
	uint64_t v = 7;

	for (size_t i = 0; i < KT_N(bad); i++)
		if (lklbuf_parse_size(bad[i], &v) != -EINVAL)
			KT_FAIL("'%s' should be -EINVAL", bad[i]);
	KT_EQ(lklbuf_parse_size(NULL, &v), -EINVAL);
	KT_EQ(lklbuf_parse_size("18446744073709551616", &v), -ERANGE);
	KT_EQ(lklbuf_parse_size("17179869184G", &v), -ERANGE);	/* 2^64 */
	KT_EQ(v, 7);		/* untouched on failure */
}

KT_TEST(device_path_default_and_override)
{
	char p[64];

	unsetenv("LKLBUF_DEV_PREFIX");
	KT_EQ(lklbuf_path(3, p, sizeof(p)), 0);
	KT_ASSERT(strcmp(p, "/dev/lkl_buf3") == 0);
	setenv("LKLBUF_DEV_PREFIX", "/tmp/fake", 1);
	KT_EQ(lklbuf_path(0, p, sizeof(p)), 0);
	KT_ASSERT(strcmp(p, "/tmp/fake0") == 0);
	KT_EQ(lklbuf_path(0, p, 5), -ENAMETOOLONG);
	unsetenv("LKLBUF_DEV_PREFIX");
}

KT_TEST(missing_device_returns_negative_errno)
{
	int saved;

	setenv("LKLBUF_DEV_PREFIX", "/nonexistent/lkl_buf", 1);
	errno = 1234;
	saved = errno;
	KT_EQ(lklbuf_open(0, 0), -ENOENT);
	KT_EQ(errno, saved);		/* the library leaves errno alone */
	KT_EQ(lklbuf_info(-1, NULL), -EBADF);
	unsetenv("LKLBUF_DEV_PREFIX");
}

static const struct kt_case cases[] = {
	KT_CASE(parse_plain_and_suffixed_sizes),
	KT_CASE(parse_rejects_junk_and_overflow),
	KT_CASE(device_path_default_and_override),
	KT_CASE(missing_device_returns_negative_errno),
};

int main(int argc, char **argv)
{
	return kt_run(cases, KT_N(cases), argc, argv);
}

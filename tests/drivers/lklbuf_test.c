// SPDX-License-Identifier: GPL-2.0
/*
 * Device tests for lkl_buf (module loaded, run as root). TAP output.
 * Exits 77 ("skipped") if /dev/lkl_buf0 is missing.
 */
#define _GNU_SOURCE
#define KT_IMPLEMENTATION
#include "kt.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "lklbuf.h"

static uint64_t g_cap;

/* A fresh, empty device 0 opened read-write. */
static int fresh(void)
{
	int fd = lklbuf_open(0, O_RDWR);

	if (fd < 0)
		KT_FAIL("open: %s", strerror(-fd));
	KT_EQ(lklbuf_clear(fd), 0);
	KT_EQ(lklbuf_reset_stats(fd), 0);
	return fd;
}

static uint64_t size_of(int fd)
{
	struct lklbuf_info i;

	KT_EQ(lklbuf_info(fd, &i), 0);
	return i.size;
}

static int all_bytes(const unsigned char *p, size_t n, unsigned char v)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != v)
			return 0;
	return 1;
}

/* ---- file semantics ------------------------------------------------------ */

KT_TEST(info_reports_device)
{
	int fd = fresh();
	struct lklbuf_info i;

	KT_EQ(lklbuf_info(fd, &i), 0);
	KT_EQ(i.api_version, LKLBUF_API_VERSION);
	KT_EQ(i.minor, 0);
	KT_ASSERT(i.capacity >= 4096);
	KT_EQ(i.size, 0);
	KT_ASSERT(i.open_count >= 1);
	g_cap = i.capacity;
	close(fd);
}

KT_TEST(write_seek_read_roundtrip)
{
	int fd = fresh();
	char buf[16] = { 0 };

	KT_EQ(write(fd, "hello", 5), 5);
	KT_EQ(lseek(fd, 0, SEEK_CUR), 5);
	KT_EQ(lseek(fd, 0, SEEK_SET), 0);
	KT_EQ(read(fd, buf, sizeof(buf)), 5);
	KT_ASSERT(memcmp(buf, "hello", 5) == 0);
	KT_EQ(read(fd, buf, sizeof(buf)), 0);		/* EOF */
	KT_EQ(size_of(fd), 5);
	close(fd);
}

KT_TEST(sparse_write_reads_back_zeros)
{
	int fd = fresh();
	unsigned char buf[1000];

	KT_EQ(pwrite(fd, "X", 1, 1000), 1);
	KT_EQ(size_of(fd), 1001);
	memset(buf, 0xEE, sizeof(buf));
	KT_EQ(pread(fd, buf, sizeof(buf), 0), (ssize_t)sizeof(buf));
	KT_ASSERT(all_bytes(buf, sizeof(buf), 0));
	close(fd);
}

/* Shrink then grow must never resurface the old bytes (scrub invariant). */
KT_TEST(truncate_never_leaks_old_data)
{
	int fd = fresh();
	unsigned char buf[100];

	memset(buf, 'A', sizeof(buf));
	KT_EQ(pwrite(fd, buf, sizeof(buf), 0), 100);
	KT_EQ(lklbuf_truncate(fd, 10), 0);
	KT_EQ(size_of(fd), 10);
	KT_EQ(lklbuf_truncate(fd, 100), 0);
	KT_EQ(pread(fd, buf, sizeof(buf), 0), 100);
	KT_ASSERT(all_bytes(buf, 10, 'A'));
	KT_ASSERT(all_bytes(buf + 10, 90, 0));
	/* Same through a sparse write past the shrunk end. */
	KT_EQ(lklbuf_truncate(fd, 5), 0);
	KT_EQ(pwrite(fd, "Z", 1, 99), 1);
	KT_EQ(pread(fd, buf, sizeof(buf), 0), 100);
	KT_ASSERT(all_bytes(buf + 5, 94, 0));
	KT_EQ(lklbuf_truncate(fd, g_cap + 1), -EINVAL);
	close(fd);
}

KT_TEST(llseek_modes)
{
	int fd = fresh();

	KT_EQ(pwrite(fd, "0123456789", 10, 0), 10);
	KT_EQ(lseek(fd, -3, SEEK_END), 7);
	KT_EQ(lseek(fd, 2, SEEK_CUR), 9);
	KT_ERRNO(lseek(fd, -1, SEEK_SET), EINVAL);
	KT_ERRNO(lseek(fd, (off_t)g_cap + 1, SEEK_SET), EINVAL);
	KT_EQ(lseek(fd, (off_t)g_cap, SEEK_SET), (off_t)g_cap);	/* at the end is fine */
	KT_EQ(lseek(fd, 4, SEEK_DATA), 4);
	KT_EQ(lseek(fd, 4, SEEK_HOLE), 10);
	KT_ERRNO(lseek(fd, 10, SEEK_DATA), ENXIO);
	KT_ERRNO(lseek(fd, 0, 99), EINVAL);
	close(fd);
}

KT_TEST(capacity_gives_short_write_then_enospc)
{
	int fd = fresh();
	char buf[64] = { 0 };

	KT_EQ(pwrite(fd, buf, sizeof(buf), (off_t)g_cap - 10), 10);	/* short */
	KT_EQ(size_of(fd), g_cap);
	KT_ERRNO(pwrite(fd, buf, 1, (off_t)g_cap), ENOSPC);
	KT_EQ(write(fd, buf, 0), 0);
	close(fd);
}

KT_TEST(o_append_always_writes_at_end)
{
	int fd = fresh();
	int ap = lklbuf_open(0, O_WRONLY | O_APPEND);
	char buf[8] = { 0 };

	KT_ASSERT(ap >= 0);
	KT_EQ(pwrite(fd, "abc", 3, 0), 3);
	lseek(ap, 0, SEEK_SET);
	KT_EQ(write(ap, "de", 2), 2);
	KT_EQ(pread(fd, buf, sizeof(buf), 0), 5);
	KT_ASSERT(memcmp(buf, "abcde", 5) == 0);
	close(ap);
	close(fd);
}

KT_TEST(o_trunc_empties_device)
{
	int fd = fresh();
	int t;

	KT_EQ(pwrite(fd, "data", 4, 0), 4);
	t = lklbuf_open(0, O_WRONLY | O_TRUNC);
	KT_ASSERT(t >= 0);
	KT_EQ(size_of(fd), 0);
	close(t);
	/* O_TRUNC is ignored for read-only opens, as for files. */
	KT_EQ(pwrite(fd, "data", 4, 0), 4);
	t = lklbuf_open(0, O_RDONLY | O_TRUNC);
	KT_ASSERT(t >= 0);
	KT_EQ(size_of(fd), 4);
	close(t);
	close(fd);
}

KT_TEST(bad_user_pointer_is_efault)
{
	int fd = fresh();
	void *bad = (void *)(uintptr_t)8;

	KT_ERRNO(syscall(SYS_pwrite64, fd, bad, 10, 0), EFAULT);
	KT_EQ(size_of(fd), 0);		/* nothing was written */
	KT_EQ(pwrite(fd, "abc", 3, 0), 3);
	KT_ERRNO(syscall(SYS_pread64, fd, bad, 3, 0), EFAULT);
	KT_ERRNO(syscall(SYS_ioctl, fd, LKLBUF_IOC_GET_INFO, bad), EFAULT);
	close(fd);
}

/* ---- ioctls ---------------------------------------------------------------- */

KT_TEST(fill_ioctl)
{
	int fd = fresh();
	unsigned char buf[30];

	KT_EQ(lklbuf_fill(fd, 10, 10, 0xAB), 0);
	KT_EQ(size_of(fd), 20);
	KT_EQ(pread(fd, buf, sizeof(buf), 0), 20);
	KT_ASSERT(all_bytes(buf, 10, 0) && all_bytes(buf + 10, 10, 0xAB));
	KT_EQ(lklbuf_fill(fd, g_cap - 5, 6, 1), -EINVAL);	/* past capacity */
	KT_EQ(lklbuf_fill(fd, UINT64_MAX - 1, 4, 1), -EINVAL);	/* overflow */

	struct lklbuf_fill f = { .offset = 0, .length = 1, .__pad = { 1 } };

	KT_ERRNO(ioctl(fd, LKLBUF_IOC_FILL, &f), EINVAL);	/* pad must be 0 */
	close(fd);
}

KT_TEST(stats_count_operations)
{
	int fd = fresh();
	struct lklbuf_stats s;
	char buf[4];

	KT_EQ(write(fd, "ab", 2), 2);
	KT_EQ(write(fd, "cd", 2), 2);
	lseek(fd, 0, SEEK_SET);
	KT_EQ(read(fd, buf, 4), 4);
	KT_EQ(lklbuf_stats(fd, &s), 0);
	KT_EQ(s.writes, 2);
	KT_EQ(s.bytes_written, 4);
	KT_EQ(s.reads, 1);
	KT_EQ(s.bytes_read, 4);
	KT_EQ(s.seeks, 1);
	KT_EQ(s.ioctls, 1);		/* the GET_STATS itself */
	close(fd);
}

/* ---- extensible ioctl structs ---------------------------------------------- */

#define IOR_SZ(nr, sz)	_IOC(_IOC_READ, LKLBUF_IOC_MAGIC, nr, sz)
#define IOW_SZ(nr, sz)	_IOC(_IOC_WRITE, LKLBUF_IOC_MAGIC, nr, sz)

KT_TEST(info_accepts_older_and_newer_callers)
{
	int fd = fresh();
	unsigned char buf[64];
	struct lklbuf_info cur;

	KT_EQ(pwrite(fd, "xyz", 3, 0), 3);
	KT_EQ(lklbuf_info(fd, &cur), 0);

	/* v0 caller: 24-byte struct. Gets the v0 fields, nothing more. */
	memset(buf, 0xCC, sizeof(buf));
	KT_OK(ioctl(fd, IOR_SZ(1, LKLBUF_INFO_SIZE_V0), buf));
	KT_ASSERT(memcmp(buf, &cur, LKLBUF_INFO_SIZE_V0) == 0);
	KT_ASSERT(all_bytes(buf + LKLBUF_INFO_SIZE_V0, 8, 0xCC));

	/* Future caller: 48-byte struct. Unknown tail comes back zeroed. */
	memset(buf, 0xCC, sizeof(buf));
	KT_OK(ioctl(fd, IOR_SZ(1, 48), buf));
	KT_ASSERT(memcmp(buf, &cur, sizeof(cur)) == 0);
	KT_ASSERT(all_bytes(buf + sizeof(cur), 48 - sizeof(cur), 0));
	KT_ASSERT(all_bytes(buf + 48, 16, 0xCC));	/* nothing past it */

	/* Smaller than the v0 layout: rejected. */
	KT_ERRNO(ioctl(fd, IOR_SZ(1, 8), buf), EINVAL);
	close(fd);
}

KT_TEST(fill_accepts_v0_and_checks_unknown_fields)
{
	int fd = fresh();
	unsigned char buf[64] = { 0 }, data[8];
	uint64_t *u = (uint64_t *)buf;

	/* v0 caller (16 bytes, no `byte` field): fills with zero. */
	KT_EQ(pwrite(fd, "QQQQQQQQ", 8, 0), 8);
	u[0] = 2;
	u[1] = 4;
	KT_OK(ioctl(fd, IOW_SZ(2, LKLBUF_FILL_SIZE_V0), buf));
	KT_EQ(pread(fd, data, 8, 0), 8);
	KT_ASSERT(memcmp(data, "QQ\0\0\0\0QQ", 8) == 0);

	/* Future caller, unknown fields zero: accepted. */
	memset(buf, 0, sizeof(buf));
	u[0] = 0;
	u[1] = 1;
	buf[16] = 'R';
	KT_OK(ioctl(fd, IOW_SZ(2, 40), buf));
	KT_EQ(pread(fd, data, 1, 0), 1);
	KT_EQ(data[0], 'R');

	/* Future caller relying on a field we do not know: refused. */
	buf[32] = 1;
	KT_ERRNO(ioctl(fd, IOW_SZ(2, 40), buf), E2BIG);
	KT_ERRNO(ioctl(fd, IOW_SZ(2, 8), buf), EINVAL);		/* too small */
	close(fd);
}

KT_TEST(malformed_commands_are_enotty)
{
	int fd = fresh();
	struct lklbuf_info i;

	KT_ERRNO(ioctl(fd, _IOC(_IOC_WRITE, LKLBUF_IOC_MAGIC, 1, sizeof(i)), &i), ENOTTY);
	KT_ERRNO(ioctl(fd, _IOR(LKLBUF_IOC_MAGIC, 99, int), &i), ENOTTY);
	KT_ERRNO(ioctl(fd, _IOR(0x42, 1, struct lklbuf_info), &i), ENOTTY);
	/* Fixed-size commands do not accept other sizes. */
	KT_ERRNO(ioctl(fd, _IOW(LKLBUF_IOC_MAGIC, 3, uint32_t), &i), ENOTTY);
	close(fd);
}

KT_TEST(devices_are_independent)
{
	int a = fresh(), b = lklbuf_open(1, O_RDWR);

	if (b < 0) {
		close(a);
		KT_SKIP("/dev/lkl_buf1 not present");
	}
	KT_EQ(lklbuf_clear(b), 0);
	KT_EQ(pwrite(a, "only-a", 6, 0), 6);
	KT_EQ(size_of(b), 0);
	struct lklbuf_info i;

	KT_EQ(lklbuf_info(b, &i), 0);
	KT_EQ(i.minor, 1);
	close(a);
	close(b);
}

/* ---- concurrency ---------------------------------------------------------- */

#define BLK 4096

struct conc {
	int stop;		/* atomic */
	int torn;		/* atomic */
	long reads;		/* atomic */
	int err;
};

/* Alternates whole blocks of 'A' and 'B' at offset 0. */
static void *flipper(void *arg)
{
	struct conc *c = arg;
	int fd = lklbuf_open(0, O_RDWR);
	unsigned char a[BLK], b[BLK];

	memset(a, 'A', BLK);
	memset(b, 'B', BLK);
	for (int i = 0; !__atomic_load_n(&c->stop, __ATOMIC_RELAXED); i++)
		if (pwrite(fd, i & 1 ? a : b, BLK, 0) != BLK)
			c->err = errno;
	close(fd);
	return NULL;
}

/* Every read must see one whole block: all 'A' or all 'B', never a mix. */
static void *checker(void *arg)
{
	struct conc *c = arg;
	int fd = lklbuf_open(0, O_RDONLY);
	unsigned char buf[BLK];

	while (!__atomic_load_n(&c->stop, __ATOMIC_RELAXED)) {
		if (pread(fd, buf, BLK, 0) != BLK)
			continue;
		if (!all_bytes(buf, BLK, 'A') && !all_bytes(buf, BLK, 'B'))
			__atomic_fetch_add(&c->torn, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&c->reads, 1, __ATOMIC_RELAXED);
	}
	close(fd);
	return NULL;
}

KT_TEST(reads_never_observe_a_torn_write)
{
	int fd = fresh();
	struct conc c = { 0 };
	pthread_t w[2], r[3];

	KT_EQ(lklbuf_fill(fd, 0, BLK, 'A'), 0);
	for (int i = 0; i < 2; i++)
		pthread_create(&w[i], NULL, flipper, &c);
	for (int i = 0; i < 3; i++)
		pthread_create(&r[i], NULL, checker, &c);
	usleep(500 * 1000);
	__atomic_store_n(&c.stop, 1, __ATOMIC_RELAXED);
	for (int i = 0; i < 2; i++)
		pthread_join(w[i], NULL);
	for (int i = 0; i < 3; i++)
		pthread_join(r[i], NULL);
	printf("  # %ld concurrent block reads checked\n", c.reads);
	KT_EQ(c.err, 0);
	KT_ASSERT(c.reads > 100);
	KT_EQ(c.torn, 0);
	close(fd);
}

struct region {
	int idx;
	int err;
};

/* Each thread owns one region and rewrites it with its own pattern. */
static void *region_writer(void *arg)
{
	struct region *rg = arg;
	int fd = lklbuf_open(0, O_RDWR);
	unsigned char buf[BLK];

	memset(buf, 'a' + rg->idx, BLK);
	for (int i = 0; i < 200; i++)
		if (pwrite(fd, buf, BLK, (off_t)rg->idx * BLK) != BLK)
			rg->err = errno;
	close(fd);
	return NULL;
}

KT_TEST(concurrent_writers_to_disjoint_regions)
{
	enum { N = 8 };
	int fd = fresh();
	struct region rg[N];
	pthread_t t[N];
	unsigned char buf[BLK];

	if (g_cap < (uint64_t)N * BLK)
		KT_SKIP("capacity below %d bytes", N * BLK);
	for (int i = 0; i < N; i++) {
		rg[i] = (struct region){ i, 0 };
		pthread_create(&t[i], NULL, region_writer, &rg[i]);
	}
	for (int i = 0; i < N; i++)
		pthread_join(t[i], NULL);
	for (int i = 0; i < N; i++) {
		KT_EQ(rg[i].err, 0);
		KT_EQ(pread(fd, buf, BLK, (off_t)i * BLK), BLK);
		KT_ASSERT(all_bytes(buf, BLK, (unsigned char)('a' + i)));
	}
	KT_EQ(size_of(fd), (uint64_t)N * BLK);
	close(fd);
}

static const struct kt_case cases[] = {
	KT_CASE(info_reports_device),
	KT_CASE(write_seek_read_roundtrip),
	KT_CASE(sparse_write_reads_back_zeros),
	KT_CASE(truncate_never_leaks_old_data),
	KT_CASE(llseek_modes),
	KT_CASE(capacity_gives_short_write_then_enospc),
	KT_CASE(o_append_always_writes_at_end),
	KT_CASE(o_trunc_empties_device),
	KT_CASE(bad_user_pointer_is_efault),
	KT_CASE(fill_ioctl),
	KT_CASE(stats_count_operations),
	KT_CASE(info_accepts_older_and_newer_callers),
	KT_CASE(fill_accepts_v0_and_checks_unknown_fields),
	KT_CASE(malformed_commands_are_enotty),
	KT_CASE(devices_are_independent),
	KT_CASE(reads_never_observe_a_torn_write),
	KT_CASE(concurrent_writers_to_disjoint_regions),
};

int main(int argc, char **argv)
{
	char path[256];

	lklbuf_path(0, path, sizeof(path));
	if (access(path, R_OK | W_OK) != 0) {
		printf("1..0 # SKIP %s: %s\n", path, strerror(errno));
		return 77;
	}
	return kt_run(cases, KT_N(cases), argc, argv);
}

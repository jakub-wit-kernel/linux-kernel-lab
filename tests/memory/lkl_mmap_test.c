// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/lkl_mmap interface tests (module loaded, run as root). TAP output.
 * Exits 77 if the device is missing.
 */
#define _GNU_SOURCE
#define KT_IMPLEMENTATION
#include "kt.h"

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEV	"/dev/lkl_mmap"
#define FAULTS	"/sys/kernel/lkl_mem/mmap_faults"
#define REFCNT	"/sys/module/lkl_mem/refcnt"

static long pagesz;

static long read_long(const char *path)
{
	char buf[32] = "";
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		KT_FAIL("open %s: %s", path, strerror(errno));
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		KT_FAIL("read %s", path);
	return strtol(buf, NULL, 10);
}

/* Device size, from lseek(SEEK_END). */
static off_t dev_size(int fd)
{
	off_t end = lseek(fd, 0, SEEK_END);

	KT_ASSERT(end > 0);
	KT_EQ(lseek(fd, 0, SEEK_SET), 0);
	return end;
}

KT_TEST(size_is_whole_pages)
{
	int fd = open(DEV, O_RDWR);

	KT_OK(fd);
	KT_EQ(dev_size(fd) % pagesz, 0);
	close(fd);
}

KT_TEST(write_then_mmap_sees_it)
{
	int fd = open(DEV, O_RDWR);
	off_t size;
	char *m;

	KT_OK(fd);
	size = dev_size(fd);
	KT_EQ(pwrite(fd, "hello", 5, pagesz + 3), 5);
	m = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	KT_ASSERT(m != MAP_FAILED);
	KT_EQ(memcmp(m + pagesz + 3, "hello", 5), 0);
	munmap(m, size);
	close(fd);
}

KT_TEST(mmap_store_then_read_sees_it)
{
	int fd = open(DEV, O_RDWR);
	off_t size;
	char *m, buf[8] = "";

	KT_OK(fd);
	size = dev_size(fd);
	m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	KT_ASSERT(m != MAP_FAILED);
	memcpy(m + size - 6, "world", 6);
	KT_EQ(pread(fd, buf, 6, size - 6), 6);
	KT_EQ(strcmp(buf, "world"), 0);
	munmap(m, size);
	close(fd);
}

KT_TEST(two_mappings_share_pages)
{
	int fd = open(DEV, O_RDWR);
	volatile char *a, *b;

	KT_OK(fd);
	a = mmap(NULL, pagesz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	b = mmap(NULL, pagesz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	KT_ASSERT(a != MAP_FAILED && b != MAP_FAILED && a != b);
	a[100] = 'x';
	KT_EQ(b[100], 'x');
	b[100] = 'y';
	KT_EQ(a[100], 'y');
	munmap((void *)a, pagesz);
	munmap((void *)b, pagesz);
	close(fd);
}

KT_TEST(file_offset_maps_the_right_page)
{
	int fd = open(DEV, O_RDWR);
	char *m;

	KT_OK(fd);
	KT_EQ(pwrite(fd, "P1", 2, pagesz), 2);
	m = mmap(NULL, pagesz, PROT_READ, MAP_SHARED, fd, pagesz);
	KT_ASSERT(m != MAP_FAILED);
	KT_EQ(memcmp(m, "P1", 2), 0);
	munmap(m, pagesz);
	close(fd);
}

KT_TEST(faults_once_per_touched_page)
{
	int fd = open(DEV, O_RDWR);
	off_t size;
	long before, after_first, after_second, pages, i;
	volatile char *m;
	char sink = 0;

	KT_OK(fd);
	size = dev_size(fd);
	pages = size / pagesz;
	m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	KT_ASSERT(m != MAP_FAILED);
	before = read_long(FAULTS);
	for (i = 0; i < pages; i++)
		sink += m[i * pagesz];
	after_first = read_long(FAULTS);
	for (i = 0; i < pages; i++)
		sink += m[i * pagesz + 1];
	after_second = read_long(FAULTS);
	(void)sink;
	printf("  # %ld pages: %ld faults on first touch, %ld on second\n",
	       pages, after_first - before, after_second - after_first);
	KT_EQ(after_first - before, pages);
	KT_EQ(after_second - after_first, 0);
	munmap((void *)m, size);
	close(fd);
}

KT_TEST(private_mapping_rejected)
{
	int fd = open(DEV, O_RDWR);

	KT_OK(fd);
	KT_ASSERT(mmap(NULL, pagesz, PROT_READ, MAP_PRIVATE, fd, 0) ==
		  MAP_FAILED);
	KT_EQ(errno, EINVAL);
	close(fd);
}

KT_TEST(mapping_beyond_the_end_rejected)
{
	int fd = open(DEV, O_RDWR);
	off_t size;

	KT_OK(fd);
	size = dev_size(fd);
	KT_ASSERT(mmap(NULL, size + pagesz, PROT_READ, MAP_SHARED, fd, 0) ==
		  MAP_FAILED);
	KT_EQ(errno, EINVAL);
	KT_ASSERT(mmap(NULL, pagesz, PROT_READ, MAP_SHARED, fd, size) ==
		  MAP_FAILED);
	KT_EQ(errno, EINVAL);
	close(fd);
}

/* mremap() can grow a mapping past the buffer; the fault handler must
 * answer with SIGBUS, not map something else. */
KT_TEST(access_past_the_buffer_is_sigbus)
{
	int fd = open(DEV, O_RDWR), status;
	off_t size;
	pid_t pid;

	KT_OK(fd);
	size = dev_size(fd);
	pid = fork();
	KT_OK(pid);
	if (pid == 0) {
		char *m = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
		volatile char *g;

		if (m == MAP_FAILED)
			_exit(2);
		g = mremap(m, size, size + pagesz, MREMAP_MAYMOVE);
		if (g == MAP_FAILED)
			_exit(3);
		(void)g[size];		/* must raise SIGBUS */
		_exit(0);
	}
	KT_EQ(waitpid(pid, &status, 0), pid);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 3)
		KT_SKIP("mremap could not grow the mapping");
	KT_ASSERT(WIFSIGNALED(status));
	KT_EQ(WTERMSIG(status), SIGBUS);
	close(fd);
}

KT_TEST(read_write_bounds)
{
	int fd = open(DEV, O_RDWR);
	off_t size;
	char c;

	KT_OK(fd);
	size = dev_size(fd);
	KT_EQ(pread(fd, &c, 1, size), 0);		/* EOF */
	KT_ERRNO(pwrite(fd, "x", 1, size), ENOSPC);
	KT_EQ(pwrite(fd, "abcd", 4, size - 2), 2);	/* short write */
	KT_ERRNO(lseek(fd, size + 1, SEEK_SET), EINVAL);
	close(fd);
}

KT_TEST(mapping_pins_the_module_after_close)
{
	int fd = open(DEV, O_RDWR);
	long base, held;
	char *m;

	KT_OK(fd);
	m = mmap(NULL, pagesz, PROT_READ, MAP_SHARED, fd, 0);
	KT_ASSERT(m != MAP_FAILED);
	base = read_long(REFCNT);
	close(fd);
	held = read_long(REFCNT);
	/*
	 * The module reference belongs to the open file, not the fd: the
	 * mapping keeps the file, and so the module, alive after close().
	 */
	KT_ASSERT(held >= 1);
	KT_EQ(held, base);
	munmap(m, pagesz);
	KT_EQ(read_long(REFCNT), base - 1);
}

static const struct kt_case cases[] = {
	KT_CASE(size_is_whole_pages),
	KT_CASE(write_then_mmap_sees_it),
	KT_CASE(mmap_store_then_read_sees_it),
	KT_CASE(two_mappings_share_pages),
	KT_CASE(file_offset_maps_the_right_page),
	KT_CASE(faults_once_per_touched_page),
	KT_CASE(private_mapping_rejected),
	KT_CASE(mapping_beyond_the_end_rejected),
	KT_CASE(access_past_the_buffer_is_sigbus),
	KT_CASE(read_write_bounds),
	KT_CASE(mapping_pins_the_module_after_close),
};

int main(int argc, char **argv)
{
	pagesz = sysconf(_SC_PAGESIZE);
	if (access(DEV, R_OK | W_OK) != 0) {
		printf("1..0 # SKIP " DEV ": %s\n", strerror(errno));
		return 77;
	}
	return kt_run(cases, KT_N(cases), argc, argv);
}

// SPDX-License-Identifier: GPL-2.0
/*
 * lkl_events interface tests (module loaded, run as root). TAP output.
 * Exits 77 if /proc/lkl_lab is missing.
 */
#define _GNU_SOURCE
#define KT_IMPLEMENTATION
#include "kt.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

#include <linux/lkl_events.h>

#define PROC "/proc/lkl_lab"
#define SYS  "/sys/kernel/lkl_lab"

_Static_assert(sizeof(struct lkl_events_header) == 24, "header layout");
_Static_assert(sizeof(struct lkl_event_record) == 64, "record layout");

/* Write a string to a file; 0 or -errno. */
static int put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC), ret = 0;

	if (fd < 0)
		return -errno;
	if (write(fd, s, strlen(s)) < 0)
		ret = -errno;
	close(fd);
	return ret;
}

/* Read a whole file using reads of @chunk bytes. Caller frees. */
static char *slurp(const char *path, size_t chunk, size_t *len_out)
{
	size_t cap = 1 << 16, len = 0;
	char *buf = malloc(cap + 1);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0 || !buf)
		KT_FAIL("open %s: %s", path, strerror(errno));
	for (;;) {
		if (len + chunk > cap) {
			cap = (cap + chunk) * 2;
			buf = realloc(buf, cap + 1);
		}
		n = read(fd, buf + len, chunk);
		if (n < 0)
			KT_FAIL("read %s: %s", path, strerror(errno));
		if (n == 0)
			break;
		len += (size_t)n;
	}
	close(fd);
	buf[len] = '\0';
	if (len_out)
		*len_out = len;
	return buf;
}

static long sys_long(const char *attr)
{
	char path[128];
	char *s;
	long v;

	snprintf(path, sizeof(path), SYS "/%s", attr);
	s = slurp(path, 4096, NULL);
	v = strtol(s, NULL, 10);
	free(s);
	return v;
}

static void reset(unsigned int capacity)
{
	char v[16];

	snprintf(v, sizeof(v), "%u", capacity);
	KT_EQ(put(SYS "/threshold", "0"), 0);
	KT_EQ(put(PROC "/control", "clear"), 0);
	KT_EQ(put(SYS "/capacity", v), 0);
}

/* ---- snapshot ------------------------------------------------------------------ */

KT_TEST(snapshot_format_and_order)
{
	size_t len;
	char *raw;
	const struct lkl_events_header *h;
	const struct lkl_event_record *r;

	reset(8);
	KT_EQ(put(PROC "/control", "burst 11"), 0);	/* 3 get overwritten */
	raw = slurp(PROC "/snapshot", 1 << 20, &len);
	h = (const void *)raw;
	KT_EQ(h->magic, LKL_EVENTS_MAGIC);
	KT_EQ(h->version, LKL_EVENTS_VERSION);
	KT_EQ(h->record_size, sizeof(*r));
	KT_EQ(h->count, 8);
	KT_EQ(h->capacity, 8);
	KT_EQ(h->total, 11);
	KT_EQ(len, sizeof(*h) + 8 * sizeof(*r));
	r = (const void *)(h + 1);
	for (unsigned int i = 0; i < 8; i++) {
		char want[32];

		snprintf(want, sizeof(want), "burst-%u", i + 3);
		KT_EQ(r[i].seq, i + 3);			/* oldest first */
		KT_ASSERT(strcmp(r[i].text, want) == 0);
		if (i)
			KT_ASSERT(r[i].timestamp_ns >= r[i - 1].timestamp_ns);
	}
	free(raw);
}

KT_TEST(snapshot_is_frozen_at_open)
{
	struct lkl_events_header h;
	int fd;

	reset(64);
	KT_EQ(put(PROC "/control", "add before"), 0);
	fd = open(PROC "/snapshot", O_RDONLY | O_CLOEXEC);
	KT_ASSERT(fd >= 0);
	KT_EQ(put(PROC "/control", "add after"), 0);
	KT_EQ(read(fd, &h, sizeof(h)), (ssize_t)sizeof(h));
	KT_EQ(h.count, 1);			/* "after" is not in it */
	close(fd);
	KT_EQ(sys_long("count"), 2);
}

KT_TEST(snapshot_odd_sized_reads_match_one_read)
{
	size_t a, b;
	char *one, *odd;

	reset(64);
	KT_EQ(put(PROC "/control", "burst 50"), 0);
	one = slurp(PROC "/snapshot", 1 << 20, &a);
	odd = slurp(PROC "/snapshot", 13, &b);
	KT_EQ(a, b);
	KT_ASSERT(memcmp(one, odd, a) == 0);
	free(one);
	free(odd);
}

/* ---- seq_file listing ---------------------------------------------------------- */

KT_TEST(events_tiny_reads_match_one_read)
{
	size_t a, b;
	char *one, *tiny;
	unsigned long expect = 0;

	reset(4096);
	KT_EQ(put(PROC "/control", "burst 3000"), 0);	/* ~100 KiB of text */
	one = slurp(PROC "/events", 1 << 20, &a);
	tiny = slurp(PROC "/events", 7, &b);
	KT_ASSERT(a > 64 * 1024);		/* several seq_file buffers' worth */
	KT_EQ(a, b);
	KT_ASSERT(memcmp(one, tiny, a) == 0);
	/* Every line present once, in order. */
	for (char *line = one; *line; line = strchr(line, '\n') + 1) {
		KT_EQ(strtoul(line, NULL, 10), expect);
		expect++;
	}
	KT_EQ(expect, 3000);
	free(one);
	free(tiny);
}

struct adder {
	int stop;
	int err;
};

static void *keep_adding(void *arg)
{
	struct adder *a = arg;

	/* Bounded: a 5-byte reader must eventually catch up and finish. */
	for (int i = 0; i < 300 && !__atomic_load_n(&a->stop, __ATOMIC_RELAXED); i++)
		if (put(PROC "/control", "burst 20"))
			a->err = 1;
	return NULL;
}

/*
 * Read the listing in small pieces while another thread keeps adding.
 * Positions are sequence numbers, so the output must be strictly
 * increasing: no duplicates, no reordering (gaps are allowed, because
 * overwritten events are skipped).
 */
KT_TEST(events_paged_read_never_duplicates_under_concurrent_adds)
{
	struct adder a = { 0, 0 };
	pthread_t t;
	char *out;
	long prev = -1, lines = 0;

	reset(256);
	KT_EQ(put(PROC "/control", "burst 256"), 0);
	pthread_create(&t, NULL, keep_adding, &a);
	out = slurp(PROC "/events", 5, NULL);
	__atomic_store_n(&a.stop, 1, __ATOMIC_RELAXED);
	pthread_join(t, NULL);
	KT_EQ(a.err, 0);
	for (char *line = out; *line; line = strchr(line, '\n') + 1) {
		long seq = strtol(line, NULL, 10);

		if (seq <= prev)
			KT_FAIL("seq %ld after %ld: duplicate or reordering", seq, prev);
		prev = seq;
		lines++;
	}
	printf("  # %ld lines read while events were being added\n", lines);
	KT_ASSERT(lines >= 1);
	free(out);
}

/* ---- pollable sysfs attribute -------------------------------------------------- */

struct waiter {
	int fd;
	int revents;
	double waited_ms;
};

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void *wait_alert(void *arg)
{
	struct waiter *w = arg;
	struct pollfd p = { .fd = w->fd, .events = POLLPRI | POLLERR };
	double t0 = now_ms();

	if (poll(&p, 1, 5000) > 0)
		w->revents = p.revents;
	w->waited_ms = now_ms() - t0;
	return NULL;
}

/* Open + read once to arm sysfs notification, then poll in a thread. */
static void start_waiter(struct waiter *w, pthread_t *t)
{
	char buf[8];

	w->fd = open(SYS "/alert", O_RDONLY | O_CLOEXEC);
	KT_ASSERT(w->fd >= 0);
	KT_ASSERT(read(w->fd, buf, sizeof(buf)) > 0);
	w->revents = 0;
	pthread_create(t, NULL, wait_alert, w);
	usleep(100 * 1000);	/* let it block in poll() */
}

KT_TEST(alert_poll_wakes_on_raise_and_on_clear)
{
	struct waiter w;
	pthread_t t;

	reset(64);
	KT_EQ(put(SYS "/threshold", "5"), 0);
	KT_EQ(sys_long("alert"), 0);

	start_waiter(&w, &t);
	KT_EQ(put(PROC "/control", "burst 4"), 0);	/* below threshold */
	usleep(100 * 1000);
	KT_EQ(w.revents, 0);
	KT_EQ(put(PROC "/control", "add fifth"), 0);	/* reaches it */
	pthread_join(t, NULL);
	KT_ASSERT(w.revents & (POLLPRI | POLLERR));
	KT_ASSERT(w.waited_ms < 4000);
	KT_EQ(sys_long("alert"), 1);
	close(w.fd);

	start_waiter(&w, &t);
	KT_EQ(put(PROC "/control", "clear"), 0);	/* falls back below */
	pthread_join(t, NULL);
	KT_ASSERT(w.revents & (POLLPRI | POLLERR));
	KT_EQ(sys_long("alert"), 0);
	close(w.fd);
}

KT_TEST(alert_follows_threshold_and_capacity_changes)
{
	reset(64);
	KT_EQ(put(PROC "/control", "burst 10"), 0);
	KT_EQ(put(SYS "/threshold", "10"), 0);
	KT_EQ(sys_long("alert"), 1);		/* raised by the threshold write */
	KT_EQ(put(SYS "/capacity", "5"), 0);	/* keeps newest 5 */
	KT_EQ(sys_long("count"), 5);
	KT_EQ(sys_long("alert"), 0);		/* dropped by the resize */
	KT_EQ(put(SYS "/threshold", "0"), 0);
}

/* ---- control errors -------------------------------------------------------------- */

KT_TEST(control_rejects_bad_commands)
{
	char big[200];

	KT_EQ(put(PROC "/control", "explode"), -EINVAL);
	KT_EQ(put(PROC "/control", "add"), -EINVAL);		/* no text */
	KT_EQ(put(PROC "/control", "burst many"), -EINVAL);
	KT_EQ(put(PROC "/control", "burst 999999999"), -EINVAL);
	KT_EQ(put(PROC "/control", "clear now"), -EINVAL);
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	KT_EQ(put(PROC "/control", big), -EINVAL);		/* > 128 bytes */
}

KT_TEST(text_is_sanitized_and_truncated)
{
	char *s;

	reset(4);
	KT_EQ(put(PROC "/control", "add tab\there"), 0);
	KT_EQ(put(PROC "/control",
		  "add 0123456789012345678901234567890123456789012345678901234567890"), 0);
	s = slurp(PROC "/events", 4096, NULL);
	KT_ASSERT(strstr(s, " tab?here\n"));
	/* 47 characters kept (48-byte field incl. NUL). */
	KT_ASSERT(strstr(s, " 01234567890123456789012345678901234567890123456\n"));
	free(s);
}

static const struct kt_case cases[] = {
	KT_CASE(snapshot_format_and_order),
	KT_CASE(snapshot_is_frozen_at_open),
	KT_CASE(snapshot_odd_sized_reads_match_one_read),
	KT_CASE(events_tiny_reads_match_one_read),
	KT_CASE(events_paged_read_never_duplicates_under_concurrent_adds),
	KT_CASE(alert_poll_wakes_on_raise_and_on_clear),
	KT_CASE(alert_follows_threshold_and_capacity_changes),
	KT_CASE(control_rejects_bad_commands),
	KT_CASE(text_is_sanitized_and_truncated),
};

int main(int argc, char **argv)
{
	if (access(PROC "/control", W_OK) != 0) {
		printf("1..0 # SKIP " PROC ": %s\n", strerror(errno));
		return 77;
	}
	return kt_run(cases, KT_N(cases), argc, argv);
}

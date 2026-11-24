// SPDX-License-Identifier: GPL-2.0
/*
 * lklbuf - command-line client for /dev/lkl_bufN.
 *
 *   lklbuf [-d N] info
 *   lklbuf [-d N] stats [--reset]
 *   lklbuf [-d N] write OFFSET DATA       (DATA "-" reads stdin)
 *   lklbuf [-d N] read OFFSET LENGTH      (raw bytes to stdout)
 *   lklbuf [-d N] fill OFFSET LENGTH BYTE (BYTE: 0..255 or 0x..)
 *   lklbuf [-d N] truncate SIZE
 *   lklbuf [-d N] clear
 *
 * Sizes and offsets accept K/M/G suffixes.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lklbuf.h"

static const char *prog = "lklbuf";

static void die(const char *what, int err)
{
	fprintf(stderr, "%s: %s: %s\n", prog, what, strerror(err < 0 ? -err : err));
	exit(1);
}

static uint64_t size_arg(const char *name, const char *s)
{
	uint64_t v;

	if (lklbuf_parse_size(s, &v)) {
		fprintf(stderr, "%s: bad %s '%s'\n", prog, name, s);
		exit(2);
	}
	return v;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: %s [-d N] info | stats [--reset] | write OFF DATA|- |\n"
		"       read OFF LEN | fill OFF LEN BYTE | truncate SIZE | clear\n",
		prog);
	exit(2);
}

static void write_all(int fd, const void *buf, size_t len, uint64_t off)
{
	const char *p = buf;

	while (len) {
		ssize_t n = pwrite(fd, p, len, (off_t)off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("write", errno);
		}
		p += n;
		off += (uint64_t)n;
		len -= (size_t)n;
	}
}

static int cmd_write(int fd, uint64_t off, const char *data)
{
	if (strcmp(data, "-") != 0) {
		write_all(fd, data, strlen(data), off);
		return 0;
	}
	char buf[65536];
	ssize_t n;

	while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
		write_all(fd, buf, (size_t)n, off);
		off += (uint64_t)n;
	}
	if (n < 0)
		die("stdin", errno);
	return 0;
}

static int cmd_read(int fd, uint64_t off, uint64_t len)
{
	char buf[65536];

	while (len) {
		size_t want = len < sizeof(buf) ? (size_t)len : sizeof(buf);
		ssize_t n = pread(fd, buf, want, (off_t)off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("read", errno);
		}
		if (n == 0)
			break;		/* end of data */
		if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
			die("stdout", errno);
		off += (uint64_t)n;
		len -= (uint64_t)n;
	}
	return 0;
}

int main(int argc, char **argv)
{
	unsigned int dev = 0;
	int fd, r;

	if (argc > 2 && strcmp(argv[1], "-d") == 0) {
		dev = (unsigned int)size_arg("device", argv[2]);
		argv += 2;
		argc -= 2;
	}
	if (argc < 2)
		usage();
	const char *cmd = argv[1];

	fd = lklbuf_open(dev, strcmp(cmd, "info") == 0 || strcmp(cmd, "read") == 0 ||
				     strcmp(cmd, "stats") == 0 ? O_RDONLY : O_RDWR);
	if (fd < 0)
		die("open", fd);

	if (strcmp(cmd, "info") == 0 && argc == 2) {
		struct lklbuf_info i;

		if ((r = lklbuf_info(fd, &i)))
			die("info", r);
		printf("api_version %u\nminor %u\ncapacity %llu\nsize %llu\nopen_count %u\n",
		       i.api_version, i.minor, (unsigned long long)i.capacity,
		       (unsigned long long)i.size, i.open_count);
	} else if (strcmp(cmd, "stats") == 0 && (argc == 2 || argc == 3)) {
		struct lklbuf_stats s;

		if ((r = lklbuf_stats(fd, &s)))
			die("stats", r);
		printf("reads %llu\nwrites %llu\nbytes_read %llu\nbytes_written %llu\n"
		       "seeks %llu\nioctls %llu\n",
		       (unsigned long long)s.reads, (unsigned long long)s.writes,
		       (unsigned long long)s.bytes_read,
		       (unsigned long long)s.bytes_written,
		       (unsigned long long)s.seeks, (unsigned long long)s.ioctls);
		if (argc == 3) {
			if (strcmp(argv[2], "--reset") != 0)
				usage();
			if ((r = lklbuf_reset_stats(fd)))
				die("reset", r);
		}
	} else if (strcmp(cmd, "write") == 0 && argc == 4) {
		cmd_write(fd, size_arg("offset", argv[2]), argv[3]);
	} else if (strcmp(cmd, "read") == 0 && argc == 4) {
		cmd_read(fd, size_arg("offset", argv[2]), size_arg("length", argv[3]));
	} else if (strcmp(cmd, "fill") == 0 && argc == 5) {
		char *end;
		unsigned long b = strtoul(argv[4], &end, 0);

		if (*end || end == argv[4] || b > 255) {
			fprintf(stderr, "%s: bad byte '%s'\n", prog, argv[4]);
			return 2;
		}
		if ((r = lklbuf_fill(fd, size_arg("offset", argv[2]),
				     size_arg("length", argv[3]), (uint8_t)b)))
			die("fill", r);
	} else if (strcmp(cmd, "truncate") == 0 && argc == 3) {
		if ((r = lklbuf_truncate(fd, size_arg("size", argv[2]))))
			die("truncate", r);
	} else if (strcmp(cmd, "clear") == 0 && argc == 2) {
		if ((r = lklbuf_clear(fd)))
			die("clear", r);
	} else {
		usage();
	}
	close(fd);
	return 0;
}

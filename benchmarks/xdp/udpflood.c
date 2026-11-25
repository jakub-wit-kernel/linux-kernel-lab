// SPDX-License-Identifier: GPL-2.0
/*
 * udpflood - send COUNT UDP datagrams as fast as one thread can, with
 * sendmmsg() in batches, and report the rate.
 *
 *   udpflood ADDR PORT COUNT [SIZE [BATCH]]     (SIZE 64, BATCH 64)
 *
 * Prints "sent=N seconds=S pps=P". The datagrams all carry the same
 * payload; only their number and timing matter here.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_BATCH	1024
#define MAX_SIZE	1472	/* fits a 1500-byte MTU */

static bool parse_ul(const char *s, unsigned long min, unsigned long max,
		     unsigned long *out)
{
	char *end;

	if (!*s || *s == '-')
		return false;
	errno = 0;
	*out = strtoul(s, &end, 10);
	return !errno && !*end && *out >= min && *out <= max;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	static struct mmsghdr msgs[MAX_BATCH];
	static struct iovec iov[MAX_BATCH];
	static char payload[MAX_SIZE];
	unsigned long port, count, size = 64, batch = 64, sent = 0;
	struct sockaddr_in dst = { .sin_family = AF_INET };
	double t0, secs;
	unsigned long i;
	int fd;

	if (argc < 4 || argc > 6 ||
	    inet_pton(AF_INET, argv[1], &dst.sin_addr) != 1 ||
	    !parse_ul(argv[2], 1, 65535, &port) ||
	    !parse_ul(argv[3], 1, 1UL << 40, &count) ||
	    (argc > 4 && !parse_ul(argv[4], 1, MAX_SIZE, &size)) ||
	    (argc > 5 && !parse_ul(argv[5], 1, MAX_BATCH, &batch))) {
		fprintf(stderr, "usage: udpflood ADDR PORT COUNT [SIZE(1-%d) [BATCH(1-%d)]]\n",
			MAX_SIZE, MAX_BATCH);
		return 2;
	}
	dst.sin_port = htons((unsigned short)port);
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&dst, sizeof(dst))) {
		perror("udpflood: socket");
		return 1;
	}
	for (i = 0; i < batch; i++) {
		iov[i].iov_base = payload;
		iov[i].iov_len = size;
		msgs[i].msg_hdr.msg_iov = &iov[i];
		msgs[i].msg_hdr.msg_iovlen = 1;
	}

	t0 = now();
	while (sent < count) {
		unsigned int n = count - sent < batch ? count - sent : batch;
		int r = sendmmsg(fd, msgs, n, 0);

		if (r < 0) {
			/* No listener: ICMP port unreachable comes back. */
			if (errno == ECONNREFUSED || errno == ENOBUFS ||
			    errno == EINTR)
				continue;
			perror("udpflood: sendmmsg");
			return 1;
		}
		sent += r;
	}
	secs = now() - t0;
	printf("sent=%lu seconds=%.3f pps=%.0f\n", sent, secs, sent / secs);
	close(fd);
	return 0;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * lklbpf - load, attach, inspect and detach the lab's eBPF programs.
 *
 * Every attach pins the program's link and maps under a BPF filesystem
 * directory (<pin>/<feature>/), so the program stays attached after this
 * process exits and later invocations can read or change its maps.
 * Removing the pinned link detaches the program.
 *
 *   lklbpf [--pin DIR] [--obj-dir DIR] COMMAND
 *
 *   probe
 *   xdp attach DEV [--mode auto|native|generic]
 *   xdp deny-port PORT | allow-port PORT | deny-src CIDR | allow-src CIDR
 *   xdp stats | xdp detach
 *   tc attach DEV --port PORT --dscp DSCP      (tcx egress, Linux 6.6+)
 *   tc stats | tc detach
 *   napi attach | napi stats DEV | napi detach  (needs kernel BTF)
 *
 * Attach in the network namespace of the device (ip netns exec). Stats,
 * map updates and detach only use the pins and work from anywhere.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "lklbpf.h"

#define BPF_FS_MAGIC	0xcafe4a11

static const char *pin_root = "/sys/fs/bpf/lkl";
static char obj_dir[PATH_MAX];

__attribute__((format(printf, 1, 2)))
static int fail(const char *fmt, ...)
{
	va_list ap;

	fputs("lklbpf: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	return 1;
}

static int usage(void)
{
	fputs("usage: lklbpf [--pin DIR] [--obj-dir DIR] COMMAND\n"
	      "  probe\n"
	      "  xdp attach DEV [--mode auto|native|generic]\n"
	      "  xdp deny-port PORT | allow-port PORT | deny-src CIDR | allow-src CIDR\n"
	      "  xdp stats | xdp detach\n"
	      "  tc attach DEV --port PORT --dscp DSCP\n"
	      "  tc stats | tc detach\n"
	      "  napi attach | napi stats DEV | napi detach\n", stderr);
	return 2;
}

/* Strict unsigned parse: the whole string, within [min, max]. */
static bool parse_uint(const char *s, unsigned long min, unsigned long max,
		       unsigned long *out)
{
	char *end;

	if (!*s || *s == '-' || *s == '+')
		return false;
	errno = 0;
	*out = strtoul(s, &end, 10);
	return !errno && !*end && *out >= min && *out <= max;
}

static void pin_path(char *buf, size_t len, const char *feature,
		     const char *name)
{
	snprintf(buf, len, "%s/%s%s%s", pin_root, feature, name ? "/" : "",
		 name ? name : "");
}

static int open_pinned(const char *feature, const char *name)
{
	char p[PATH_MAX];
	int fd;

	pin_path(p, sizeof(p), feature, name);
	fd = bpf_obj_get(p);
	if (fd < 0)
		fail("%s: %s (is the program attached?)", p, strerror(errno));
	return fd;
}

/* Create <pin>/<feature>/ on a BPF filesystem; refuse if it is in use. */
static int make_pin_dir(const char *feature)
{
	char p[PATH_MAX];
	struct statfs st;

	if (mkdir(pin_root, 0700) && errno != EEXIST)
		return fail("mkdir %s: %s", pin_root, strerror(errno));
	if (statfs(pin_root, &st) || st.f_type != BPF_FS_MAGIC)
		return fail("%s is not on a BPF filesystem (mount -t bpf bpf DIR)",
			    pin_root);
	pin_path(p, sizeof(p), feature, NULL);
	if (mkdir(p, 0700)) {
		if (errno == EEXIST)
			return fail("%s exists: already attached? (%s detach)",
				    p, feature);
		return fail("mkdir %s: %s", p, strerror(errno));
	}
	return 0;
}

/* Remove every pin under <pin>/<feature>/ and the directory itself. */
static void remove_pin_dir(const char *feature)
{
	char dir[PATH_MAX], p[PATH_MAX + NAME_MAX + 2];
	struct dirent *de;
	DIR *d;

	pin_path(dir, sizeof(dir), feature, NULL);
	d = opendir(dir);
	if (!d)
		return;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
		unlink(p);
	}
	closedir(d);
	rmdir(dir);
}

static struct bpf_object *load_obj(const char *name)
{
	char path[PATH_MAX + 32];
	struct bpf_object *obj;

	if (snprintf(path, sizeof(path), "%s/%s.bpf.o", obj_dir, name) >=
	    (int)sizeof(path)) {
		fail("object path too long");
		return NULL;
	}
	obj = bpf_object__open_file(path, NULL);
	if (!obj) {
		fail("open %s: %s", path, strerror(errno));
		return NULL;
	}
	if (bpf_object__load(obj)) {
		/* libbpf has already printed the verifier log. */
		fail("load %s: %s", path, strerror(errno));
		bpf_object__close(obj);
		return NULL;
	}
	return obj;
}

static struct bpf_program *find_prog(struct bpf_object *obj, const char *name)
{
	struct bpf_program *prog = bpf_object__find_program_by_name(obj, name);

	if (!prog)
		fail("program %s not found in the object", name);
	return prog;
}

/*
 * Pin the object's maps and @link_fd under <pin>/<feature>/. On failure
 * everything is unpinned again; closing the link fd then detaches.
 *
 * Each map is pinned by its own path: bpf_object__pin_maps() replaces
 * every '.' in the whole path with '_' (bpffs names may not contain
 * dots), so a pin directory such as /tmp/tmp.XXXX/... would not be found.
 */
static int pin_all(struct bpf_object *obj, const char *feature, int link_fd)
{
	char dir[PATH_MAX], p[PATH_MAX];
	struct bpf_map *map;
	int err = 0;

	pin_path(dir, sizeof(dir), feature, NULL);
	bpf_object__for_each_map(map, obj) {
		pin_path(p, sizeof(p), feature, bpf_map__name(map));
		if (bpf_map__pin(map, p)) {
			err = errno;
			break;
		}
	}
	pin_path(p, sizeof(p), feature, "link");
	if (!err && bpf_obj_pin(link_fd, p))
		err = errno;
	if (err) {
		remove_pin_dir(feature);
		return fail("pin under %s: %s", dir, strerror(err));
	}
	return 0;
}

/*
 * Detach: take an fd on the pinned link, remove the pins, detach (where
 * the link type supports it) and close. Closing the last reference
 * releases the link, which detaches the program in every case.
 */
static int cmd_detach(const char *feature)
{
	int fd = open_pinned(feature, "link");

	if (fd < 0)
		return 1;
	remove_pin_dir(feature);
	if (bpf_link_detach(fd) && errno != EOPNOTSUPP && errno != EINVAL)
		fail("detach: %s", strerror(errno));
	close(fd);
	printf("%s detached\n", feature);
	return 0;
}

/* Sum a per-CPU value of @words u64s at @key into @out. */
static int percpu_sum(int fd, const void *key, int words, __u64 *out)
{
	int ncpu = libbpf_num_possible_cpus(), c, w;
	__u64 *vals;

	if (ncpu < 0)
		return fail("cannot count CPUs: %s", strerror(-ncpu));
	vals = calloc((size_t)ncpu * words, sizeof(*vals));
	if (!vals)
		return fail("out of memory");
	if (bpf_map_lookup_elem(fd, key, vals)) {
		free(vals);
		return fail("map lookup: %s", strerror(errno));
	}
	memset(out, 0, words * sizeof(*out));
	for (c = 0; c < ncpu; c++)
		for (w = 0; w < words; w++)
			out[w] += vals[c * words + w];
	free(vals);
	return 0;
}

/* ---- probe ------------------------------------------------------------------- */

static int cmd_probe(void)
{
	printf("xdp=%d sched_cls=%d tracepoint=%d btf=%d\n",
	       libbpf_probe_bpf_prog_type(BPF_PROG_TYPE_XDP, NULL) == 1,
	       libbpf_probe_bpf_prog_type(BPF_PROG_TYPE_SCHED_CLS, NULL) == 1,
	       libbpf_probe_bpf_prog_type(BPF_PROG_TYPE_TRACEPOINT, NULL) == 1,
	       access("/sys/kernel/btf/vmlinux", R_OK) == 0);
	return 0;
}

/* ---- xdp --------------------------------------------------------------------- */

static int xdp_attach(const char *dev, const char *mode)
{
	LIBBPF_OPTS(bpf_link_create_opts, opts);
	LIBBPF_OPTS(bpf_xdp_query_opts, q);
	unsigned int ifindex = if_nametoindex(dev);
	struct bpf_program *prog;
	struct bpf_object *obj;
	int link_fd, ret = 1;

	if (!ifindex)
		return fail("no device %s in this network namespace", dev);
	if (!strcmp(mode, "native"))
		opts.flags = XDP_FLAGS_DRV_MODE;
	else if (!strcmp(mode, "generic"))
		opts.flags = XDP_FLAGS_SKB_MODE;
	else if (strcmp(mode, "auto"))
		return usage();
	if (make_pin_dir("xdp"))
		return 1;
	obj = load_obj("xdp_filter");
	if (!obj)
		goto out_dir;
	prog = find_prog(obj, "xdp_filter");
	if (!prog)
		goto out_obj;
	link_fd = bpf_link_create(bpf_program__fd(prog), ifindex, BPF_XDP, &opts);
	if (link_fd < 0) {
		fail("attach XDP to %s (%s mode): %s", dev, mode, strerror(errno));
		goto out_obj;
	}
	if (pin_all(obj, "xdp", link_fd)) {
		close(link_fd);		/* not pinned: this detaches */
		goto out_obj;
	}
	close(link_fd);
	bpf_xdp_query(ifindex, 0, &q);
	printf("xdp_filter attached to %s, mode=%s\n", dev,
	       q.attach_mode == XDP_ATTACHED_DRV ? "native" :
	       q.attach_mode == XDP_ATTACHED_SKB ? "generic" : "other");
	ret = 0;
out_obj:
	bpf_object__close(obj);
out_dir:
	if (ret)
		remove_pin_dir("xdp");
	return ret;
}

static int xdp_port(const char *arg, bool deny)
{
	unsigned long v;
	__u64 zero = 0;
	__u16 port;
	int fd, err;

	if (!parse_uint(arg, 1, 65535, &v))
		return fail("bad port '%s'", arg);
	port = (__u16)v;
	fd = open_pinned("xdp", "deny_port");
	if (fd < 0)
		return 1;
	err = deny ? bpf_map_update_elem(fd, &port, &zero, BPF_NOEXIST) :
		     bpf_map_delete_elem(fd, &port);
	if (err && !(deny && errno == EEXIST)) {
		err = errno;
		close(fd);
		return fail("%s port %u: %s", deny ? "deny" : "allow", port,
			    strerror(err));
	}
	close(fd);
	return 0;
}

static int parse_cidr(const char *arg, struct lkl_lpm4 *key)
{
	char buf[32], *slash;
	unsigned long len = 32;
	struct in_addr a;

	snprintf(buf, sizeof(buf), "%s", arg);
	slash = strchr(buf, '/');
	if (slash) {
		*slash = '\0';
		if (!parse_uint(slash + 1, 0, 32, &len))
			return -1;
	}
	if (inet_pton(AF_INET, buf, &a) != 1)
		return -1;
	key->prefixlen = (__u32)len;
	key->addr = len ? a.s_addr & htonl(~0U << (32 - len)) : 0;
	return 0;
}

static int xdp_src(const char *arg, bool deny)
{
	struct lkl_lpm4 key;
	__u64 zero = 0;
	int fd, err;

	if (parse_cidr(arg, &key))
		return fail("bad prefix '%s' (want A.B.C.D[/LEN])", arg);
	fd = open_pinned("xdp", "deny_src");
	if (fd < 0)
		return 1;
	err = deny ? bpf_map_update_elem(fd, &key, &zero, BPF_NOEXIST) :
		     bpf_map_delete_elem(fd, &key);
	if (err && !(deny && errno == EEXIST)) {
		err = errno;
		close(fd);
		return fail("%s %s: %s", deny ? "deny" : "allow", arg,
			    strerror(err));
	}
	close(fd);
	return 0;
}

static int xdp_stats(void)
{
	static const char *const names[LKL_XDP_ACTIONS] = {
		"aborted", "drop", "pass", "tx", "redirect",
	};
	int sfd, pfd = -1, dfd = -1, ret = 1;
	struct lkl_lpm4 lk, *lprev = NULL;
	__u16 port, *pprev = NULL;
	__u64 c[2], hits;
	__u32 i;

	sfd = open_pinned("xdp", "xdp_stats");
	if (sfd < 0)
		return 1;
	pfd = open_pinned("xdp", "xdp_proto");
	dfd = open_pinned("xdp", "deny_src");
	if (pfd < 0 || dfd < 0)
		goto out;
	for (i = 0; i < LKL_XDP_ACTIONS; i++) {
		if (percpu_sum(sfd, &i, 2, c))
			goto out;
		printf("%s%s=%llu %s_bytes=%llu", i ? " " : "", names[i],
		       (unsigned long long)c[0], names[i],
		       (unsigned long long)c[1]);
	}
	for (i = 0; i < 256; i++) {
		if (percpu_sum(pfd, &i, 1, c))
			goto out;
		if (c[0])
			printf(" ipproto_%u=%llu", i, (unsigned long long)c[0]);
	}
	while (!bpf_map_get_next_key(dfd, lprev, &lk)) {
		char addr[INET_ADDRSTRLEN];

		if (!bpf_map_lookup_elem(dfd, &lk, &hits)) {
			inet_ntop(AF_INET, &lk.addr, addr, sizeof(addr));
			printf(" src_%s/%u=%llu", addr, lk.prefixlen,
			       (unsigned long long)hits);
		}
		lprev = &lk;
	}
	close(dfd);
	dfd = open_pinned("xdp", "deny_port");
	if (dfd < 0)
		goto out;
	while (!bpf_map_get_next_key(dfd, pprev, &port)) {
		if (!bpf_map_lookup_elem(dfd, &port, &hits))
			printf(" port_%u=%llu", port, (unsigned long long)hits);
		pprev = &port;
	}
	putchar('\n');
	ret = 0;
out:
	close(sfd);
	if (pfd >= 0)
		close(pfd);
	if (dfd >= 0)
		close(dfd);
	return ret;
}

static int cmd_xdp(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[0], "attach")) {
		const char *mode = "auto";

		if (argc == 4 && !strcmp(argv[2], "--mode"))
			mode = argv[3];
		else if (argc != 2)
			return usage();
		return xdp_attach(argv[1], mode);
	}
	if (argc == 2 && !strcmp(argv[0], "deny-port"))
		return xdp_port(argv[1], true);
	if (argc == 2 && !strcmp(argv[0], "allow-port"))
		return xdp_port(argv[1], false);
	if (argc == 2 && !strcmp(argv[0], "deny-src"))
		return xdp_src(argv[1], true);
	if (argc == 2 && !strcmp(argv[0], "allow-src"))
		return xdp_src(argv[1], false);
	if (argc == 1 && !strcmp(argv[0], "stats"))
		return xdp_stats();
	if (argc == 1 && !strcmp(argv[0], "detach"))
		return cmd_detach("xdp");
	return usage();
}

/* ---- tc ---------------------------------------------------------------------- */

static int tc_attach(const char *dev, unsigned long port, unsigned long dscp)
{
	struct lkl_tc_cfg cfg = { .port = (__u16)port, .dscp = (__u8)dscp };
	unsigned int ifindex = if_nametoindex(dev);
	struct bpf_program *prog;
	struct bpf_object *obj;
	struct bpf_link *link;
	struct bpf_map *map;
	__u32 zero = 0;
	int ret = 1;

	if (!ifindex)
		return fail("no device %s in this network namespace", dev);
	if (make_pin_dir("tc"))
		return 1;
	obj = load_obj("tc_dscp");
	if (!obj)
		goto out_dir;
	map = bpf_object__find_map_by_name(obj, "tc_cfg");
	prog = find_prog(obj, "tc_dscp");
	if (!map || !prog)
		goto out_obj;
	if (bpf_map_update_elem(bpf_map__fd(map), &zero, &cfg, BPF_ANY)) {
		fail("configure: %s", strerror(errno));
		goto out_obj;
	}
	link = bpf_program__attach_tcx(prog, ifindex, NULL);
	if (!link) {
		fail("attach tcx egress to %s: %s (tcx needs Linux 6.6+)", dev,
		     strerror(errno));
		goto out_obj;
	}
	if (pin_all(obj, "tc", bpf_link__fd(link))) {
		bpf_link__destroy(link);	/* not pinned: detaches */
		goto out_obj;
	}
	/* Pinned: let go of our handle without detaching. */
	bpf_link__disconnect(link);
	bpf_link__destroy(link);
	printf("tc_dscp attached to %s egress: UDP port %lu -> DSCP %lu\n", dev,
	       port, dscp);
	ret = 0;
out_obj:
	bpf_object__close(obj);
out_dir:
	if (ret)
		remove_pin_dir("tc");
	return ret;
}

static int tc_stats(void)
{
	static const char *const names[LKL_TC_STATS] = {
		"seen", "ipv4", "matched", "rewritten", "errors",
	};
	int fd = open_pinned("tc", "tc_stats");
	__u64 c;
	__u32 i;

	if (fd < 0)
		return 1;
	for (i = 0; i < LKL_TC_STATS; i++) {
		if (percpu_sum(fd, &i, 1, &c)) {
			close(fd);
			return 1;
		}
		printf("%s%s=%llu", i ? " " : "", names[i],
		       (unsigned long long)c);
	}
	putchar('\n');
	close(fd);
	return 0;
}

static int cmd_tc(int argc, char **argv)
{
	unsigned long port, dscp;

	if (argc == 6 && !strcmp(argv[0], "attach") &&
	    !strcmp(argv[2], "--port") && !strcmp(argv[4], "--dscp")) {
		if (!parse_uint(argv[3], 1, 65535, &port))
			return fail("bad port '%s'", argv[3]);
		if (!parse_uint(argv[5], 0, 63, &dscp))
			return fail("bad DSCP '%s' (0-63)", argv[5]);
		return tc_attach(argv[1], port, dscp);
	}
	if (argc == 1 && !strcmp(argv[0], "stats"))
		return tc_stats();
	if (argc == 1 && !strcmp(argv[0], "detach"))
		return cmd_detach("tc");
	return usage();
}

/* ---- napi -------------------------------------------------------------------- */

static int napi_attach(void)
{
	struct bpf_program *prog;
	struct bpf_object *obj;
	struct bpf_link *link;
	int ret = 1;

	if (access("/sys/kernel/btf/vmlinux", R_OK))
		return fail("no kernel BTF (/sys/kernel/btf/vmlinux): CO-RE needs CONFIG_DEBUG_INFO_BTF");
	if (make_pin_dir("napi"))
		return 1;
	obj = load_obj("napi_hist");	/* CO-RE relocations happen here */
	if (!obj)
		goto out_dir;
	prog = find_prog(obj, "napi_poll");
	if (!prog)
		goto out_obj;
	link = bpf_program__attach(prog);	/* tracepoint from SEC() */
	if (!link) {
		fail("attach napi:napi_poll: %s (is tracefs mounted?)",
		     strerror(errno));
		goto out_obj;
	}
	if (pin_all(obj, "napi", bpf_link__fd(link))) {
		bpf_link__destroy(link);
		goto out_obj;
	}
	bpf_link__disconnect(link);
	bpf_link__destroy(link);
	printf("napi_hist attached to napi:napi_poll\n");
	ret = 0;
out_obj:
	bpf_object__close(obj);
out_dir:
	if (ret)
		remove_pin_dir("napi");
	return ret;
}

struct bucket {
	__u32 work;
	__u64 polls;
};

static int cmp_bucket(const void *a, const void *b)
{
	const struct bucket *x = a, *y = b;

	return (x->work > y->work) - (x->work < y->work);
}

static int napi_stats(const char *dev)
{
	struct lkl_napi_key hk, *hprev = NULL;
	struct lkl_napi_total total = { 0 };
	struct bucket buckets[512];
	struct lkl_dev_key dk;
	int tfd, hfd, n = 0, i;
	__u64 polls;

	if (strlen(dev) >= LKL_DEV_NAME_LEN)
		return fail("device name too long: %s", dev);
	memset(&dk, 0, sizeof(dk));
	strcpy(dk.dev, dev);
	tfd = open_pinned("napi", "napi_totals");
	if (tfd < 0)
		return 1;
	hfd = open_pinned("napi", "napi_hist");
	if (hfd < 0) {
		close(tfd);
		return 1;
	}
	bpf_map_lookup_elem(tfd, &dk, &total);	/* absent: no polls yet */
	while (!bpf_map_get_next_key(hfd, hprev, &hk) &&
	       n < (int)(sizeof(buckets) / sizeof(buckets[0]))) {
		if (!memcmp(hk.dev, dk.dev, sizeof(hk.dev)) &&
		    !bpf_map_lookup_elem(hfd, &hk, &polls)) {
			buckets[n].work = hk.work;
			buckets[n].polls = polls;
			n++;
		}
		hprev = &hk;
	}
	qsort(buckets, n, sizeof(buckets[0]), cmp_bucket);
	printf("polls=%llu work=%llu hist=", (unsigned long long)total.polls,
	       (unsigned long long)total.work);
	for (i = 0; i < n; i++)
		printf("%s%u:%llu", i ? "," : "", buckets[i].work,
		       (unsigned long long)buckets[i].polls);
	putchar('\n');
	close(tfd);
	close(hfd);
	return 0;
}

static int cmd_napi(int argc, char **argv)
{
	if (argc == 1 && !strcmp(argv[0], "attach"))
		return napi_attach();
	if (argc == 2 && !strcmp(argv[0], "stats"))
		return napi_stats(argv[1]);
	if (argc == 1 && !strcmp(argv[0], "detach"))
		return cmd_detach("napi");
	return usage();
}

/* ---- main -------------------------------------------------------------------- */

/* Default object directory: "ebpf" next to this executable. */
static void default_obj_dir(void)
{
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	char *slash;

	if (n <= 0) {
		snprintf(obj_dir, sizeof(obj_dir), "ebpf");
		return;
	}
	exe[n] = '\0';
	slash = strrchr(exe, '/');
	if (slash)
		*slash = '\0';
	if (snprintf(obj_dir, sizeof(obj_dir), "%s/ebpf", exe) >=
	    (int)sizeof(obj_dir))
		snprintf(obj_dir, sizeof(obj_dir), "ebpf");
}

int main(int argc, char **argv)
{
	int i = 1;

	default_obj_dir();
	if (getenv("LKLBPF_PIN"))
		pin_root = getenv("LKLBPF_PIN");
	for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
		if (!strcmp(argv[i], "--pin"))
			pin_root = argv[i + 1];
		else if (!strcmp(argv[i], "--obj-dir"))
			snprintf(obj_dir, sizeof(obj_dir), "%s", argv[i + 1]);
		else
			return usage();
	}
	if (i >= argc)
		return usage();
	if (!strcmp(argv[i], "probe") && argc == i + 1)
		return cmd_probe();
	if (!strcmp(argv[i], "xdp"))
		return cmd_xdp(argc - i - 1, argv + i + 1);
	if (!strcmp(argv[i], "tc"))
		return cmd_tc(argc - i - 1, argv + i + 1);
	if (!strcmp(argv[i], "napi"))
		return cmd_napi(argc - i - 1, argv + i + 1);
	return usage();
}

// SPDX-License-Identifier: GPL-2.0
/*
 * napi_hist - a tracepoint program on napi:napi_poll that records, per
 * network device, how many polls ran and how many packets each one
 * processed.
 *
 * Maps (pinned by lklbpf under <pin>/napi/):
 *   napi_hist    {dev, work} -> polls that processed `work` packets
 *   napi_totals  {dev} -> {polls, work}
 *
 * CO-RE: there is no vmlinux.h here. The structs below declare only the
 * fields the program reads, marked preserve_access_index. The compiler
 * records each access as a relocation, and libbpf rewrites the offsets
 * when loading, using the running kernel's BTF (/sys/kernel/btf/vmlinux).
 * So one object works on kernels whose napi_struct and net_device
 * layouts differ.
 */
#include <linux/bpf.h>
#include <linux/types.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>

#include "lklbpf.h"

struct net_device {
	char name[LKL_DEV_NAME_LEN];
} __attribute__((preserve_access_index));

struct napi_struct {
	struct net_device *dev;
} __attribute__((preserve_access_index));

/* The tracepoint's record, as the kernel's BTF names it. */
struct trace_event_raw_napi_poll {
	struct napi_struct *napi;
	int work;
	int budget;
} __attribute__((preserve_access_index));

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 4096);
	__type(key, struct lkl_napi_key);
	__type(value, __u64);
} napi_hist SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 256);
	__type(key, struct lkl_dev_key);
	__type(value, struct lkl_napi_total);
} napi_totals SEC(".maps");

/* Look up @key, inserting @init first if it is missing (race-free). */
static __always_inline void *lookup_or_init(void *map, const void *key,
					    const void *init)
{
	void *v = bpf_map_lookup_elem(map, key);

	if (v)
		return v;
	bpf_map_update_elem(map, key, init, BPF_NOEXIST);  /* may lose a race */
	return bpf_map_lookup_elem(map, key);
}

SEC("tracepoint/napi/napi_poll")
int napi_poll(struct trace_event_raw_napi_poll *ctx)
{
	struct lkl_napi_total zero_total = {}, *t;
	struct lkl_napi_key hk = {};
	struct lkl_dev_key dk = {};
	__u64 zero = 0, *n;
	int work = ctx->work;

	/* ctx->napi->dev->name, each step relocated for this kernel */
	BPF_CORE_READ_STR_INTO(&dk.dev, ctx, napi, dev, name);
	__builtin_memcpy(hk.dev, dk.dev, sizeof(hk.dev));
	hk.work = work < 0 ? 0 : work;

	n = lookup_or_init(&napi_hist, &hk, &zero);
	if (n)
		__sync_fetch_and_add(n, 1);
	t = lookup_or_init(&napi_totals, &dk, &zero_total);
	if (t) {
		__sync_fetch_and_add(&t->polls, 1);
		__sync_fetch_and_add(&t->work, hk.work);
	}
	return 0;
}

char LICENSE[] SEC("license") = "GPL";

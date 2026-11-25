// SPDX-License-Identifier: GPL-2.0
/*
 * xdp_filter - drop IPv4 packets by source prefix or UDP destination port,
 * as early as the driver allows (XDP), and count what it decides.
 *
 * Maps (pinned by lklbpf under <pin>/xdp/):
 *   xdp_stats   per-CPU {packets, bytes} per XDP action
 *   xdp_proto   per-CPU packet count per IPv4 protocol number
 *   deny_src    LPM trie of IPv4 prefixes -> packets dropped
 *   deny_port   hash of UDP destination ports -> datagrams dropped
 *
 * Every access to packet data is preceded by a bounds check against
 * data_end; the verifier rejects the program otherwise.
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#include "lklbpf.h"

#define IP_OFFSET_MASK	0x1fff	/* fragment offset bits of frag_off */

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, LKL_XDP_ACTIONS);
	__type(key, __u32);
	__type(value, struct lkl_counter);
} xdp_stats SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 256);
	__type(key, __u32);
	__type(value, __u64);
} xdp_proto SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_LPM_TRIE);
	__uint(max_entries, 1024);
	__type(key, struct lkl_lpm4);
	__type(value, __u64);
	__uint(map_flags, BPF_F_NO_PREALLOC);	/* required for LPM tries */
} deny_src SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, __u16);			/* host byte order */
	__type(value, __u64);
} deny_port SEC(".maps");

static __always_inline int verdict(struct xdp_md *ctx, __u32 action)
{
	struct lkl_counter *c = bpf_map_lookup_elem(&xdp_stats, &action);

	if (c) {
		c->packets++;		/* per-CPU: no atomics needed */
		c->bytes += ctx->data_end - ctx->data;
	}
	return action;
}

SEC("xdp")
int xdp_filter(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;
	struct ethhdr *eth = data;
	struct lkl_lpm4 key;
	struct udphdr *udp;
	struct iphdr *iph;
	__u64 *count;
	__u32 proto;
	__u16 port;

	/* Not IPv4, or too short to tell: the stack decides. */
	if ((void *)(eth + 1) > data_end || eth->h_proto != bpf_htons(ETH_P_IP))
		return verdict(ctx, XDP_PASS);
	iph = (void *)(eth + 1);
	if ((void *)(iph + 1) > data_end || iph->ihl < 5)
		return verdict(ctx, XDP_PASS);

	proto = iph->protocol;
	count = bpf_map_lookup_elem(&xdp_proto, &proto);
	if (count)
		(*count)++;

	/* Longest-prefix match on the source address. */
	key.prefixlen = 32;
	key.addr = iph->saddr;
	count = bpf_map_lookup_elem(&deny_src, &key);
	if (count) {
		__sync_fetch_and_add(count, 1);	/* shared: atomic */
		return verdict(ctx, XDP_DROP);
	}

	/* Only the first fragment carries the UDP header. */
	if (iph->protocol != IPPROTO_UDP ||
	    (iph->frag_off & bpf_htons(IP_OFFSET_MASK)))
		return verdict(ctx, XDP_PASS);
	udp = (void *)iph + iph->ihl * 4;
	if ((void *)(udp + 1) > data_end)
		return verdict(ctx, XDP_PASS);
	port = bpf_ntohs(udp->dest);
	count = bpf_map_lookup_elem(&deny_port, &port);
	if (count) {
		__sync_fetch_and_add(count, 1);
		return verdict(ctx, XDP_DROP);
	}
	return verdict(ctx, XDP_PASS);
}

char LICENSE[] SEC("license") = "GPL";

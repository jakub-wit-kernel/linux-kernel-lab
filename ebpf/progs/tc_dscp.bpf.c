// SPDX-License-Identifier: GPL-2.0
/*
 * tc_dscp - a tc (tcx) egress program that rewrites the DSCP of IPv4 UDP
 * datagrams to one port, keeping the ECN bits and fixing the IPv4 header
 * checksum incrementally.
 *
 * Maps (pinned by lklbpf under <pin>/tc/):
 *   tc_cfg      [0] = {port, dscp}; port 0 disables the rewrite
 *   tc_stats    per-CPU packet counts (LKL_TC_*)
 *
 * Unlike XDP, tc programs see the sk_buff and may change the packet with
 * helpers. A wrong checksum would make the receiver drop every rewritten
 * datagram, so the tests check delivery as well as the TOS byte.
 */
#include <stddef.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/pkt_cls.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#include "lklbpf.h"

#define IP_OFFSET_MASK	0x1fff
#define IP_TOS_OFF	(ETH_HLEN + offsetof(struct iphdr, tos))
#define IP_CSUM_OFF	(ETH_HLEN + offsetof(struct iphdr, check))

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct lkl_tc_cfg);
} tc_cfg SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, LKL_TC_STATS);
	__type(key, __u32);
	__type(value, __u64);
} tc_stats SEC(".maps");

static __always_inline void bump(__u32 idx)
{
	__u64 *c = bpf_map_lookup_elem(&tc_stats, &idx);

	if (c)
		(*c)++;
}

SEC("tcx/egress")
int tc_dscp(struct __sk_buff *skb)
{
	void *data_end = (void *)(long)skb->data_end;
	void *data = (void *)(long)skb->data;
	struct ethhdr *eth = data;
	struct lkl_tc_cfg *cfg;
	struct udphdr *udp;
	struct iphdr *iph;
	__u16 old_word, new_word;
	__u8 new_tos;
	__u32 zero = 0;

	bump(LKL_TC_SEEN);
	if ((void *)(eth + 1) > data_end || eth->h_proto != bpf_htons(ETH_P_IP))
		return TC_ACT_OK;
	iph = (void *)(eth + 1);
	if ((void *)(iph + 1) > data_end || iph->ihl < 5)
		return TC_ACT_OK;
	bump(LKL_TC_IPV4);
	if (iph->protocol != IPPROTO_UDP ||
	    (iph->frag_off & bpf_htons(IP_OFFSET_MASK)))
		return TC_ACT_OK;
	udp = (void *)iph + iph->ihl * 4;
	if ((void *)(udp + 1) > data_end)
		return TC_ACT_OK;
	cfg = bpf_map_lookup_elem(&tc_cfg, &zero);
	if (!cfg || !cfg->port || bpf_ntohs(udp->dest) != cfg->port)
		return TC_ACT_OK;
	bump(LKL_TC_MATCHED);

	new_tos = (cfg->dscp << 2) | (iph->tos & 0x03);	/* keep ECN */
	if (new_tos == iph->tos)
		return TC_ACT_OK;
	/*
	 * The checksum covers 16-bit words; TOS is the low byte (in memory
	 * order, the second byte) of the first one. Update the checksum for
	 * that word, then store the byte. Both helpers may move the packet,
	 * so no packet pointer is used after them.
	 */
	old_word = *(__u16 *)iph;
	new_word = old_word;
	((__u8 *)&new_word)[1] = new_tos;
	if (bpf_l3_csum_replace(skb, IP_CSUM_OFF, old_word, new_word,
				sizeof(__u16)) ||
	    bpf_skb_store_bytes(skb, IP_TOS_OFF, &new_tos, sizeof(new_tos), 0)) {
		bump(LKL_TC_ERRORS);
		return TC_ACT_OK;
	}
	bump(LKL_TC_REWRITTEN);
	return TC_ACT_OK;
}

char LICENSE[] SEC("license") = "GPL";

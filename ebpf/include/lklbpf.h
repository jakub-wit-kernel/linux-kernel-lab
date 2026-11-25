/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Map key and value layouts shared by the eBPF programs (ebpf/progs/) and
 * the user-space tool that reads and writes their maps (ebpf/user/).
 * Only fixed-size types: the same header is compiled for -target bpf and
 * for the host.
 */
#ifndef LKLBPF_H
#define LKLBPF_H

#include <linux/types.h>

#define LKL_DEV_NAME_LEN	16	/* IFNAMSIZ */

/* ---- xdp_filter ---------------------------------------------------------- */

/* xdp_stats: per-CPU array indexed by XDP action (XDP_ABORTED..XDP_REDIRECT). */
#define LKL_XDP_ACTIONS		5

struct lkl_counter {
	__u64 packets;
	__u64 bytes;
};

/* deny_src: LPM trie key. addr is in network byte order. */
struct lkl_lpm4 {
	__u32 prefixlen;
	__u32 addr;
};

/* ---- tc_dscp --------------------------------------------------------------- */

/* tc_cfg[0]: mark UDP to `port` with DSCP `dscp`. port 0: off. */
struct lkl_tc_cfg {
	__u16 port;
	__u8 dscp;
	__u8 pad;
};

/* tc_stats: per-CPU array of packet counts. */
enum {
	LKL_TC_SEEN,		/* every packet on the hook */
	LKL_TC_IPV4,		/* IPv4 packets */
	LKL_TC_MATCHED,		/* UDP to the configured port */
	LKL_TC_REWRITTEN,	/* TOS byte changed, checksum fixed */
	LKL_TC_ERRORS,		/* a rewrite helper failed */
	LKL_TC_STATS,
};

/* ---- napi_hist ------------------------------------------------------------- */

/* napi_hist: number of polls of `dev` that processed `work` packets. */
struct lkl_napi_key {
	char dev[LKL_DEV_NAME_LEN];
	__u32 work;
};

/* napi_totals: per device. */
struct lkl_dev_key {
	char dev[LKL_DEV_NAME_LEN];
};

struct lkl_napi_total {
	__u64 polls;
	__u64 work;
};

#endif /* LKLBPF_H */

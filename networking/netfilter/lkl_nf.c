// SPDX-License-Identifier: GPL-2.0
/*
 * lkl_nf - a per-network-namespace netfilter hook.
 *
 * Every network namespace (existing ones at load time, and every one
 * created later) gets its own IPv4 LOCAL_IN hook, its own counters and
 * its own rule, configured through its own /proc/net/lkl_nf:
 *
 *   cat /proc/net/lkl_nf            drop_udp_port=0 seen=12 dropped=0
 *   echo "drop_udp_port 9999" > /proc/net/lkl_nf   drop UDP to port 9999
 *   echo "drop_udp_port 0" > /proc/net/lkl_nf      rule off
 *   echo reset > /proc/net/lkl_nf                  zero the counters
 *
 * "seen" counts IPv4 packets delivered locally in that namespace;
 * "dropped" counts the ones the rule dropped. A rule in one namespace
 * never affects another.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/ip.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/proc_fs.h>
#include <linux/seq_file_net.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/udp.h>
#include <net/ip.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>

#define PROC_NAME	"lkl_nf"

struct lkl_nf_net {
	struct nf_hook_ops ops;		/* registered in this namespace only */
	atomic64_t seen;
	atomic64_t dropped;
	unsigned int drop_port;		/* 0: no rule */
};

static unsigned int lkl_nf_net_id __read_mostly;

static unsigned int lkl_nf_hook(void *priv, struct sk_buff *skb,
				const struct nf_hook_state *state)
{
	struct lkl_nf_net *n = priv;
	unsigned int port = READ_ONCE(n->drop_port);
	const struct udphdr *uh;
	struct udphdr _uh;

	atomic64_inc(&n->seen);
	/* LOCAL_IN: the IP header is validated and fragments reassembled. */
	if (!port || ip_hdr(skb)->protocol != IPPROTO_UDP)
		return NF_ACCEPT;
	/* The UDP header need not be in the linear part: copy if needed. */
	uh = skb_header_pointer(skb, skb_network_offset(skb) + ip_hdrlen(skb),
				sizeof(_uh), &_uh);
	if (!uh || ntohs(uh->dest) != port)
		return NF_ACCEPT;
	atomic64_inc(&n->dropped);
	return NF_DROP;
}

static int lkl_nf_show(struct seq_file *seq, void *v)
{
	struct lkl_nf_net *n = net_generic(seq_file_single_net(seq),
					   lkl_nf_net_id);

	seq_printf(seq, "drop_udp_port=%u seen=%lld dropped=%lld\n",
		   READ_ONCE(n->drop_port), atomic64_read(&n->seen),
		   atomic64_read(&n->dropped));
	return 0;
}

/* @buf is a NUL-terminated kernel copy of what was written. */
static int lkl_nf_write(struct file *file, char *buf, size_t size)
{
	struct net *net = seq_file_single_net(file->private_data);
	struct lkl_nf_net *n = net_generic(net, lkl_nf_net_id);
	size_t skip = str_has_prefix(buf, "drop_udp_port ");
	unsigned int port;

	if (sysfs_streq(buf, "reset")) {
		atomic64_set(&n->seen, 0);
		atomic64_set(&n->dropped, 0);
		return 0;
	}
	/* kstrtouint() allows one trailing newline, nothing else. */
	if (!skip || kstrtouint(buf + skip, 10, &port) || port > 65535)
		return -EINVAL;
	WRITE_ONCE(n->drop_port, port);
	return 0;
}

static int __net_init lkl_nf_net_init(struct net *net)
{
	struct lkl_nf_net *n = net_generic(net, lkl_nf_net_id);
	int ret;

	n->ops.hook = lkl_nf_hook;
	n->ops.priv = n;
	n->ops.pf = NFPROTO_IPV4;
	n->ops.hooknum = NF_INET_LOCAL_IN;
	n->ops.priority = NF_IP_PRI_FILTER;
	ret = nf_register_net_hook(net, &n->ops);
	if (ret)
		return ret;
	if (!proc_create_net_single_write(PROC_NAME, 0644, net->proc_net,
					  lkl_nf_show, lkl_nf_write, NULL)) {
		nf_unregister_net_hook(net, &n->ops);
		return -ENOMEM;
	}
	return 0;
}

static void __net_exit lkl_nf_net_exit(struct net *net)
{
	struct lkl_nf_net *n = net_generic(net, lkl_nf_net_id);

	remove_proc_entry(PROC_NAME, net->proc_net);
	/* Waits for packets still inside the hook (synchronize_net()). */
	nf_unregister_net_hook(net, &n->ops);
}

static struct pernet_operations lkl_nf_net_ops = {
	.init = lkl_nf_net_init,
	.exit = lkl_nf_net_exit,
	.id = &lkl_nf_net_id,
	.size = sizeof(struct lkl_nf_net),	/* zeroed per namespace */
};

static int __init lkl_nf_init(void)
{
	int ret = register_pernet_subsys(&lkl_nf_net_ops);

	if (ret)
		return ret;
	pr_info("loaded: IPv4 LOCAL_IN hook in every network namespace\n");
	return 0;
}

static void __exit lkl_nf_exit(void)
{
	unregister_pernet_subsys(&lkl_nf_net_ops);
	pr_info("unloaded\n");
}

module_init(lkl_nf_init);
module_exit(lkl_nf_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jakub Witkowski");
MODULE_DESCRIPTION("Linux kernel lab: per-network-namespace netfilter hook");
MODULE_VERSION("1.0");

// SPDX-License-Identifier: GPL-2.0
/*
 * lkl_vnet - pairs of virtual Ethernet devices with a NAPI receive path.
 *
 * Loading the module creates `pairs` pairs: lklvnet0 <-> lklvnet1,
 * lklvnet2 <-> lklvnet3, ... A frame transmitted on one device is
 * received on its peer, which may live in another network namespace.
 *
 *   transmit (A)                         receive (B)
 *   ndo_start_xmit                       NAPI poll, softirq
 *     MTU check, scrub, eth_type_trans     dequeue up to `budget` frames
 *     enqueue on B's rx ring  ------->     napi_gro_receive()
 *     napi_schedule(B)                     wake A's queue once B's ring
 *     stop A's queue if B's ring is full   has drained to half
 *
 * The devices belong to the module: `ip link del` is refused, deleting a
 * namespace hands them back to the initial namespace, and rmmod removes
 * them wherever they are.
 *
 * Accounting, per direction, at any moment:
 *   A.tx_packets == B.rx_packets + B.rx_dropped + B.rx_queue_len
 * (rx_dropped: frames B refused for its MTU, or flushed from its ring when
 * it went down). Frames sent while the peer is down are A.tx_dropped.
 * ethtool -S shows the NAPI and flow-control counters.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>

#define MAX_PAIRS	4
#define VNET_MAX_MTU	9000

static unsigned int pairs = 1;
module_param(pairs, uint, 0444);
MODULE_PARM_DESC(pairs, "Number of device pairs (1-4, default 1)");

static unsigned int rx_ring = 256;
module_param(rx_ring, uint, 0444);
MODULE_PARM_DESC(rx_ring, "Receive ring length per device (16-4096, default 256)");

struct lkl_vnet {
	struct net_device *dev;
	struct net_device *peer;	/* fixed for the module's lifetime */
	struct napi_struct napi;
	struct sk_buff_head rxq;	/* frames sent by the peer, not yet polled */

	atomic64_t tx_packets, tx_bytes, tx_dropped;
	atomic64_t rx_packets, rx_bytes, rx_dropped;
	atomic64_t rx_dropped_mtu;
	atomic64_t napi_polls, napi_budget_exhausted;
	atomic64_t queue_stops, queue_wakes;	/* of this device's tx queue */
};

static struct net_device *devs[2 * MAX_PAIRS];
static unsigned int ndevs;

/* ---- data path --------------------------------------------------------------- */

/* Drop everything still queued for @p, counting it as received-and-dropped. */
static void lkl_vnet_flush(struct lkl_vnet *p)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&p->rxq))) {
		atomic64_inc(&p->rx_dropped);
		kfree_skb(skb);
	}
}

/*
 * Hand @skb over to @rcv: what __dev_forward_skb() does, but with the
 * drops counted here. False means the frame is too big for @rcv's MTU (or
 * its zerocopy pages could not be copied); the caller frees it.
 */
static bool lkl_vnet_forward(struct net_device *rcv, struct sk_buff *skb)
{
	if (skb_orphan_frags(skb, GFP_ATOMIC) || !is_skb_forwardable(rcv, skb))
		return false;
	/* Crossing namespaces: forget marks, socket timestamps, ipvs state. */
	skb_scrub_packet(skb, !net_eq(dev_net(rcv), dev_net(skb->dev)));
	skb->priority = 0;
	skb->protocol = eth_type_trans(skb, rcv);	/* also sets skb->dev */
	skb_postpull_rcsum(skb, eth_hdr(skb), ETH_HLEN);
	return true;
}

static netdev_tx_t lkl_vnet_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct lkl_vnet *p = netdev_priv(dev);
	struct lkl_vnet *pp = netdev_priv(p->peer);
	unsigned int len = skb->len;

	if (unlikely(!netif_running(p->peer))) {
		atomic64_inc(&p->tx_dropped);
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}
	atomic64_inc(&p->tx_packets);
	atomic64_add(len, &p->tx_bytes);

	if (unlikely(!lkl_vnet_forward(p->peer, skb))) {
		atomic64_inc(&pp->rx_dropped);
		atomic64_inc(&pp->rx_dropped_mtu);
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}
	skb_queue_tail(&pp->rxq, skb);
	napi_schedule(&pp->napi);

	/*
	 * Flow control. Only this queue (serialized by the core's tx lock)
	 * adds to the peer's ring, so the ring never exceeds rx_ring.
	 */
	if (skb_queue_len_lockless(&pp->rxq) >= rx_ring) {
		netif_stop_queue(dev);
		atomic64_inc(&p->queue_stops);
		/*
		 * The peer's poll may have drained the ring and checked our
		 * queue before we stopped it. Pairs with the smp_mb() in
		 * lkl_vnet_poll(): either it sees the stopped queue, or we
		 * see the drained ring here.
		 */
		smp_mb__after_atomic();
		if (skb_queue_len_lockless(&pp->rxq) <= rx_ring / 2) {
			netif_wake_queue(dev);
			atomic64_inc(&p->queue_wakes);
		}
	}
	return NETDEV_TX_OK;
}

static int lkl_vnet_poll(struct napi_struct *napi, int budget)
{
	struct lkl_vnet *p = container_of(napi, struct lkl_vnet, napi);
	struct lkl_vnet *pp = netdev_priv(p->peer);
	struct sk_buff *skb;
	int done = 0;

	while (done < budget && (skb = skb_dequeue(&p->rxq))) {
		atomic64_inc(&p->rx_packets);
		atomic64_add(skb->len + ETH_HLEN, &p->rx_bytes);
		napi_gro_receive(napi, skb);
		done++;
	}
	atomic64_inc(&p->napi_polls);
	if (budget && done == budget)
		atomic64_inc(&p->napi_budget_exhausted);

	/* Pairs with smp_mb__after_atomic() in lkl_vnet_xmit(). */
	smp_mb();
	if (netif_queue_stopped(p->peer) && netif_running(p->peer) &&
	    skb_queue_len_lockless(&p->rxq) <= rx_ring / 2) {
		netif_wake_queue(p->peer);
		atomic64_inc(&pp->queue_wakes);
	}

	/*
	 * A frame queued after our last dequeue scheduled NAPI again; the
	 * core then reports it here (napi_complete_done() returns false)
	 * and polls once more, so nothing is left behind.
	 */
	if (done < budget)
		napi_complete_done(napi, done);
	return done;
}

/* ---- control path (rtnl held) ------------------------------------------------- */

static int lkl_vnet_open(struct net_device *dev)
{
	struct lkl_vnet *p = netdev_priv(dev);

	lkl_vnet_flush(p);	/* frames that raced with our last stop */
	napi_enable(&p->napi);
	netif_start_queue(dev);
	if (netif_running(p->peer)) {
		netif_carrier_on(dev);
		netif_carrier_on(p->peer);
		/* The peer may have stopped while our ring was full. */
		netif_wake_queue(p->peer);
	}
	return 0;
}

static int lkl_vnet_stop(struct net_device *dev)
{
	struct lkl_vnet *p = netdev_priv(dev);

	netif_stop_queue(dev);
	netif_carrier_off(dev);
	netif_carrier_off(p->peer);
	napi_disable(&p->napi);
	lkl_vnet_flush(p);
	/* Let the peer's backlog drain (as tx_dropped) instead of sticking. */
	if (netif_running(p->peer))
		netif_wake_queue(p->peer);
	return 0;
}

static void lkl_vnet_get_stats64(struct net_device *dev,
				 struct rtnl_link_stats64 *s)
{
	struct lkl_vnet *p = netdev_priv(dev);

	s->tx_packets = atomic64_read(&p->tx_packets);
	s->tx_bytes = atomic64_read(&p->tx_bytes);
	s->tx_dropped = atomic64_read(&p->tx_dropped);
	s->rx_packets = atomic64_read(&p->rx_packets);
	s->rx_bytes = atomic64_read(&p->rx_bytes);
	s->rx_dropped = atomic64_read(&p->rx_dropped);
}

static const struct net_device_ops lkl_vnet_netdev_ops = {
	.ndo_open = lkl_vnet_open,
	.ndo_stop = lkl_vnet_stop,
	.ndo_start_xmit = lkl_vnet_xmit,
	.ndo_get_stats64 = lkl_vnet_get_stats64,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

/* ---- ethtool -S --------------------------------------------------------------- */

static const char lkl_vnet_gstrings[][ETH_GSTRING_LEN] = {
	"peer_ifindex",
	"rx_queue_len",
	"rx_dropped_mtu",
	"napi_polls",
	"napi_budget_exhausted",
	"queue_stops",
	"queue_wakes",
};

static void lkl_vnet_get_drvinfo(struct net_device *dev,
				 struct ethtool_drvinfo *info)
{
	strscpy(info->driver, KBUILD_MODNAME, sizeof(info->driver));
}

static int lkl_vnet_get_sset_count(struct net_device *dev, int sset)
{
	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;
	return ARRAY_SIZE(lkl_vnet_gstrings);
}

static void lkl_vnet_get_strings(struct net_device *dev, u32 sset, u8 *buf)
{
	if (sset == ETH_SS_STATS)
		memcpy(buf, lkl_vnet_gstrings, sizeof(lkl_vnet_gstrings));
}

static void lkl_vnet_get_ethtool_stats(struct net_device *dev,
				       struct ethtool_stats *stats, u64 *data)
{
	struct lkl_vnet *p = netdev_priv(dev);
	int i = 0;

	data[i++] = p->peer->ifindex;
	data[i++] = skb_queue_len_lockless(&p->rxq);
	data[i++] = atomic64_read(&p->rx_dropped_mtu);
	data[i++] = atomic64_read(&p->napi_polls);
	data[i++] = atomic64_read(&p->napi_budget_exhausted);
	data[i++] = atomic64_read(&p->queue_stops);
	data[i++] = atomic64_read(&p->queue_wakes);
}

static const struct ethtool_ops lkl_vnet_ethtool_ops = {
	.get_drvinfo = lkl_vnet_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_sset_count = lkl_vnet_get_sset_count,
	.get_strings = lkl_vnet_get_strings,
	.get_ethtool_stats = lkl_vnet_get_ethtool_stats,
};

/* ---- setup and teardown ---------------------------------------------------------- */

static void lkl_vnet_setup(struct net_device *dev)
{
	ether_setup(dev);
	dev->netdev_ops = &lkl_vnet_netdev_ops;
	dev->ethtool_ops = &lkl_vnet_ethtool_ops;
	dev->max_mtu = VNET_MAX_MTU;
}

/* Unregister the first @registered devices, then free all of them. */
static void lkl_vnet_destroy(unsigned int registered)
{
	LIST_HEAD(list);
	unsigned int i;

	rtnl_lock();
	for (i = 0; i < registered; i++)
		unregister_netdevice_queue(devs[i], &list);
	unregister_netdevice_many(&list);	/* closes them all first */
	rtnl_unlock();

	for (i = 0; i < ndevs; i++) {
		struct lkl_vnet *p = netdev_priv(devs[i]);

		skb_queue_purge(&p->rxq);
		netif_napi_del(&p->napi);
		free_netdev(devs[i]);
	}
	ndevs = 0;
}

static int __init lkl_vnet_init(void)
{
	unsigned int i, registered = 0;
	int ret;

	if (pairs < 1 || pairs > MAX_PAIRS || rx_ring < 16 || rx_ring > 4096) {
		pr_err("invalid parameters: pairs=%u (1-%d), rx_ring=%u (16-4096)\n",
		       pairs, MAX_PAIRS, rx_ring);
		return -EINVAL;
	}
	for (i = 0; i < 2 * pairs; i++) {
		/* Locally administered, stable: 02:6c:6b:00:00:<n>. */
		u8 mac[ETH_ALEN] = { 0x02, 0x6c, 0x6b, 0x00, 0x00, i + 1 };
		struct net_device *dev;
		struct lkl_vnet *p;

		dev = alloc_netdev(sizeof(*p), "lklvnet%d", NET_NAME_ENUM,
				   lkl_vnet_setup);
		if (!dev) {
			ret = -ENOMEM;
			goto err;
		}
		devs[ndevs++] = dev;
		p = netdev_priv(dev);
		p->dev = dev;
		skb_queue_head_init(&p->rxq);
		netif_napi_add(dev, &p->napi, lkl_vnet_poll);
		eth_hw_addr_set(dev, mac);
	}
	for (i = 0; i < ndevs; i++) {
		struct lkl_vnet *p = netdev_priv(devs[i]);

		p->peer = devs[i ^ 1];		/* 0<->1, 2<->3, ... */
	}
	for (i = 0; i < ndevs; i++) {
		netif_carrier_off(devs[i]);	/* until both ends are up */
		ret = register_netdev(devs[i]);
		if (ret)
			goto err;
		registered++;
	}
	pr_info("loaded: %u pair(s), rx_ring %u, %s..%s\n", pairs, rx_ring,
		devs[0]->name, devs[ndevs - 1]->name);
	return 0;

err:
	lkl_vnet_destroy(registered);
	return ret;
}

static void __exit lkl_vnet_exit(void)
{
	lkl_vnet_destroy(ndevs);
	pr_info("unloaded\n");
}

module_init(lkl_vnet_init);
module_exit(lkl_vnet_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jakub Witkowski");
MODULE_DESCRIPTION("Linux kernel lab: virtual Ethernet pairs with a NAPI receive path");
MODULE_VERSION("1.0");

/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdlib.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_ethdev.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_ip.h>
#include <rte_devargs.h>
#include <rte_kvargs.h>
#include <rte_dev.h>
#include <rte_alarm.h>
#include <rte_cycles.h>

#include "rte_eth_bond.h"
#include "rte_eth_bond_private.h"
#include "rte_eth_bond_8023ad_private.h"

#define REORDER_PERIOD_MS 10
/* Table for statistics in mode 5 TLB */
static uint64_t tlb_last_obytets[RTE_MAX_ETHPORTS];

static uint16_t
bond_ethdev_rx_burst(void *queue, struct rte_mbuf **bufs, uint16_t nb_pkts)
{
	struct bond_dev_private *internals;

	uint16_t num_rx_slave = 0;
	uint16_t num_rx_total = 0;

	int i;

	/* Cast to structure, containing bonded device's port id and queue id */
	struct bond_rx_queue *bd_rx_q = (struct bond_rx_queue *)queue;

	internals = bd_rx_q->dev_private;


	for (i = 0; i < internals->active_slave_count && nb_pkts; i++) {
		/* Offset of pointer to *bufs increases as packets are received
		 * from other slaves */
		num_rx_slave = rte_eth_rx_burst(internals->active_slaves[i],
				bd_rx_q->queue_id, bufs + num_rx_total, nb_pkts);
		if (num_rx_slave) {
			num_rx_total += num_rx_slave;
			nb_pkts -= num_rx_slave;
		}
	}

	return num_rx_total;
}

static uint16_t
bond_ethdev_rx_burst_active_backup(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_pkts)
{
	struct bond_dev_private *internals;

	/* Cast to structure, containing bonded device's port id and queue id */
	struct bond_rx_queue *bd_rx_q = (struct bond_rx_queue *)queue;

	internals = bd_rx_q->dev_private;

	return rte_eth_rx_burst(internals->current_primary_port,
			bd_rx_q->queue_id, bufs, nb_pkts);
}

static uint16_t
bond_ethdev_rx_burst_8023ad(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_pkts)
{
	/* Cast to structure, containing bonded device's port id and queue id */
	struct bond_rx_queue *bd_rx_q = (struct bond_rx_queue *)queue;
	struct bond_dev_private *internals = bd_rx_q->dev_private;
	struct ether_addr bond_mac;

	struct ether_hdr *hdr;

	const uint16_t ether_type_slow_be = rte_be_to_cpu_16(ETHER_TYPE_SLOW);
	uint16_t num_rx_total = 0;	/* Total number of received packets */
	uint8_t slaves[RTE_MAX_ETHPORTS];
	uint8_t slave_count;

	uint8_t collecting;  /* current slave collecting status */
	const uint8_t promisc = internals->promiscuous_en;
	uint8_t i, j, k;

	rte_eth_macaddr_get(internals->port_id, &bond_mac);
	/* Copy slave list to protect against slave up/down changes during tx
	 * bursting */
	slave_count = internals->active_slave_count;
	memcpy(slaves, internals->active_slaves,
			sizeof(internals->active_slaves[0]) * slave_count);

	for (i = 0; i < slave_count && num_rx_total < nb_pkts; i++) {
		j = num_rx_total;
		collecting = ACTOR_STATE(&mode_8023ad_ports[slaves[i]], COLLECTING);

		/* Read packets from this slave */
		num_rx_total += rte_eth_rx_burst(slaves[i], bd_rx_q->queue_id,
				&bufs[num_rx_total], nb_pkts - num_rx_total);

		for (k = j; k < 2 && k < num_rx_total; k++)
			rte_prefetch0(rte_pktmbuf_mtod(bufs[k], void *));

		/* Handle slow protocol packets. */
		while (j < num_rx_total) {
			if (j + 3 < num_rx_total)
				rte_prefetch0(rte_pktmbuf_mtod(bufs[j + 3], void *));

			hdr = rte_pktmbuf_mtod(bufs[j], struct ether_hdr *);
			/* Remove packet from array if it is slow packet or slave is not
			 * in collecting state or bondign interface is not in promiscus
			 * mode and packet address does not match. */
			if (unlikely(hdr->ether_type == ether_type_slow_be ||
				!collecting || (!promisc &&
					!is_same_ether_addr(&bond_mac, &hdr->d_addr)))) {

				if (hdr->ether_type == ether_type_slow_be) {
					bond_mode_8023ad_handle_slow_pkt(internals, slaves[i],
						bufs[j]);
				} else
					rte_pktmbuf_free(bufs[j]);

				/* Packet is managed by mode 4 or dropped, shift the array */
				num_rx_total--;
				if (j < num_rx_total) {
					memmove(&bufs[j], &bufs[j + 1], sizeof(bufs[0]) *
						(num_rx_total - j));
				}
			} else
				j++;
		}
	}

	return num_rx_total;
}

static uint16_t
bond_ethdev_tx_burst_round_robin(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_pkts)
{
	struct bond_dev_private *internals;
	struct bond_tx_queue *bd_tx_q;

	struct rte_mbuf *slave_bufs[RTE_MAX_ETHPORTS][nb_pkts];
	uint16_t slave_nb_pkts[RTE_MAX_ETHPORTS] = { 0 };

	uint8_t num_of_slaves;
	uint8_t slaves[RTE_MAX_ETHPORTS];

	uint16_t num_tx_total = 0, num_tx_slave;

	static int slave_idx = 0;
	int i, cslave_idx = 0, tx_fail_total = 0;

	bd_tx_q = (struct bond_tx_queue *)queue;
	internals = bd_tx_q->dev_private;

	/* Copy slave list to protect against slave up/down changes during tx
	 * bursting */
	num_of_slaves = internals->active_slave_count;
	memcpy(slaves, internals->active_slaves,
			sizeof(internals->active_slaves[0]) * num_of_slaves);

	if (num_of_slaves < 1)
		return num_tx_total;

	/* Populate slaves mbuf with which packets are to be sent on it  */
	for (i = 0; i < nb_pkts; i++) {
		cslave_idx = (slave_idx + i) % num_of_slaves;
		slave_bufs[cslave_idx][(slave_nb_pkts[cslave_idx])++] = bufs[i];
	}

	/* increment current slave index so the next call to tx burst starts on the
	 * next slave */
	slave_idx = ++cslave_idx;

	/* Send packet burst on each slave device */
	for (i = 0; i < num_of_slaves; i++) {
		if (slave_nb_pkts[i] > 0) {
			num_tx_slave = rte_eth_tx_burst(slaves[i], bd_tx_q->queue_id,
					slave_bufs[i], slave_nb_pkts[i]);

			/* if tx burst fails move packets to end of bufs */
			if (unlikely(num_tx_slave < slave_nb_pkts[i])) {
				int tx_fail_slave = slave_nb_pkts[i] - num_tx_slave;

				tx_fail_total += tx_fail_slave;

				memcpy(&bufs[nb_pkts - tx_fail_total],
						&slave_bufs[i][num_tx_slave],
						tx_fail_slave * sizeof(bufs[0]));
			}
			num_tx_total += num_tx_slave;
		}
	}

	return num_tx_total;
}

static uint16_t
bond_ethdev_tx_burst_active_backup(void *queue,
		struct rte_mbuf **bufs, uint16_t nb_pkts)
{
	struct bond_dev_private *internals;
	struct bond_tx_queue *bd_tx_q;

	bd_tx_q = (struct bond_tx_queue *)queue;
	internals = bd_tx_q->dev_private;

	if (internals->active_slave_count < 1)
		return 0;

	return rte_eth_tx_burst(internals->current_primary_port, bd_tx_q->queue_id,
			bufs, nb_pkts);
}

static inline uint16_t
ether_hash(struct ether_hdr *eth_hdr)
{
	uint16_t *word_src_addr = (uint16_t *)eth_hdr->s_addr.addr_bytes;
	uint16_t *word_dst_addr = (uint16_t *)eth_hdr->d_addr.addr_bytes;

	return (word_src_addr[0] ^ word_dst_addr[0]) ^
			(word_src_addr[1] ^ word_dst_addr[1]) ^
			(word_src_addr[2] ^ word_dst_addr[2]);
}

static inline uint32_t
ipv4_hash(struct ipv4_hdr *ipv4_hdr)
{
	return (ipv4_hdr->src_addr ^ ipv4_hdr->dst_addr);
}

static inline uint32_t
ipv6_hash(struct ipv6_hdr *ipv6_hdr)
{
	uint32_t *word_src_addr = (uint32_t *)&(ipv6_hdr->src_addr[0]);
	uint32_t *word_dst_addr = (uint32_t *)&(ipv6_hdr->dst_addr[0]);

	return (word_src_addr[0] ^ word_dst_addr[0]) ^
			(word_src_addr[1] ^ word_dst_addr[1]) ^
			(word_src_addr[2] ^ word_dst_addr[2]) ^
			(word_src_addr[3] ^ word_dst_addr[3]);
}

static uint32_t
udp_hash(struct udp_hdr *hdr)
{
	return hdr->src_port ^ hdr->dst_port;
}

static inline uint16_t
xmit_slave_hash(const struct rte_mbuf *buf, uint8_t slave_count, uint8_t policy)
{
	struct ether_hdr *eth_hdr;
	struct udp_hdr *udp_hdr;
	size_t eth_offset = 0;
	uint32_t hash = 0;

	if (slave_count == 1)
		return 0;

	switch (policy) {
	case BALANCE_XMIT_POLICY_LAYER2:
		eth_hdr = rte_pktmbuf_mtod(buf, struct ether_hdr *);

		hash = ether_hash(eth_hdr);
		hash ^= hash >> 8;
		return hash % slave_count;

	case BALANCE_XMIT_POLICY_LAYER23:
		eth_hdr = rte_pktmbuf_mtod(buf, struct ether_hdr *);

		if (buf->ol_flags & PKT_RX_VLAN_PKT)
			eth_offset = sizeof(struct ether_hdr) + sizeof(struct vlan_hdr);
		else
			eth_offset = sizeof(struct ether_hdr);

		if (buf->ol_flags & PKT_RX_IPV4_HDR) {
			struct ipv4_hdr *ipv4_hdr;
			ipv4_hdr = (struct ipv4_hdr *)(rte_pktmbuf_mtod(buf,
					unsigned char *) + eth_offset);

			hash = ether_hash(eth_hdr) ^ ipv4_hash(ipv4_hdr);

		} else {
			struct ipv6_hdr *ipv6_hdr;

			ipv6_hdr = (struct ipv6_hdr *)(rte_pktmbuf_mtod(buf,
					unsigned char *) + eth_offset);

			hash = ether_hash(eth_hdr) ^ ipv6_hash(ipv6_hdr);
		}
		break;

	case BALANCE_XMIT_POLICY_LAYER34:
		if (buf->ol_flags & PKT_RX_VLAN_PKT)
			eth_offset = sizeof(struct ether_hdr) + sizeof(struct vlan_hdr);
		else
			eth_offset = sizeof(struct ether_hdr);

		if (buf->ol_flags & PKT_RX_IPV4_HDR) {
			struct ipv4_hdr *ipv4_hdr = (struct ipv4_hdr *)
					(rte_pktmbuf_mtod(buf, unsigned char *) + eth_offset);

			if (ipv4_hdr->next_proto_id == IPPROTO_UDP) {
				udp_hdr = (struct udp_hdr *)
						(rte_pktmbuf_mtod(buf, unsigned char *) + eth_offset +
								sizeof(struct ipv4_hdr));
				hash = ipv4_hash(ipv4_hdr) ^ udp_hash(udp_hdr);
			} else {
				hash = ipv4_hash(ipv4_hdr);
			}
		} else {
			struct ipv6_hdr *ipv6_hdr = (struct ipv6_hdr *)
					(rte_pktmbuf_mtod(buf, unsigned char *) + eth_offset);

			if (ipv6_hdr->proto == IPPROTO_UDP) {
				udp_hdr = (struct udp_hdr *)
						(rte_pktmbuf_mtod(buf, unsigned char *) + eth_offset +
								sizeof(struct ipv6_hdr));
				hash = ipv6_hash(ipv6_hdr) ^ udp_hash(udp_hdr);
			} else {
				hash = ipv6_hash(ipv6_hdr);
			}
		}
		break;
	}

	hash ^= hash >> 16;
	hash ^= hash >> 8;

	return hash % slave_count;
}

struct bwg_slave {
	uint64_t bwg_left_int;
	uint64_t bwg_left_remainder;
	uint8_t slave;
};

static int
bandwidth_cmp(const void *a, const void *b)
{
	const struct bwg_slave *bwg_a = a;
	const struct bwg_slave *bwg_b = b;
	int64_t diff = (int64_t)bwg_b->bwg_left_int - (int64_t)bwg_a->bwg_left_int;
	int64_t diff2 = (int64_t)bwg_b->bwg_left_remainder -
			(int64_t)bwg_a->bwg_left_remainder;
	if (diff > 0)
		return 1;
	else if (diff < 0)
		return -1;
	else if (diff2 > 0)
		return 1;
	else if (diff2 < 0)
		return -1;
	else
		return 0;
}

static void
bandwidth_left(int port_id, uint64_t load, uint8_t update_idx,
		struct bwg_slave *bwg_slave)
{
	struct rte_eth_link link_status;

	rte_eth_link_get(port_id, &link_status);
	uint64_t link_bwg = link_status.link_speed * 1000000ULL / 8;
	if (link_bwg == 0)
		return;
	link_bwg = (link_bwg * (update_idx+1) * REORDER_PERIOD_MS);
	bwg_slave->bwg_left_int = (link_bwg - 1000*load) / link_bwg;
	bwg_slave->bwg_left_remainder = (link_bwg - 1000*load) % link_bwg;
}

static void
bond_ethdev_update_tlb_slave_cb(void *arg)
{
	struct bond_dev_private *internals = arg;
	struct rte_eth_stats slave_stats;
	struct bwg_slave bwg_array[RTE_MAX_ETHPORTS];
	uint8_t slave_count;
	uint64_t tx_bytes;

	uint8_t update_stats = 0;
	uint8_t i, slave_id;

	internals->slave_update_idx++;


	if (internals->slave_update_idx >= REORDER_PERIOD_MS)
		update_stats = 1;

	for (i = 0; i < internals->active_slave_count; i++) {
		slave_id = internals->active_slaves[i];
		rte_eth_stats_get(slave_id, &slave_stats);
		tx_bytes = slave_stats.obytes - tlb_last_obytets[slave_id];
		bandwidth_left(slave_id, tx_bytes,
				internals->slave_update_idx, &bwg_array[i]);
		bwg_array[i].slave = slave_id;

		if (update_stats)
			tlb_last_obytets[slave_id] = slave_stats.obytes;
	}

	if (update_stats == 1)
		internals->slave_update_idx = 0;

	slave_count = i;
	qsort(bwg_array, slave_count, sizeof(bwg_array[0]), bandwidth_cmp);
	for (i = 0; i < slave_count; i++)
		internals->active_slaves[i] = bwg_array[i].slave;

	rte_eal_alarm_set(REORDER_PERIOD_MS * 1000, bond_ethdev_update_tlb_slave_cb,
			(struct bond_dev_private *)internals);
}

static uint16_t
bond_ethdev_tx_burst_tlb(void *queue, struct rte_mbuf **bufs, uint16_t nb_pkts)
{
	struct bond_tx_queue *bd_tx_q = (struct bond_tx_queue *)queue;
	struct bond_dev_private *internals = bd_tx_q->dev_private;

	struct rte_eth_dev *primary_port =
			&rte_eth_devices[internals->primary_port];
	uint16_t num_tx_total = 0;
	uint8_t i, j;

	uint8_t num_of_slaves = internals->active_slave_count;
	uint8_t slaves[RTE_MAX_ETHPORTS];

	struct ether_hdr *ether_hdr;
	struct ether_addr primary_slave_addr;
	struct ether_addr active_slave_addr;

	if (num_of_slaves < 1)
		return num_tx_total;

	memcpy(slaves, internals->active_slaves,
				sizeof(internals->active_slaves[0]) * num_of_slaves);


	ether_addr_copy(primary_port->data->mac_addrs, &primary_slave_addr);

	if (nb_pkts > 3) {
		for (i = 0; i < 3; i++)
			rte_prefetch0(rte_pktmbuf_mtod(bufs[i], void*));
	}

	for (i = 0; i < num_of_slaves; i++) {
		ether_addr_copy(&internals->slaves[slaves[i]].persisted_mac_addr,
				&active_slave_addr);

		for (j = num_tx_total; j < nb_pkts; j++) {
			if (j + 3 < nb_pkts)
				rte_prefetch0(rte_pktmbuf_mtod(bufs[j+3], void*));

			ether_hdr = rte_pktmbuf_mtod(bufs[j], struct ether_hdr *);
			if (is_same_ether_addr(&ether_hdr->s_addr, &primary_slave_addr))
				ether_addr_copy(&active_slave_addr, &ether_hdr->s_addr);
		}

		num_tx_total += rte_eth_tx_burst(slaves[i], bd_tx_q->queue_id,
				bufs + num_tx_total, nb_pkts - num_tx_total);

		if (num_tx_total == nb_pkts)
			break;
	}

	return num_tx_total;
}

static uint16_t
bond_ethdev_tx_burst_balance(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_pkts)
{
	struct bond_dev_private *internals;
	struct bond_tx_queue *bd_tx_q;

	uint8_t num_of_slaves;
	uint8_t slaves[RTE_MAX_ETHPORTS];

	uint16_t num_tx_total = 0, num_tx_slave = 0, tx_fail_total = 0;

	int i, op_slave_id;

	struct rte_mbuf *slave_bufs[RTE_MAX_ETHPORTS][nb_pkts];
	uint16_t slave_nb_pkts[RTE_MAX_ETHPORTS] = { 0 };

	bd_tx_q = (struct bond_tx_queue *)queue;
	internals = bd_tx_q->dev_private;

	/* Copy slave list to protect against slave up/down changes during tx
	 * bursting */
	num_of_slaves = internals->active_slave_count;
	memcpy(slaves, internals->active_slaves,
			sizeof(internals->active_slaves[0]) * num_of_slaves);

	if (num_of_slaves < 1)
		return num_tx_total;

	/* Populate slaves mbuf with the packets which are to be sent on it  */
	for (i = 0; i < nb_pkts; i++) {
		/* Select output slave using hash based on xmit policy */
		op_slave_id = xmit_slave_hash(bufs[i], num_of_slaves,
				internals->balance_xmit_policy);

		/* Populate slave mbuf arrays with mbufs for that slave */
		slave_bufs[op_slave_id][slave_nb_pkts[op_slave_id]++] = bufs[i];
	}

	/* Send packet burst on each slave device */
	for (i = 0; i < num_of_slaves; i++) {
		if (slave_nb_pkts[i] > 0) {
			num_tx_slave = rte_eth_tx_burst(slaves[i], bd_tx_q->queue_id,
					slave_bufs[i], slave_nb_pkts[i]);

			/* if tx burst fails move packets to end of bufs */
			if (unlikely(num_tx_slave < slave_nb_pkts[i])) {
				int slave_tx_fail_count = slave_nb_pkts[i] - num_tx_slave;

				tx_fail_total += slave_tx_fail_count;
				memcpy(&bufs[nb_pkts - tx_fail_total],
						&slave_bufs[i][num_tx_slave],
						slave_tx_fail_count * sizeof(bufs[0]));
			}

			num_tx_total += num_tx_slave;
		}
	}

	return num_tx_total;
}

static uint16_t
bond_ethdev_tx_burst_8023ad(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_pkts)
{
	struct bond_dev_private *internals;
	struct bond_tx_queue *bd_tx_q;

	uint8_t num_of_slaves;
	uint8_t slaves[RTE_MAX_ETHPORTS];
	 /* possitions in slaves, not ID */
	uint8_t distributing_offsets[RTE_MAX_ETHPORTS];
	uint8_t distributing_count;

	uint16_t num_tx_slave, num_tx_total = 0, num_tx_fail_total = 0;
	uint16_t i, j, op_slave_idx;
	const uint16_t buffs_size = nb_pkts + BOND_MODE_8023AX_SLAVE_TX_PKTS + 1;

	/* Allocate additional packets in case 8023AD mode. */
	struct rte_mbuf *slave_bufs[RTE_MAX_ETHPORTS][buffs_size];
	void *slow_pkts[BOND_MODE_8023AX_SLAVE_TX_PKTS] = { NULL };

	/* Total amount of packets in slave_bufs */
	uint16_t slave_nb_pkts[RTE_MAX_ETHPORTS] = { 0 };
	/* Slow packets placed in each slave */
	uint8_t slave_slow_nb_pkts[RTE_MAX_ETHPORTS] = { 0 };

	bd_tx_q = (struct bond_tx_queue *)queue;
	internals = bd_tx_q->dev_private;

	/* Copy slave list to protect against slave up/down changes during tx
	 * bursting */
	num_of_slaves = internals->active_slave_count;
	if (num_of_slaves < 1)
		return num_tx_total;

	memcpy(slaves, internals->active_slaves, sizeof(slaves[0]) * num_of_slaves);

	distributing_count = 0;
	for (i = 0; i < num_of_slaves; i++) {
		struct port *port = &mode_8023ad_ports[slaves[i]];

		slave_slow_nb_pkts[i] = rte_ring_dequeue_burst(port->tx_ring,
				slow_pkts, BOND_MODE_8023AX_SLAVE_TX_PKTS);
		slave_nb_pkts[i] = slave_slow_nb_pkts[i];

		for (j = 0; j < slave_slow_nb_pkts[i]; j++)
			slave_bufs[i][j] = slow_pkts[j];

		if (ACTOR_STATE(port, DISTRIBUTING))
			distributing_offsets[distributing_count++] = i;
	}

	if (likely(distributing_count > 0)) {
		/* Populate slaves mbuf with the packets which are to be sent on it */
		for (i = 0; i < nb_pkts; i++) {
			/* Select output slave using hash based on xmit policy */
			op_slave_idx = xmit_slave_hash(bufs[i], distributing_count,
					internals->balance_xmit_policy);

			/* Populate slave mbuf arrays with mbufs for that slave. Use only
			 * slaves that are currently distributing. */
			uint8_t slave_offset = distributing_offsets[op_slave_idx];
			slave_bufs[slave_offset][slave_nb_pkts[slave_offset]] = bufs[i];
			slave_nb_pkts[slave_offset]++;
		}
	}

	/* Send packet burst on each slave device */
	for (i = 0; i < num_of_slaves; i++) {
		if (slave_nb_pkts[i] == 0)
			continue;

		num_tx_slave = rte_eth_tx_burst(slaves[i], bd_tx_q->queue_id,
				slave_bufs[i], slave_nb_pkts[i]);

		/* If tx burst fails drop slow packets */
		for ( ; num_tx_slave < slave_slow_nb_pkts[i]; num_tx_slave++)
			rte_pktmbuf_free(slave_bufs[i][num_tx_slave]);

		num_tx_total += num_tx_slave - slave_slow_nb_pkts[i];
		num_tx_fail_total += slave_nb_pkts[i] - num_tx_slave;

		/* If tx burst fails move packets to end of bufs */
		if (unlikely(num_tx_slave < slave_nb_pkts[i])) {
			uint16_t j = nb_pkts - num_tx_fail_total;
			for ( ; num_tx_slave < slave_nb_pkts[i]; j++, num_tx_slave++)
				bufs[j] = slave_bufs[i][num_tx_slave];
		}
	}

	return num_tx_total;
}

#ifdef RTE_MBUF_REFCNT
static uint16_t
bond_ethdev_tx_burst_broadcast(void *queue, struct rte_mbuf **bufs,
		uint16_t nb_pkts)
{
	struct bond_dev_private *internals;
	struct bond_tx_queue *bd_tx_q;

	uint8_t tx_failed_flag = 0, num_of_slaves;
	uint8_t slaves[RTE_MAX_ETHPORTS];

	uint16_t max_nb_of_tx_pkts = 0;

	int slave_tx_total[RTE_MAX_ETHPORTS];
	int i, most_successful_tx_slave = -1;

	bd_tx_q = (struct bond_tx_queue *)queue;
	internals = bd_tx_q->dev_private;

	/* Copy slave list to protect against slave up/down changes during tx
	 * bursting */
	num_of_slaves = internals->active_slave_count;
	memcpy(slaves, internals->active_slaves,
			sizeof(internals->active_slaves[0]) * num_of_slaves);

	if (num_of_slaves < 1)
		return 0;

	/* Increment reference count on mbufs */
	for (i = 0; i < nb_pkts; i++)
		rte_mbuf_refcnt_update(bufs[i], num_of_slaves - 1);

	/* Transmit burst on each active slave */
	for (i = 0; i < num_of_slaves; i++) {
		slave_tx_total[i] = rte_eth_tx_burst(slaves[i], bd_tx_q->queue_id,
					bufs, nb_pkts);

		if (unlikely(slave_tx_total[i] < nb_pkts))
			tx_failed_flag = 1;

		/* record the value and slave index for the slave which transmits the
		 * maximum number of packets */
		if (slave_tx_total[i] > max_nb_of_tx_pkts) {
			max_nb_of_tx_pkts = slave_tx_total[i];
			most_successful_tx_slave = i;
		}
	}

	/* if slaves fail to transmit packets from burst, the calling application
	 * is not expected to know about multiple references to packets so we must
	 * handle failures of all packets except those of the most successful slave
	 */
	if (unlikely(tx_failed_flag))
		for (i = 0; i < num_of_slaves; i++)
			if (i != most_successful_tx_slave)
				while (slave_tx_total[i] < nb_pkts)
					rte_pktmbuf_free(bufs[slave_tx_total[i]++]);

	return max_nb_of_tx_pkts;
}
#endif

void
link_properties_set(struct rte_eth_dev *bonded_eth_dev,
		struct rte_eth_link *slave_dev_link)
{
	struct rte_eth_link *bonded_dev_link = &bonded_eth_dev->data->dev_link;
	struct bond_dev_private *internals = bonded_eth_dev->data->dev_private;

	if (slave_dev_link->link_status &&
		bonded_eth_dev->data->dev_started) {
		bonded_dev_link->link_duplex = slave_dev_link->link_duplex;
		bonded_dev_link->link_speed = slave_dev_link->link_speed;

		internals->link_props_set = 1;
	}
}

void
link_properties_reset(struct rte_eth_dev *bonded_eth_dev)
{
	struct bond_dev_private *internals = bonded_eth_dev->data->dev_private;

	memset(&(bonded_eth_dev->data->dev_link), 0,
			sizeof(bonded_eth_dev->data->dev_link));

	internals->link_props_set = 0;
}

int
link_properties_valid(struct rte_eth_link *bonded_dev_link,
		struct rte_eth_link *slave_dev_link)
{
	if (bonded_dev_link->link_duplex != slave_dev_link->link_duplex ||
		bonded_dev_link->link_speed !=  slave_dev_link->link_speed)
		return -1;

	return 0;
}

int
mac_address_get(struct rte_eth_dev *eth_dev, struct ether_addr *dst_mac_addr)
{
	struct ether_addr *mac_addr;

	if (eth_dev == NULL) {
		RTE_LOG(ERR, PMD, "%s: NULL pointer eth_dev specified\n", __func__);
		return -1;
	}

	if (dst_mac_addr == NULL) {
		RTE_LOG(ERR, PMD, "%s: NULL pointer MAC specified\n", __func__);
		return -1;
	}

	mac_addr = eth_dev->data->mac_addrs;

	ether_addr_copy(mac_addr, dst_mac_addr);
	return 0;
}

int
mac_address_set(struct rte_eth_dev *eth_dev, struct ether_addr *new_mac_addr)
{
	struct ether_addr *mac_addr;

	if (eth_dev == NULL) {
		RTE_BOND_LOG(ERR, "NULL pointer eth_dev specified");
		return -1;
	}

	if (new_mac_addr == NULL) {
		RTE_BOND_LOG(ERR, "NULL pointer MAC specified");
		return -1;
	}

	mac_addr = eth_dev->data->mac_addrs;

	/* If new MAC is different to current MAC then update */
	if (memcmp(mac_addr, new_mac_addr, sizeof(*mac_addr)) != 0)
		memcpy(mac_addr, new_mac_addr, sizeof(*mac_addr));

	return 0;
}

int
mac_address_slaves_update(struct rte_eth_dev *bonded_eth_dev)
{
	struct bond_dev_private *internals = bonded_eth_dev->data->dev_private;
	int i;

	/* Update slave devices MAC addresses */
	if (internals->slave_count < 1)
		return -1;

	switch (internals->mode) {
	case BONDING_MODE_ROUND_ROBIN:
	case BONDING_MODE_BALANCE:
#ifdef RTE_MBUF_REFCNT
	case BONDING_MODE_BROADCAST:
#endif
		for (i = 0; i < internals->slave_count; i++) {
			if (mac_address_set(&rte_eth_devices[internals->slaves[i].port_id],
					bonded_eth_dev->data->mac_addrs)) {
				RTE_BOND_LOG(ERR, "Failed to update port Id %d MAC address",
						internals->slaves[i].port_id);
				return -1;
			}
		}
		break;
	case BONDING_MODE_8023AD:
		bond_mode_8023ad_mac_address_update(bonded_eth_dev);
		break;
	case BONDING_MODE_ACTIVE_BACKUP:
	case BONDING_MODE_ADAPTIVE_TRANSMIT_LOAD_BALANCING:
	default:
		for (i = 0; i < internals->slave_count; i++) {
			if (internals->slaves[i].port_id ==
					internals->current_primary_port) {
				if (mac_address_set(&rte_eth_devices[internals->primary_port],
						bonded_eth_dev->data->mac_addrs)) {
					RTE_BOND_LOG(ERR, "Failed to update port Id %d MAC address",
							internals->current_primary_port);
					return -1;
				}
			} else {
				if (mac_address_set(
						&rte_eth_devices[internals->slaves[i].port_id],
						&internals->slaves[i].persisted_mac_addr)) {
					RTE_BOND_LOG(ERR, "Failed to update port Id %d MAC address",
							internals->slaves[i].port_id);
					return -1;
				}
			}
		}
	}

	return 0;
}

int
bond_ethdev_mode_set(struct rte_eth_dev *eth_dev, int mode)
{
	struct bond_dev_private *internals;

	internals = eth_dev->data->dev_private;

	switch (mode) {
	case BONDING_MODE_ROUND_ROBIN:
		eth_dev->tx_pkt_burst = bond_ethdev_tx_burst_round_robin;
		eth_dev->rx_pkt_burst = bond_ethdev_rx_burst;
		break;
	case BONDING_MODE_ACTIVE_BACKUP:
		eth_dev->tx_pkt_burst = bond_ethdev_tx_burst_active_backup;
		eth_dev->rx_pkt_burst = bond_ethdev_rx_burst_active_backup;
		break;
	case BONDING_MODE_BALANCE:
		eth_dev->tx_pkt_burst = bond_ethdev_tx_burst_balance;
		eth_dev->rx_pkt_burst = bond_ethdev_rx_burst;
		break;
#ifdef RTE_MBUF_REFCNT
	case BONDING_MODE_BROADCAST:
		eth_dev->tx_pkt_burst = bond_ethdev_tx_burst_broadcast;
		eth_dev->rx_pkt_burst = bond_ethdev_rx_burst;
		break;
#endif
	case BONDING_MODE_8023AD:
		if (bond_mode_8023ad_enable(eth_dev) != 0)
			return -1;

		eth_dev->rx_pkt_burst = bond_ethdev_rx_burst_8023ad;
		eth_dev->tx_pkt_burst = bond_ethdev_tx_burst_8023ad;
		RTE_BOND_LOG(WARNING,
				"Using mode 4, it is necessary to do TX burst and RX burst "
				"at least every 100ms.");
		break;
	case BONDING_MODE_ADAPTIVE_TRANSMIT_LOAD_BALANCING:
		eth_dev->tx_pkt_burst = bond_ethdev_tx_burst_tlb;
		eth_dev->rx_pkt_burst = bond_ethdev_rx_burst_active_backup;
		break;
	default:
		return -1;
	}

	internals->mode = mode;

	return 0;
}

int
slave_configure(struct rte_eth_dev *bonded_eth_dev,
		struct rte_eth_dev *slave_eth_dev)
{
	struct bond_rx_queue *bd_rx_q;
	struct bond_tx_queue *bd_tx_q;

	int errval, q_id;

	/* Stop slave */
	rte_eth_dev_stop(slave_eth_dev->data->port_id);

	/* Enable interrupts on slave device if supported */
	if (slave_eth_dev->driver->pci_drv.drv_flags & RTE_PCI_DRV_INTR_LSC)
		slave_eth_dev->data->dev_conf.intr_conf.lsc = 1;

	/* Configure device */
	errval = rte_eth_dev_configure(slave_eth_dev->data->port_id,
			bonded_eth_dev->data->nb_rx_queues,
			bonded_eth_dev->data->nb_tx_queues,
			&(slave_eth_dev->data->dev_conf));
	if (errval != 0) {
		RTE_BOND_LOG(ERR, "Cannot configure slave device: port %u , err (%d)",
				slave_eth_dev->data->port_id, errval);
		return errval;
	}

	/* Setup Rx Queues */
	for (q_id = 0; q_id < bonded_eth_dev->data->nb_rx_queues; q_id++) {
		bd_rx_q = (struct bond_rx_queue *)bonded_eth_dev->data->rx_queues[q_id];

		errval = rte_eth_rx_queue_setup(slave_eth_dev->data->port_id, q_id,
				bd_rx_q->nb_rx_desc,
				rte_eth_dev_socket_id(slave_eth_dev->data->port_id),
				&(bd_rx_q->rx_conf), bd_rx_q->mb_pool);
		if (errval != 0) {
			RTE_BOND_LOG(ERR,
					"rte_eth_rx_queue_setup: port=%d queue_id %d, err (%d)",
					slave_eth_dev->data->port_id, q_id, errval);
			return errval;
		}
	}

	/* Setup Tx Queues */
	for (q_id = 0; q_id < bonded_eth_dev->data->nb_tx_queues; q_id++) {
		bd_tx_q = (struct bond_tx_queue *)bonded_eth_dev->data->tx_queues[q_id];

		errval = rte_eth_tx_queue_setup(slave_eth_dev->data->port_id, q_id,
				bd_tx_q->nb_tx_desc,
				rte_eth_dev_socket_id(slave_eth_dev->data->port_id),
				&bd_tx_q->tx_conf);
		if (errval != 0) {
			RTE_BOND_LOG(ERR,
					"rte_eth_tx_queue_setup: port=%d queue_id %d, err (%d)",
					slave_eth_dev->data->port_id, q_id, errval);
			return errval;
		}
	}

	/* Start device */
	errval = rte_eth_dev_start(slave_eth_dev->data->port_id);
	if (errval != 0) {
		RTE_BOND_LOG(ERR, "rte_eth_dev_start: port=%u, err (%d)",
				slave_eth_dev->data->port_id, errval);
		return -1;
	}

	return 0;
}

void
slave_remove(struct bond_dev_private *internals,
		struct rte_eth_dev *slave_eth_dev)
{
	uint8_t i;

	for (i = 0; i < internals->slave_count; i++)
		if (internals->slaves[i].port_id ==
				slave_eth_dev->data->port_id)
			break;

	if (i < (internals->slave_count - 1))
		memmove(&internals->slaves[i], &internals->slaves[i + 1],
				sizeof(internals->slaves[0]) *
				(internals->slave_count - i - 1));

	internals->slave_count--;
}

static void
bond_ethdev_slave_link_status_change_monitor(void *cb_arg);

void
slave_add(struct bond_dev_private *internals,
		struct rte_eth_dev *slave_eth_dev)
{
	struct bond_slave_details *slave_details =
			&internals->slaves[internals->slave_count];

	slave_details->port_id = slave_eth_dev->data->port_id;
	slave_details->last_link_status = 0;

	/* If slave device doesn't support interrupts then we need to enabled
	 * polling to monitor link status */
	if (!(slave_eth_dev->pci_dev->driver->drv_flags & RTE_PCI_DRV_INTR_LSC)) {
		slave_details->link_status_poll_enabled = 1;

		if (!internals->link_status_polling_enabled) {
			internals->link_status_polling_enabled = 1;

			rte_eal_alarm_set(internals->link_status_polling_interval_ms * 1000,
					bond_ethdev_slave_link_status_change_monitor,
					(void *)&rte_eth_devices[internals->port_id]);
		}
	}

	slave_details->link_status_wait_to_complete = 0;
	/* clean tlb_last_obytes when adding port for bonding device */
	memcpy(&(slave_details->persisted_mac_addr), slave_eth_dev->data->mac_addrs,
			sizeof(struct ether_addr));
}

void
bond_ethdev_primary_set(struct bond_dev_private *internals,
		uint8_t slave_port_id)
{
	int i;

	if (internals->active_slave_count < 1)
		internals->current_primary_port = slave_port_id;
	else
		/* Search bonded device slave ports for new proposed primary port */
		for (i = 0; i < internals->active_slave_count; i++) {
			if (internals->active_slaves[i] == slave_port_id)
				internals->current_primary_port = slave_port_id;
		}
}

static void
bond_ethdev_promiscuous_enable(struct rte_eth_dev *eth_dev);

static int
bond_ethdev_start(struct rte_eth_dev *eth_dev)
{
	struct bond_dev_private *internals;
	int i;

	/* slave eth dev will be started by bonded device */
	if (valid_bonded_ethdev(eth_dev)) {
		RTE_BOND_LOG(ERR, "User tried to explicitly start a slave eth_dev (%d)",
				eth_dev->data->port_id);
		return -1;
	}

	eth_dev->data->dev_link.link_status = 0;
	eth_dev->data->dev_started = 1;

	internals = eth_dev->data->dev_private;

	if (internals->slave_count == 0) {
		RTE_BOND_LOG(ERR, "Cannot start port since there are no slave devices");
		return -1;
	}

	if (internals->user_defined_mac == 0) {
		struct ether_addr *new_mac_addr = NULL;

		for (i = 0; i < internals->slave_count; i++)
			if (internals->slaves[i].port_id == internals->primary_port)
				new_mac_addr = &internals->slaves[i].persisted_mac_addr;

		if (new_mac_addr == NULL)
			return -1;

		if (mac_address_set(eth_dev, new_mac_addr) != 0) {
			RTE_BOND_LOG(ERR, "bonded port (%d) failed to update MAC address",
					eth_dev->data->port_id);
			return -1;
		}
	}

	/* Update all slave devices MACs*/
	if (mac_address_slaves_update(eth_dev) != 0)
		return -1;

	/* If bonded device is configure in promiscuous mode then re-apply config */
	if (internals->promiscuous_en)
		bond_ethdev_promiscuous_enable(eth_dev);

	/* Reconfigure each slave device if starting bonded device */
	for (i = 0; i < internals->slave_count; i++) {
		if (slave_configure(eth_dev,
				&(rte_eth_devices[internals->slaves[i].port_id])) != 0) {
			RTE_BOND_LOG(ERR,
					"bonded port (%d) failed to reconfigure slave device (%d)",
					eth_dev->data->port_id, internals->slaves[i].port_id);
			return -1;
		}
	}

	if (internals->user_defined_primary_port)
		bond_ethdev_primary_set(internals, internals->primary_port);

	if (internals->mode == BONDING_MODE_8023AD)
		bond_mode_8023ad_start(eth_dev);

	if (internals->mode == BONDING_MODE_ADAPTIVE_TRANSMIT_LOAD_BALANCING)
		bond_ethdev_update_tlb_slave_cb(internals);

	return 0;
}

static void
bond_ethdev_stop(struct rte_eth_dev *eth_dev)
{
	struct bond_dev_private *internals = eth_dev->data->dev_private;
	uint8_t i;

	if (internals->mode == BONDING_MODE_8023AD) {
		struct port *port;
		void *pkt = NULL;

		bond_mode_8023ad_stop(eth_dev);

		/* Discard all messages to/from mode 4 state machines */
		for (i = 0; i < internals->slave_count; i++) {
			port = &mode_8023ad_ports[internals->slaves[i].port_id];

			RTE_VERIFY(port->rx_ring != NULL);
			while (rte_ring_dequeue(port->rx_ring, &pkt) != -ENOENT)
				rte_pktmbuf_free(pkt);

			RTE_VERIFY(port->tx_ring != NULL);
			while (rte_ring_dequeue(port->tx_ring, &pkt) != -ENOENT)
				rte_pktmbuf_free(pkt);
		}
	}

	if (internals->mode == BONDING_MODE_ADAPTIVE_TRANSMIT_LOAD_BALANCING) {
		rte_eal_alarm_cancel(bond_ethdev_update_tlb_slave_cb, internals);
	}

	internals->active_slave_count = 0;
	internals->link_status_polling_enabled = 0;

	eth_dev->data->dev_link.link_status = 0;
	eth_dev->data->dev_started = 0;
}

static void
bond_ethdev_close(struct rte_eth_dev *dev __rte_unused)
{
}

/* forward declaration */
static int bond_ethdev_configure(struct rte_eth_dev *dev);

static void
bond_ethdev_info(struct rte_eth_dev *dev, struct rte_eth_dev_info *dev_info)
{
	struct bond_dev_private *internals = dev->data->dev_private;

	dev_info->driver_name = driver_name;
	dev_info->max_mac_addrs = 1;

	dev_info->max_rx_pktlen = (uint32_t)2048;

	dev_info->max_rx_queues = (uint16_t)128;
	dev_info->max_tx_queues = (uint16_t)512;

	dev_info->min_rx_bufsize = 0;
	dev_info->pci_dev = dev->pci_dev;

	dev_info->rx_offload_capa = internals->rx_offload_capa;
	dev_info->tx_offload_capa = internals->tx_offload_capa;
}

static int
bond_ethdev_rx_queue_setup(struct rte_eth_dev *dev, uint16_t rx_queue_id,
		uint16_t nb_rx_desc, unsigned int socket_id __rte_unused,
		const struct rte_eth_rxconf *rx_conf, struct rte_mempool *mb_pool)
{
	struct bond_rx_queue *bd_rx_q = (struct bond_rx_queue *)
			rte_zmalloc_socket(NULL, sizeof(struct bond_rx_queue),
					0, dev->pci_dev->numa_node);
	if (bd_rx_q == NULL)
		return -1;

	bd_rx_q->queue_id = rx_queue_id;
	bd_rx_q->dev_private = dev->data->dev_private;

	bd_rx_q->nb_rx_desc = nb_rx_desc;

	memcpy(&(bd_rx_q->rx_conf), rx_conf, sizeof(struct rte_eth_rxconf));
	bd_rx_q->mb_pool = mb_pool;

	dev->data->rx_queues[rx_queue_id] = bd_rx_q;

	return 0;
}

static int
bond_ethdev_tx_queue_setup(struct rte_eth_dev *dev, uint16_t tx_queue_id,
		uint16_t nb_tx_desc, unsigned int socket_id __rte_unused,
		const struct rte_eth_txconf *tx_conf)
{
	struct bond_tx_queue *bd_tx_q  = (struct bond_tx_queue *)
			rte_zmalloc_socket(NULL, sizeof(struct bond_tx_queue),
					0, dev->pci_dev->numa_node);

	if (bd_tx_q == NULL)
		return -1;

	bd_tx_q->queue_id = tx_queue_id;
	bd_tx_q->dev_private = dev->data->dev_private;

	bd_tx_q->nb_tx_desc = nb_tx_desc;
	memcpy(&(bd_tx_q->tx_conf), tx_conf, sizeof(bd_tx_q->tx_conf));

	dev->data->tx_queues[tx_queue_id] = bd_tx_q;

	return 0;
}

static void
bond_ethdev_rx_queue_release(void *queue)
{
	if (queue == NULL)
		return;

	rte_free(queue);
}

static void
bond_ethdev_tx_queue_release(void *queue)
{
	if (queue == NULL)
		return;

	rte_free(queue);
}

static void
bond_ethdev_slave_link_status_change_monitor(void *cb_arg)
{
	struct rte_eth_dev *bonded_ethdev, *slave_ethdev;
	struct bond_dev_private *internals;

	/* Default value for polling slave found is true as we don't want to
	 * disable the polling thread if we cannot get the lock */
	int i, polling_slave_found = 1;

	if (cb_arg == NULL)
		return;

	bonded_ethdev = (struct rte_eth_dev *)cb_arg;
	internals = (struct bond_dev_private *)bonded_ethdev->data->dev_private;

	if (!bonded_ethdev->data->dev_started ||
		!internals->link_status_polling_enabled)
		return;

	/* If device is currently being configured then don't check slaves link
	 * status, wait until next period */
	if (rte_spinlock_trylock(&internals->lock)) {
		if (internals->slave_count > 0)
			polling_slave_found = 0;

		for (i = 0; i < internals->slave_count; i++) {
			if (!internals->slaves[i].link_status_poll_enabled)
				continue;

			slave_ethdev = &rte_eth_devices[internals->slaves[i].port_id];
			polling_slave_found = 1;

			/* Update slave link status */
			(*slave_ethdev->dev_ops->link_update)(slave_ethdev,
					internals->slaves[i].link_status_wait_to_complete);

			/* if link status has changed since last checked then call lsc
			 * event callback */
			if (slave_ethdev->data->dev_link.link_status !=
					internals->slaves[i].last_link_status) {
				internals->slaves[i].last_link_status =
						slave_ethdev->data->dev_link.link_status;

				bond_ethdev_lsc_event_callback(internals->slaves[i].port_id,
						RTE_ETH_EVENT_INTR_LSC,
						&bonded_ethdev->data->port_id);
			}
		}
		rte_spinlock_unlock(&internals->lock);
	}

	if (polling_slave_found)
		/* Set alarm to continue monitoring link status of slave ethdev's */
		rte_eal_alarm_set(internals->link_status_polling_interval_ms * 1000,
				bond_ethdev_slave_link_status_change_monitor, cb_arg);
}

static int
bond_ethdev_link_update(struct rte_eth_dev *bonded_eth_dev,
		int wait_to_complete)
{
	struct bond_dev_private *internals = bonded_eth_dev->data->dev_private;

	if (!bonded_eth_dev->data->dev_started ||
		internals->active_slave_count == 0) {
		bonded_eth_dev->data->dev_link.link_status = 0;
		return 0;
	} else {
		struct rte_eth_dev *slave_eth_dev;
		int i, link_up = 0;

		for (i = 0; i < internals->active_slave_count; i++) {
			slave_eth_dev = &rte_eth_devices[internals->active_slaves[i]];

			(*slave_eth_dev->dev_ops->link_update)(slave_eth_dev,
					wait_to_complete);
			if (slave_eth_dev->data->dev_link.link_status == 1) {
				link_up = 1;
				break;
			}
		}

		bonded_eth_dev->data->dev_link.link_status = link_up;
	}

	return 0;
}

static void
bond_ethdev_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	struct bond_dev_private *internals = dev->data->dev_private;
	struct rte_eth_stats slave_stats;

	int i;

	/* clear bonded stats before populating from slaves */
	memset(stats, 0, sizeof(*stats));

	for (i = 0; i < internals->slave_count; i++) {
		rte_eth_stats_get(internals->slaves[i].port_id, &slave_stats);

		stats->ipackets += slave_stats.ipackets;
		stats->opackets += slave_stats.opackets;
		stats->ibytes += slave_stats.ibytes;
		stats->obytes += slave_stats.obytes;
		stats->ierrors += slave_stats.ierrors;
		stats->oerrors += slave_stats.oerrors;
		stats->imcasts += slave_stats.imcasts;
		stats->rx_nombuf += slave_stats.rx_nombuf;
		stats->fdirmatch += slave_stats.fdirmatch;
		stats->fdirmiss += slave_stats.fdirmiss;
		stats->tx_pause_xon += slave_stats.tx_pause_xon;
		stats->rx_pause_xon += slave_stats.rx_pause_xon;
		stats->tx_pause_xoff += slave_stats.tx_pause_xoff;
		stats->rx_pause_xoff += slave_stats.rx_pause_xoff;
	}
}

static void
bond_ethdev_stats_reset(struct rte_eth_dev *dev)
{
	struct bond_dev_private *internals = dev->data->dev_private;
	int i;

	for (i = 0; i < internals->slave_count; i++)
		rte_eth_stats_reset(internals->slaves[i].port_id);
}

static void
bond_ethdev_promiscuous_enable(struct rte_eth_dev *eth_dev)
{
	struct bond_dev_private *internals = eth_dev->data->dev_private;
	int i;

	internals->promiscuous_en = 1;

	switch (internals->mode) {
	/* Promiscuous mode is propagated to all slaves */
	case BONDING_MODE_ROUND_ROBIN:
	case BONDING_MODE_BALANCE:
#ifdef RTE_MBUF_REFCNT
	case BONDING_MODE_BROADCAST:
#endif
		for (i = 0; i < internals->slave_count; i++)
			rte_eth_promiscuous_enable(internals->slaves[i].port_id);
		break;
	/* In mode4 promiscus mode is managed when slave is added/removed */
	case BONDING_MODE_8023AD:
		break;
	/* Promiscuous mode is propagated only to primary slave */
	case BONDING_MODE_ACTIVE_BACKUP:
	case BONDING_MODE_ADAPTIVE_TRANSMIT_LOAD_BALANCING:
	default:
		rte_eth_promiscuous_enable(internals->current_primary_port);
	}
}

static void
bond_ethdev_promiscuous_disable(struct rte_eth_dev *dev)
{
	struct bond_dev_private *internals = dev->data->dev_private;
	int i;

	internals->promiscuous_en = 0;

	switch (internals->mode) {
	/* Promiscuous mode is propagated to all slaves */
	case BONDING_MODE_ROUND_ROBIN:
	case BONDING_MODE_BALANCE:
#ifdef RTE_MBUF_REFCNT
	case BONDING_MODE_BROADCAST:
#endif
		for (i = 0; i < internals->slave_count; i++)
			rte_eth_promiscuous_disable(internals->slaves[i].port_id);
		break;
	/* In mode4 promiscus mode is set managed when slave is added/removed */
	case BONDING_MODE_8023AD:
		break;
	/* Promiscuous mode is propagated only to primary slave */
	case BONDING_MODE_ACTIVE_BACKUP:
	case BONDING_MODE_ADAPTIVE_TRANSMIT_LOAD_BALANCING:
	default:
		rte_eth_promiscuous_disable(internals->current_primary_port);
	}
}

static void
bond_ethdev_delayed_lsc_propagation(void *arg)
{
	if (arg == NULL)
		return;

	_rte_eth_dev_callback_process((struct rte_eth_dev *)arg,
			RTE_ETH_EVENT_INTR_LSC);
}

void
bond_ethdev_lsc_event_callback(uint8_t port_id, enum rte_eth_event_type type,
		void *param)
{
	struct rte_eth_dev *bonded_eth_dev, *slave_eth_dev;
	struct bond_dev_private *internals;
	struct rte_eth_link link;

	int i, valid_slave = 0;
	uint8_t active_pos;
	uint8_t lsc_flag = 0;

	if (type != RTE_ETH_EVENT_INTR_LSC || param == NULL)
		return;

	bonded_eth_dev = &rte_eth_devices[*(uint8_t *)param];
	slave_eth_dev = &rte_eth_devices[port_id];

	if (valid_bonded_ethdev(bonded_eth_dev))
		return;

	internals = bonded_eth_dev->data->dev_private;

	/* If the device isn't started don't handle interrupts */
	if (!bonded_eth_dev->data->dev_started)
		return;

	/* verify that port_id is a valid slave of bonded port */
	for (i = 0; i < internals->slave_count; i++) {
		if (internals->slaves[i].port_id == port_id) {
			valid_slave = 1;
			break;
		}
	}

	if (!valid_slave)
		return;

	/* Search for port in active port list */
	active_pos = find_slave_by_id(internals->active_slaves,
			internals->active_slave_count, port_id);

	rte_eth_link_get_nowait(port_id, &link);
	if (link.link_status) {
		if (active_pos < internals->active_slave_count)
			return;

		/* if no active slave ports then set this port to be primary port */
		if (internals->active_slave_count < 1) {
			/* If first active slave, then change link status */
			bonded_eth_dev->data->dev_link.link_status = 1;
			internals->current_primary_port = port_id;
			lsc_flag = 1;

			mac_address_slaves_update(bonded_eth_dev);

			/* Inherit eth dev link properties from first active slave */
			link_properties_set(bonded_eth_dev,
					&(slave_eth_dev->data->dev_link));
		}

		activate_slave(bonded_eth_dev, port_id);

		/* If user has defined the primary port then default to using it */
		if (internals->user_defined_primary_port &&
				internals->primary_port == port_id)
			bond_ethdev_primary_set(internals, port_id);
	} else {
		if (active_pos == internals->active_slave_count)
			return;

		/* Remove from active slave list */
		deactivate_slave(bonded_eth_dev, port_id);

		/* No active slaves, change link status to down and reset other
		 * link properties */
		if (internals->active_slave_count < 1) {
			lsc_flag = 1;
			bonded_eth_dev->data->dev_link.link_status = 0;

			link_properties_reset(bonded_eth_dev);
		}

		/* Update primary id, take first active slave from list or if none
		 * available set to -1 */
		if (port_id == internals->current_primary_port) {
			if (internals->active_slave_count > 0)
				bond_ethdev_primary_set(internals,
						internals->active_slaves[0]);
			else
				internals->current_primary_port = internals->primary_port;
		}
	}

	if (lsc_flag) {
		/* Cancel any possible outstanding interrupts if delays are enabled */
		if (internals->link_up_delay_ms > 0 ||
			internals->link_down_delay_ms > 0)
			rte_eal_alarm_cancel(bond_ethdev_delayed_lsc_propagation,
					bonded_eth_dev);

		if (bonded_eth_dev->data->dev_link.link_status) {
			if (internals->link_up_delay_ms > 0)
				rte_eal_alarm_set(internals->link_up_delay_ms * 1000,
						bond_ethdev_delayed_lsc_propagation,
						(void *)bonded_eth_dev);
			else
				_rte_eth_dev_callback_process(bonded_eth_dev,
						RTE_ETH_EVENT_INTR_LSC);

		} else {
			if (internals->link_down_delay_ms > 0)
				rte_eal_alarm_set(internals->link_down_delay_ms * 1000,
						bond_ethdev_delayed_lsc_propagation,
						(void *)bonded_eth_dev);
			else
				_rte_eth_dev_callback_process(bonded_eth_dev,
						RTE_ETH_EVENT_INTR_LSC);
		}
	}
}

struct eth_dev_ops default_dev_ops = {
		.dev_start = bond_ethdev_start,
		.dev_stop = bond_ethdev_stop,
		.dev_close = bond_ethdev_close,
		.dev_configure = bond_ethdev_configure,
		.dev_infos_get = bond_ethdev_info,
		.rx_queue_setup = bond_ethdev_rx_queue_setup,
		.tx_queue_setup = bond_ethdev_tx_queue_setup,
		.rx_queue_release = bond_ethdev_rx_queue_release,
		.tx_queue_release = bond_ethdev_tx_queue_release,
		.link_update = bond_ethdev_link_update,
		.stats_get = bond_ethdev_stats_get,
		.stats_reset = bond_ethdev_stats_reset,
		.promiscuous_enable = bond_ethdev_promiscuous_enable,
		.promiscuous_disable = bond_ethdev_promiscuous_disable
};

static int
bond_init(const char *name, const char *params)
{
	struct bond_dev_private *internals;
	struct rte_kvargs *kvlist;
	uint8_t bonding_mode, socket_id;
	int  arg_count, port_id;

	RTE_LOG(INFO, EAL, "Initializing pmd_bond for %s\n", name);

	kvlist = rte_kvargs_parse(params, pmd_bond_init_valid_arguments);
	if (kvlist == NULL)
		return -1;

	/* Parse link bonding mode */
	if (rte_kvargs_count(kvlist, PMD_BOND_MODE_KVARG) == 1) {
		if (rte_kvargs_process(kvlist, PMD_BOND_MODE_KVARG,
				&bond_ethdev_parse_slave_mode_kvarg, &bonding_mode) != 0) {
			RTE_LOG(ERR, EAL, "Invalid mode for bonded device %s\n", name);
			return -1;
		}
	} else {
		RTE_LOG(ERR, EAL,
				"Mode must be specified only once for bonded device %s\n",
				name);
		return -1;
	}

	/* Parse socket id to create bonding device on */
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_SOCKET_ID_KVARG);
	if (arg_count == 1) {
		if (rte_kvargs_process(kvlist, PMD_BOND_SOCKET_ID_KVARG,
				&bond_ethdev_parse_socket_id_kvarg, &socket_id) != 0) {
			RTE_LOG(ERR, EAL,
					"Invalid socket Id specified for bonded device %s\n",
					name);
			return -1;
		}
	} else if (arg_count > 1) {
		RTE_LOG(ERR, EAL,
				"Socket Id can be specified only once for bonded device %s\n",
				name);
		return -1;
	} else {
		socket_id = rte_socket_id();
	}

	/* Create link bonding eth device */
	port_id = rte_eth_bond_create(name, bonding_mode, socket_id);
	if (port_id < 0) {
		RTE_LOG(ERR, EAL,
				"Failed to create socket %s in mode %u on socket %u.\n",
				name, bonding_mode, socket_id);
		return -1;
	}
	internals = rte_eth_devices[port_id].data->dev_private;
	internals->kvlist = kvlist;

	RTE_LOG(INFO, EAL,
			"Create bonded device %s on port %d in mode %u on socket %u.\n",
			name, port_id, bonding_mode, socket_id);
	return 0;
}

/* this part will resolve the slave portids after all the other pdev and vdev
 * have been allocated */
static int
bond_ethdev_configure(struct rte_eth_dev *dev)
{
	char *name = dev->data->name;
	struct bond_dev_private *internals = dev->data->dev_private;
	struct rte_kvargs *kvlist = internals->kvlist;
	int arg_count, port_id = dev - rte_eth_devices;

	/*
	 * if no kvlist, it means that this bonded device has been created
	 * through the bonding api.
	 */
	if (!kvlist)
		return 0;

	/* Parse MAC address for bonded device */
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_MAC_ADDR_KVARG);
	if (arg_count == 1) {
		struct ether_addr bond_mac;

		if (rte_kvargs_process(kvlist, PMD_BOND_MAC_ADDR_KVARG,
				&bond_ethdev_parse_bond_mac_addr_kvarg, &bond_mac) < 0) {
			RTE_LOG(INFO, EAL, "Invalid mac address for bonded device %s\n",
					name);
			return -1;
		}

		/* Set MAC address */
		if (rte_eth_bond_mac_address_set(port_id, &bond_mac) != 0) {
			RTE_LOG(ERR, EAL,
					"Failed to set mac address on bonded device %s\n",
					name);
			return -1;
		}
	} else if (arg_count > 1) {
		RTE_LOG(ERR, EAL,
				"MAC address can be specified only once for bonded device %s\n",
				name);
		return -1;
	}

	/* Parse/set balance mode transmit policy */
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_XMIT_POLICY_KVARG);
	if (arg_count == 1) {
		uint8_t xmit_policy;

		if (rte_kvargs_process(kvlist, PMD_BOND_XMIT_POLICY_KVARG,
				&bond_ethdev_parse_balance_xmit_policy_kvarg, &xmit_policy) !=
						0) {
			RTE_LOG(INFO, EAL,
					"Invalid xmit policy specified for bonded device %s\n",
					name);
			return -1;
		}

		/* Set balance mode transmit policy*/
		if (rte_eth_bond_xmit_policy_set(port_id, xmit_policy) != 0) {
			RTE_LOG(ERR, EAL,
					"Failed to set balance xmit policy on bonded device %s\n",
					name);
			return -1;
		}
	} else if (arg_count > 1) {
		RTE_LOG(ERR, EAL,
				"Transmit policy can be specified only once for bonded device"
				" %s\n", name);
		return -1;
	}

	/* Parse/add slave ports to bonded device */
	if (rte_kvargs_count(kvlist, PMD_BOND_SLAVE_PORT_KVARG) > 0) {
		struct bond_ethdev_slave_ports slave_ports;
		unsigned i;

		memset(&slave_ports, 0, sizeof(slave_ports));

		if (rte_kvargs_process(kvlist, PMD_BOND_SLAVE_PORT_KVARG,
				&bond_ethdev_parse_slave_port_kvarg, &slave_ports) != 0) {
			RTE_LOG(ERR, EAL,
					"Failed to parse slave ports for bonded device %s\n",
					name);
			return -1;
		}

		for (i = 0; i < slave_ports.slave_count; i++) {
			if (rte_eth_bond_slave_add(port_id, slave_ports.slaves[i]) != 0) {
				RTE_LOG(ERR, EAL,
						"Failed to add port %d as slave to bonded device %s\n",
						slave_ports.slaves[i], name);
			}
		}

	} else {
		RTE_LOG(INFO, EAL, "No slaves specified for bonded device %s\n", name);
		return -1;
	}

	/* Parse/set primary slave port id*/
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_PRIMARY_SLAVE_KVARG);
	if (arg_count == 1) {
		uint8_t primary_slave_port_id;

		if (rte_kvargs_process(kvlist,
				PMD_BOND_PRIMARY_SLAVE_KVARG,
				&bond_ethdev_parse_primary_slave_port_id_kvarg,
				&primary_slave_port_id) < 0) {
			RTE_LOG(INFO, EAL,
					"Invalid primary slave port id specified for bonded device"
					" %s\n", name);
			return -1;
		}

		/* Set balance mode transmit policy*/
		if (rte_eth_bond_primary_set(port_id, (uint8_t)primary_slave_port_id)
				!= 0) {
			RTE_LOG(ERR, EAL,
					"Failed to set primary slave port %d on bonded device %s\n",
					primary_slave_port_id, name);
			return -1;
		}
	} else if (arg_count > 1) {
		RTE_LOG(INFO, EAL,
				"Primary slave can be specified only once for bonded device"
				" %s\n", name);
		return -1;
	}

	/* Parse link status monitor polling interval */
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_LSC_POLL_PERIOD_KVARG);
	if (arg_count == 1) {
		uint32_t lsc_poll_interval_ms;

		if (rte_kvargs_process(kvlist,
				PMD_BOND_LSC_POLL_PERIOD_KVARG,
				&bond_ethdev_parse_time_ms_kvarg,
				&lsc_poll_interval_ms) < 0) {
			RTE_LOG(INFO, EAL,
					"Invalid lsc polling interval value specified for bonded"
					" device %s\n", name);
			return -1;
		}

		if (rte_eth_bond_link_monitoring_set(port_id, lsc_poll_interval_ms)
				!= 0) {
			RTE_LOG(ERR, EAL,
					"Failed to set lsc monitor polling interval (%u ms) on"
					" bonded device %s\n", lsc_poll_interval_ms, name);
			return -1;
		}
	} else if (arg_count > 1) {
		RTE_LOG(INFO, EAL,
				"LSC polling interval can be specified only once for bonded"
				" device %s\n", name);
		return -1;
	}

	/* Parse link up interrupt propagation delay */
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_LINK_UP_PROP_DELAY_KVARG);
	if (arg_count == 1) {
		uint32_t link_up_delay_ms;

		if (rte_kvargs_process(kvlist,
				PMD_BOND_LINK_UP_PROP_DELAY_KVARG,
				&bond_ethdev_parse_time_ms_kvarg,
				&link_up_delay_ms) < 0) {
			RTE_LOG(INFO, EAL,
					"Invalid link up propagation delay value specified for"
					" bonded device %s\n", name);
			return -1;
		}

		/* Set balance mode transmit policy*/
		if (rte_eth_bond_link_up_prop_delay_set(port_id, link_up_delay_ms)
				!= 0) {
			RTE_LOG(ERR, EAL,
					"Failed to set link up propagation delay (%u ms) on bonded"
					" device %s\n", link_up_delay_ms, name);
			return -1;
		}
	} else if (arg_count > 1) {
		RTE_LOG(INFO, EAL,
				"Link up propagation delay can be specified only once for"
				" bonded device %s\n", name);
		return -1;
	}

	/* Parse link down interrupt propagation delay */
	arg_count = rte_kvargs_count(kvlist, PMD_BOND_LINK_DOWN_PROP_DELAY_KVARG);
	if (arg_count == 1) {
		uint32_t link_down_delay_ms;

		if (rte_kvargs_process(kvlist,
				PMD_BOND_LINK_DOWN_PROP_DELAY_KVARG,
				&bond_ethdev_parse_time_ms_kvarg,
				&link_down_delay_ms) < 0) {
			RTE_LOG(INFO, EAL,
					"Invalid link down propagation delay value specified for"
					" bonded device %s\n", name);
			return -1;
		}

		/* Set balance mode transmit policy*/
		if (rte_eth_bond_link_down_prop_delay_set(port_id, link_down_delay_ms)
				!= 0) {
			RTE_LOG(ERR, EAL,
					"Failed to set link down propagation delay (%u ms) on"
					" bonded device %s\n", link_down_delay_ms, name);
			return -1;
		}
	} else if (arg_count > 1) {
		RTE_LOG(INFO, EAL,
				"Link down propagation delay can be specified only once for"
				" bonded device %s\n", name);
		return -1;
	}

	return 0;
}

static struct rte_driver bond_drv = {
	.name = "eth_bond",
	.type = PMD_VDEV,
	.init = bond_init,
};

PMD_REGISTER_DRIVER(bond_drv);

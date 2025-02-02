/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2016 Intel Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <inttypes.h>
#include <uk/alloc.h>

#include <e1000/e1000_api.h>
#include <e1000/e1000_ethdev.h>
#include <e1000/e1000_osdep.h>

#define	E1000_TXD_VLAN_SHIFT	16

#define E1000_RXDCTL_GRAN	0x01000000 /* RXDCTL Granularity */
#define RTE_CACHE_LINE_SIZE 128
#define E1000_TX_OFFLOAD_MASK ( \
		PKT_TX_IPV6 |           \
		PKT_TX_IPV4 |           \
		PKT_TX_IP_CKSUM |       \
		PKT_TX_L4_MASK |        \
		PKT_TX_VLAN_PKT)

/**
 * Define max possible fragments for the network packets.
 */
#define NET_MAX_FRAGMENTS    ((__U16_MAX >> __PAGE_SHIFT) + 2)


#define E1000_TX_OFFLOAD_NOTSUP_MASK \
		(PKT_TX_OFFLOAD_MASK ^ E1000_TX_OFFLOAD_MASK)


#define to_e1000dev(ndev) \
	__containerof(ndev, struct e1000_hw, netdev)

/**
 * Structure associated with each descriptor of the RX ring of a RX queue.
 */
struct em_rx_entry {
	struct uk_netbuf *mbuf; /**< mbuf associated with RX descriptor. */
};

/**
 * Structure associated with each descriptor of the TX ring of a TX queue.
 */
struct em_tx_entry {
	struct uk_netbuf *mbuf; /**< mbuf associated with TX desc, if any. */
	uint16_t next_id; /**< Index of next descriptor in ring. */
	uint16_t last_id; /**< Index of last scattered descriptor. */
};

/**
 * Structure associated with each RX queue.
 */
struct em_rx_queue {
	struct uk_alloc *a;
	volatile struct e1000_rx_desc *rx_ring; /**< RX ring virtual address. */
	uint64_t            rx_ring_phys_addr; /**< RX ring DMA address. */
	volatile uint32_t   *rdt_reg_addr; /**< RDT register address. */
	volatile uint32_t   *rdh_reg_addr; /**< RDH register address. */
	struct em_rx_entry *sw_ring;   /**< address of RX software ring. */
	struct uk_netbuf *pkt_first_seg; /**< First segment of current packet. */
	struct uk_netbuf *pkt_last_seg;  /**< Last segment of current packet. */
	/* User-provided receive buffer allocation function */
	uk_netdev_alloc_rxpkts alloc_rxpkts;
	void *alloc_rxpkts_argp;
	uint64_t	    offloads;   /**< Offloads of DEV_RX_OFFLOAD_* */
	uint16_t            nb_rx_desc; /**< number of RX descriptors. */
	uint16_t            rx_tail;    /**< current value of RDT register. */
	uint16_t            nb_rx_hold; /**< number of held free RX desc. */
	uint16_t            rx_free_thresh; /**< max free RX desc to hold. */
	uint16_t            queue_id;   /**< RX queue index. */
	uint16_t            port_id;    /**< Device port identifier. */
	uint8_t             pthresh;    /**< Prefetch threshold register. */
	uint8_t             hthresh;    /**< Host threshold register. */
	uint8_t             wthresh;    /**< Write-back threshold register. */
	uint8_t             crc_len;    /**< 0 if CRC stripped, 4 otherwise. */
};
typedef struct em_rx_queue uk_netdev_rx_queue;

/**
 * Hardware context number
 */
enum {
	EM_CTX_0    = 0, /**< CTX0 */
	EM_CTX_NUM  = 1, /**< CTX NUM */
};

/** Offload features */
union em_vlan_macip {
	uint32_t data;
	struct {
		uint16_t l3_len:9; /**< L3 (IP) Header Length. */
		uint16_t l2_len:7; /**< L2 (MAC) Header Length. */
		uint16_t vlan_tci;
		/**< VLAN Tag Control Identifier (CPU order). */
	} f;
};

/*
 * Compare mask for vlan_macip_len.data,
 * should be in sync with em_vlan_macip.f layout.
 * */
#define TX_VLAN_CMP_MASK        0xFFFF0000  /**< VLAN length - 16-bits. */
#define TX_MAC_LEN_CMP_MASK     0x0000FE00  /**< MAC length - 7-bits. */
#define TX_IP_LEN_CMP_MASK      0x000001FF  /**< IP  length - 9-bits. */
/** MAC+IP  length. */
#define TX_MACIP_LEN_CMP_MASK   (TX_MAC_LEN_CMP_MASK | TX_IP_LEN_CMP_MASK)

/**
 * Structure to check if new context need be built
 */
struct em_ctx_info {
	uint64_t flags;              /**< ol_flags related to context build. */
	uint32_t cmp_mask;           /**< compare mask */
	union em_vlan_macip hdrlen;  /**< L2 and L3 header lenghts */
};

/**
 * Structure associated with each TX queue.
 */
struct em_tx_queue {
	volatile struct e1000_data_desc *tx_ring; /**< TX ring address */
	uint64_t               tx_ring_phys_addr; /**< TX ring DMA address. */
	struct em_tx_entry    *sw_ring; /**< virtual address of SW ring. */
	volatile uint32_t      *tdt_reg_addr; /**< Address of TDT register. */
	uint16_t               nb_tx_desc;    /**< number of TX descriptors. */
	uint16_t               tx_tail;  /**< Current value of TDT register. */
	/**< Start freeing TX buffers if there are less free descriptors than
	     this value. */
	uint16_t               tx_free_thresh;
	/**< Number of TX descriptors to use before RS bit is set. */
	uint16_t               tx_rs_thresh;
	/** Number of TX descriptors used since RS bit was set. */
	uint16_t               nb_tx_used;
	/** Index to last TX descriptor to have been cleaned. */
	uint16_t	       last_desc_cleaned;
	/** Total number of TX descriptors ready to be allocated. */
	uint16_t               nb_tx_free;
	uint16_t               queue_id; /**< TX queue index. */
	uint16_t               port_id;  /**< Device port identifier. */
	uint8_t                pthresh;  /**< Prefetch threshold register. */
	uint8_t                hthresh;  /**< Host threshold register. */
	uint8_t                wthresh;  /**< Write-back threshold register. */
	struct em_ctx_info ctx_cache;
	/**< Hardware context history.*/
	uint64_t	       offloads; /**< offloads of DEV_TX_OFFLOAD_* */
	struct e1000_hw		*hw;
};

typedef struct em_tx_queue uk_netdev_tx_queue;

#if 1
#define RTE_PMD_USE_PREFETCH
#endif

#ifdef RTE_PMD_USE_PREFETCH
#define rte_em_prefetch(p)	rte_prefetch0(p)
#else
#define rte_em_prefetch(p)	do {} while(0)
#endif

#ifdef RTE_PMD_PACKET_PREFETCH
#define rte_packet_prefetch(p) rte_prefetch1(p)
#else
#define rte_packet_prefetch(p)	do {} while(0)
#endif

#ifndef DEFAULT_TX_FREE_THRESH
#define DEFAULT_TX_FREE_THRESH  16
#endif /* DEFAULT_TX_FREE_THRESH */

#ifndef DEFAULT_TX_RS_THRESH
#define DEFAULT_TX_RS_THRESH  16
#endif /* DEFAULT_TX_RS_THRESH */

// /* Reset transmit descriptors after they have been used */
static inline int
em_xmit_cleanup(struct em_tx_queue *txq)
{
	struct em_tx_entry *sw_ring = txq->sw_ring;
	volatile struct e1000_data_desc *txr = txq->tx_ring;
	uint16_t last_desc_cleaned = txq->last_desc_cleaned;
	uint16_t nb_tx_desc = txq->nb_tx_desc;
	uint16_t desc_to_clean_to;
	uint16_t nb_tx_to_clean;

	/* Determine the last descriptor needing to be cleaned */
	desc_to_clean_to = (uint16_t)(last_desc_cleaned + txq->tx_rs_thresh);
	if (desc_to_clean_to >= nb_tx_desc)
		desc_to_clean_to = (uint16_t)(desc_to_clean_to - nb_tx_desc);

	/* Check to make sure the last descriptor to clean is done */
	desc_to_clean_to = sw_ring[desc_to_clean_to].last_id;
	if (! (txr[desc_to_clean_to].upper.fields.status & E1000_TXD_STAT_DD))
	{
		/* Failed to clean any descriptors, better luck next time */
		return -(1);
	}

	/* Figure out how many descriptors will be cleaned */
	if (last_desc_cleaned > desc_to_clean_to)
		nb_tx_to_clean = (uint16_t)((nb_tx_desc - last_desc_cleaned) +
							desc_to_clean_to);
	else
		nb_tx_to_clean = (uint16_t)(desc_to_clean_to -
						last_desc_cleaned);

	/*
	 * The last descriptor to clean is done, so that means all the
	 * descriptors from the last descriptor that was cleaned
	 * up to the last descriptor with the RS bit set
	 * are done. Only reset the threshold descriptor.
	 */
	txr[desc_to_clean_to].upper.fields.status = 0;

	/* Update the txq to reflect the last descriptor that was cleaned */
	txq->last_desc_cleaned = desc_to_clean_to;
	txq->nb_tx_free = (uint16_t)(txq->nb_tx_free + nb_tx_to_clean);

	/* No Error */
	return 0;
}

int eth_em_xmit_pkts(__unused struct uk_netdev *dev,
	struct uk_netdev_tx_queue *queue,
	struct uk_netbuf *pkt)
{
	struct em_tx_queue *txq;
	struct em_tx_entry *sw_ring;
	struct em_tx_entry *txe, *txn;
	volatile struct e1000_data_desc *txr;
	volatile struct e1000_data_desc *txd;
	struct uk_netbuf     *tx_pkt;
	struct uk_netbuf     *m_seg;
	uint64_t buf_dma_addr;
	uint32_t popts_spec;
	uint32_t cmd_type_len;
	uint16_t slen;
	uint16_t tx_id;
	uint16_t tx_last;
	uint16_t nb_tx;
	uint16_t nb_used;
	uint32_t new_ctx;

	txq = (struct em_tx_queue *) queue;
	sw_ring = txq->sw_ring;
	txr     = txq->tx_ring;
	tx_id   = txq->tx_tail;
	txe = &sw_ring[tx_id];

	/* Determine if the descriptor ring needs to be cleaned. */
	 if (txq->nb_tx_free < txq->tx_free_thresh) {
		em_xmit_cleanup(txq);
	 }

	for (nb_tx = 0; nb_tx < 1; nb_tx++) {
		new_ctx = 0;
		tx_pkt = pkt;

		/*
		 * Keep track of how many descriptors are used this loop
		 * This will always be the number of segments + the number of
		 * Context descriptors required to transmit the packet
		 */
		nb_used = (uint16_t)(1 + new_ctx);

		/*
		 * The number of descriptors that must be allocated for a
		 * packet is the number of segments of that packet, plus 1
		 * Context Descriptor for the hardware offload, if any.
		 * Determine the last TX descriptor to allocate in the TX ring
		 * for the packet, starting from the current position (tx_id)
		 * in the ring.
		 */
		tx_last = (uint16_t) (tx_id + nb_used - 1);

		/* Circular ring */
		if (tx_last >= txq->nb_tx_desc)
			tx_last = (uint16_t) (tx_last - txq->nb_tx_desc);

		/*
		 * Make sure there are enough TX descriptors available to
		 * transmit the entire packet.
		 * nb_used better be less than or equal to txq->tx_rs_thresh
		 */
		while (unlikely (nb_used > txq->nb_tx_free)) {
			if (em_xmit_cleanup(txq) != 0) {
				/* Could not clean any descriptors */
				if (nb_tx == 0)
					return 0;
				goto end_of_tx;
			}
		}

		/*
		 * By now there are enough free TX descriptors to transmit
		 * the packet.
		 */

		/*
		 * Set common flags of all TX Data Descriptors.
		 *
		 * The following bits must be set in all Data Descriptors:
		 *    - E1000_TXD_DTYP_DATA
		 *    - E1000_TXD_DTYP_DEXT
		 *
		 * The following bits must be set in the first Data Descriptor
		 * and are ignored in the other ones:
		 *    - E1000_TXD_POPTS_IXSM
		 *    - E1000_TXD_POPTS_TXSM
		 *
		 * The following bits must be set in the last Data Descriptor
		 * and are ignored in the other ones:
		 *    - E1000_TXD_CMD_VLE
		 *    - E1000_TXD_CMD_IFCS
		 *
		 * The following bits must only be set in the last Data
		 * Descriptor:
		 *   - E1000_TXD_CMD_EOP
		 *
		 * The following bits can be set in any Data Descriptor, but
		 * are only set in the last Data Descriptor:
		 *   - E1000_TXD_CMD_RS
		 */
		cmd_type_len = E1000_TXD_CMD_DEXT | E1000_TXD_DTYP_D |
			E1000_TXD_CMD_IFCS;
		popts_spec = 0;

		m_seg = tx_pkt;
		do {
			txd = &txr[tx_id];
			txn = &sw_ring[txe->next_id];

			if (txe->mbuf != NULL) {
				uk_netbuf_free_single(txe->mbuf);
			}
			txe->mbuf = m_seg;

			/*
			 * Set up Transmit Data Descriptor.
			 */
			slen = m_seg->buflen;
			buf_dma_addr = (uint64_t) m_seg->buf;

			txd->buffer_addr = buf_dma_addr;
			txd->lower.data = cmd_type_len | slen;
			txd->upper.data = popts_spec;

			txe->last_id = tx_last;
			tx_id = txe->next_id;
			txe = txn;
			m_seg = m_seg->next;
		} while (m_seg != NULL);

		/*
		 * The last packet data descriptor needs End Of Packet (EOP)
		 */
		cmd_type_len |= E1000_TXD_CMD_EOP;
		txq->nb_tx_used = (uint16_t)(txq->nb_tx_used + nb_used);
		txq->nb_tx_free = (uint16_t)(txq->nb_tx_free - nb_used);

		/* Set RS bit only on threshold packets' last descriptor */
		if (txq->nb_tx_used >= txq->tx_rs_thresh) {
			cmd_type_len |= E1000_TXD_CMD_RS;
			/* Update txq RS bit counters */
			txq->nb_tx_used = 0;
		}
		txd->lower.data |= cmd_type_len;
	}
end_of_tx:
	wmb();

	/*
	 * Set the Transmit Descriptor Tail (TDT)
	 */
	E1000_PCI_REG_WRITE_RELAXED(txq->tdt_reg_addr, tx_id);
	txq->tx_tail = tx_id;

	return UK_NETDEV_STATUS_SUCCESS;

}

int
eth_em_recv_pkts(__unused struct uk_netdev *dev, struct uk_netdev_rx_queue *rx_queue, 
	struct uk_netbuf **pkt)
{
	volatile struct e1000_rx_desc *rx_ring;
	volatile struct e1000_rx_desc *rxdp;
	struct em_rx_entry *sw_ring;
	struct em_rx_entry *rxe;
	struct em_rx_queue *rxq;
	struct uk_netbuf *rxm;
	struct uk_netbuf *nmb;
	struct e1000_rx_desc rxd;
	uint64_t dma_addr;
	uint16_t pkt_len;
	uint16_t rx_id;
	uint16_t nb_rx;
	uint16_t nb_hold;
	uint8_t status;
	int ret_status = 0;

	rxq = (struct em_rx_queue *) rx_queue;
	nb_rx = 0;
	nb_hold = 0;
	rx_id = rxq->rx_tail;
	rx_ring = rxq->rx_ring;
	sw_ring = rxq->sw_ring;
	while (nb_rx < 1) {
		/*
		 * The order of operations here is important as the DD status
		 * bit must not be read after any other descriptor fields.
		 * rx_ring and rxdp are pointing to volatile data so the order
		 * of accesses cannot be reordered by the compiler. If they were
		 * not volatile, they could be reordered which could lead to
		 * using invalid descriptor fields when read from rxd.
		 */
		rxdp = &rx_ring[rx_id];
		status = rxdp->status;
		if (! (status & E1000_RXD_STAT_DD)) {
			return 0;
		}
		rxd = *rxdp;

		/*
		 * End of packet.
		 *
		 * If the E1000_RXD_STAT_EOP flag is not set, the RX packet is
		 * likely to be invalid and to be dropped by the various
		 * validation checks performed by the network stack.
		 *
		 * Allocate a new mbuf to replenish the RX ring descriptor.
		 * If the allocation fails:
		 *    - arrange for that RX descriptor to be the first one
		 *      being parsed the next time the receive function is
		 *      invoked [on the same queue].
		 *
		 *    - Stop parsing the RX ring and return immediately.
		 *
		 * This policy do not drop the packet received in the RX
		 * descriptor for which the allocation of a new mbuf failed.
		 * Thus, it allows that packet to be later retrieved if
		 * mbuf have been freed in the mean time.
		 * As a side effect, holding RX descriptors instead of
		 * systematically giving them back to the NIC may lead to
		 * RX ring exhaustion situations.
		 * However, the NIC can gracefully prevent such situations
		 * to happen by sending specific "back-pressure" flow control
		 * frames to its peer(s).
		 */

		struct uk_netbuf *mbufs[1];
		rxq->alloc_rxpkts(rxq->alloc_rxpkts_argp, mbufs, 1);
		nmb = mbufs[0];
		if (nmb == NULL) {
			break;
		}

		nb_hold++;
		rxe = &sw_ring[rx_id];
		rx_id++;
		if (rx_id == rxq->nb_rx_desc)
			rx_id = 0;

		/* Rearm RXD: attach new mbuf and reset status to zero. */
		rxm = rxe->mbuf;
		rxe->mbuf = nmb;
		dma_addr = (uint64_t) nmb->buf;

		rxdp->buffer_addr = dma_addr;
		rxdp->status = 0;

		/*
		 * Initialize the returned mbuf.
		 * 1) setup generic mbuf fields:
		 *    - number of segments,
		 *    - next segment,
		 *    - packet length,
		 *    - RX port identifier.
		 * 2) integrate hardware offload data, if any:
		 *    - RSS flag & hash,
		 *    - IP checksum flag,
		 *    - VLAN TCI, if any,
		 *    - error flags.
		 */
		pkt_len = (uint16_t) rxd.length - rxq->crc_len;
		rxm->buflen = pkt_len;
		rxm->len = pkt_len;
		rxm->next = NULL;
		ret_status |= UK_NETDEV_STATUS_SUCCESS;

		/*
		 * Store the mbuf address into the next entry of the array
		 * of returned packets.
		 */
		*pkt = rxm;
		nb_rx++;
	}
	rxq->rx_tail = rx_id;

	/*
	 * If the number of free RX descriptors is greater than the RX free
	 * threshold of the queue, advance the Receive Descriptor Tail (RDT)
	 * register.
	 * Update the RDT with the value of the last processed RX descriptor
	 * minus 1, to guarantee that the RDT register is never equal to the
	 * RDH register, which creates a "full" ring situtation from the
	 * hardware point of view...
	 */
	nb_hold = (uint16_t) (nb_hold + rxq->nb_rx_hold);
	if (nb_hold > rxq->rx_free_thresh) {
		rx_id = (uint16_t) ((rx_id == 0) ?
			(rxq->nb_rx_desc - 1) : (rx_id - 1));
		E1000_PCI_REG_WRITE(rxq->rdt_reg_addr, rx_id);
		nb_hold = 0;
	}
	rxq->nb_rx_hold = nb_hold;
	ret_status |= UK_NETDEV_STATUS_MORE;
	return ret_status;
}

#define	EM_MAX_BUF_SIZE     16384
#define EM_RCTL_FLXBUF_STEP 1024

static void
em_tx_queue_release_mbufs(struct em_tx_queue *txq)
{
	unsigned i;

	if (txq->sw_ring != NULL) {
		for (i = 0; i != txq->nb_tx_desc; i++) {
			if (txq->sw_ring[i].mbuf != NULL) {
				uk_netbuf_free_single(txq->sw_ring[i].mbuf);
				txq->sw_ring[i].mbuf = NULL;
			}
		}
	}
}

static void
em_tx_queue_release(struct em_tx_queue *txq)
{
	if (txq != NULL) {
		em_tx_queue_release_mbufs(txq);
	}
}

/* (Re)set dynamic em_tx_queue fields to defaults */
static void
em_reset_tx_queue(struct em_tx_queue *txq)
{
	uint16_t i, nb_desc, prev;
	static const struct e1000_data_desc txd_init = {
		.upper.fields = {.status = E1000_TXD_STAT_DD},
	};

	nb_desc = txq->nb_tx_desc;

	/* Initialize ring entries */

	prev = (uint16_t) (nb_desc - 1);

	for (i = 0; i < nb_desc; i++) {
		txq->tx_ring[i] = txd_init;
		txq->sw_ring[i].mbuf = NULL;
		txq->sw_ring[i].last_id = i;
		txq->sw_ring[prev].next_id = i;
		prev = i;
	}

	/*
	 * Always allow 1 descriptor to be un-allocated to avoid
	 * a H/W race condition
	 */
	txq->nb_tx_free = (uint16_t)(nb_desc - 1);
	txq->last_desc_cleaned = (uint16_t)(nb_desc - 1);
	txq->nb_tx_used = 0;
	txq->tx_tail = 0;

	memset((void*)&txq->ctx_cache, 0, sizeof (txq->ctx_cache));
}


struct uk_netdev_tx_queue *
eth_em_tx_queue_setup(struct uk_netdev *dev,
			 uint16_t queue_idx,
			 uint16_t nb_desc,
			 __unused struct uk_netdev_txqueue_conf *tx_conf)
{
	void *mem;
	struct em_tx_queue *txq;
	struct e1000_hw     *hw;
	uint16_t tx_rs_thresh, tx_free_thresh;
	
	hw = to_e1000dev(dev);

	/*
	 * Validate number of transmit descriptors.
	 * It must not exceed hardware maximum, and must be multiple
	 * of E1000_ALIGN.
	 */
	nb_desc = 32;
	if (nb_desc % EM_TXD_ALIGN != 0 ||
			(nb_desc > E1000_MAX_RING_DESC) ||
			(nb_desc < E1000_MIN_RING_DESC)) {
		return NULL;
	}

	tx_free_thresh = DEFAULT_TX_FREE_THRESH;
	tx_rs_thresh = DEFAULT_TX_RS_THRESH;

	if (tx_free_thresh >= (nb_desc - 3)) {
		uk_pr_err("tx_free_thresh must be less than the "
			     "number of TX descriptors minus 3. "
			     "(tx_free_thresh=%u queue=%d)",
			     (unsigned int)tx_free_thresh, (int)queue_idx);
		return NULL;
	}
	if (tx_rs_thresh > tx_free_thresh) {
		uk_pr_err("tx_rs_thresh must be less than or equal to "
			     "tx_free_thresh. (tx_free_thresh=%u "
			     "tx_rs_thresh=%u queue=%d)",
			     (unsigned int)tx_free_thresh,
			     (unsigned int)tx_rs_thresh,
			     (int)queue_idx);
		return NULL;
	}

	/* Free memory prior to re-allocation if needed... */
	if (dev->_tx_queue[queue_idx] != NULL) {
		em_tx_queue_release((struct em_tx_queue *)dev->_tx_queue[queue_idx]);
		dev->_tx_queue[queue_idx] = NULL;
	}

	/*
	 * Allocate TX ring hardware descriptors. A memzone large enough to
	 * handle the maximum ring size is allocated in order to allow for
	 * resizing in later calls to the queue setup function.
	 */
	mem = uk_memalign(hw->a, 16, E1000_MAX_RING_DESC * sizeof(txq->tx_ring[0]));
	if (mem == NULL) {
		return NULL;
	}

	/* Allocate the tx queue data structure. */
	if ((txq = uk_calloc(hw->a, 1, sizeof(*txq))) == NULL) {
		return NULL;
	}

	// /* Allocate software ring */
	if ((txq->sw_ring = uk_calloc(hw->a, nb_desc,
			sizeof(txq->sw_ring[0]))) == NULL) {
		em_tx_queue_release(txq);
		return NULL;
	}

	txq->nb_tx_desc = nb_desc;
	txq->tx_free_thresh = tx_free_thresh;
	txq->tx_rs_thresh = tx_rs_thresh;
	txq->pthresh = 4;
	txq->hthresh = 0;
	txq->wthresh = 0;
	txq->queue_id = queue_idx;
	txq->hw = hw;

	txq->tdt_reg_addr = E1000_PCI_REG_ADDR(hw, E1000_TDT(queue_idx));
	txq->tx_ring_phys_addr = (uint64_t) mem;
	txq->tx_ring = (struct e1000_data_desc *) mem;

	em_reset_tx_queue(txq);

	dev->_tx_queue[queue_idx] = (struct uk_netdev_tx_queue *) txq;
	hw->nb_tx_queues++;

	return (struct uk_netdev_tx_queue *) txq;
}

static void
em_rx_queue_release_mbufs(struct em_rx_queue *rxq)
{
	unsigned i;

	debug_uk_pr_info("em_rx_queue_release_mbufs\n");
	if (rxq->sw_ring != NULL) {
		for (i = 0; i != rxq->nb_rx_desc; i++) {
			if (rxq->sw_ring[i].mbuf != NULL) {
				uk_netbuf_free_single(rxq->sw_ring[i].mbuf);
				rxq->sw_ring[i].mbuf = NULL;
			}
		}
	}
}

static void
em_rx_queue_release(struct em_rx_queue *rxq)
{
	debug_uk_pr_info("em_rx_queue_release\n");

	if (rxq != NULL) {
		em_rx_queue_release_mbufs(rxq);
	}
}

// /* Reset dynamic em_rx_queue fields back to defaults */
static void
em_reset_rx_queue(struct em_rx_queue *rxq)
{
	rxq->rx_tail = 0;
	rxq->nb_rx_hold = 0;
	rxq->pkt_first_seg = NULL;
	rxq->pkt_last_seg = NULL;
}

struct uk_netdev_rx_queue *
eth_em_rx_queue_setup(struct uk_netdev *dev,
		uint16_t queue_idx,
		uint16_t nb_desc,
		struct uk_netdev_rxqueue_conf *rx_conf)
{
	UK_ASSERT(rx_conf->alloc_rxpkts);

	void *mem;
	struct em_rx_queue *rxq;
	struct e1000_hw     *hw;

	hw = to_e1000dev(dev);

	/*
	 * Validate number of receive descriptors.
	 * It must not exceed hardware maximum, and must be multiple
	 * of E1000_ALIGN.
	 */
	nb_desc = 32;
	if (nb_desc % EM_RXD_ALIGN != 0 ||
			(nb_desc > E1000_MAX_RING_DESC) ||
			(nb_desc < E1000_MIN_RING_DESC)) {
		uk_pr_err("Invalid number of received descriptors %d\n", nb_desc);
		return NULL;
	}

	/* Free memory prior to re-allocation if needed. */
	if (dev->_rx_queue[queue_idx] != NULL) {
		em_rx_queue_release((struct em_rx_queue *)dev->_rx_queue[queue_idx]);
		debug_uk_pr_info("Setting dev->_rx_queue[queue_idx] to null\n");
		dev->_rx_queue[queue_idx] = NULL;
	}

	/* Allocate RX ring for max possible mumber of hardware descriptors. */
	// mem = uk_calloc(hw->a, E1000_MAX_RING_DESC, sizeof(rxq->rx_ring[0]));
	// mem = uk_malloc(hw->a, ALIGN_UP(E1000_MAX_RING_DESC * sizeof(rxq->rx_ring[0]), 16));
	mem = uk_memalign(hw->a, 16, E1000_MAX_RING_DESC * sizeof(rxq->rx_ring[0]));
	if (mem == NULL) {
		uk_pr_err("Failed to allocate RX ring\n");
		return NULL;
	}

	/* Allocate the RX queue data structure. */
	if ((rxq = uk_calloc(hw->a, 1, sizeof(*rxq))) == NULL) {
		uk_pr_err("Failed to allocate RX queue data\n");
		return NULL;
	}

	/* Allocate software ring. */
	if ((rxq->sw_ring = uk_calloc(hw->a, nb_desc, sizeof (rxq->sw_ring[0]))) == NULL) {
		uk_pr_err("Failed to allocate software ring\n");
		em_rx_queue_release(rxq);
		return NULL;
	}

	rxq->nb_rx_desc = nb_desc;
	rxq->pthresh = 8;
	rxq->hthresh = 8;
	rxq->wthresh = 36;
	rxq->rx_free_thresh = 0;
	rxq->queue_id = queue_idx;
	rxq->crc_len = 0;
	rxq->a = hw->a;

	rxq->rdt_reg_addr = E1000_PCI_REG_ADDR(hw, E1000_RDT(queue_idx));
	rxq->rdh_reg_addr = E1000_PCI_REG_ADDR(hw, E1000_RDH(queue_idx));
	rxq->rx_ring_phys_addr = (uint64_t) mem;
	rxq->rx_ring = (struct e1000_rx_desc *) mem;

	dev->_rx_queue[queue_idx] = (struct uk_netdev_rx_queue *) rxq;
	hw->nb_rx_queues++;
	em_reset_rx_queue(rxq);

	rxq->alloc_rxpkts = rx_conf->alloc_rxpkts;
	rxq->alloc_rxpkts_argp = rx_conf->alloc_rxpkts_argp;

	return (struct uk_netdev_rx_queue *)rxq;
}

void
em_dev_clear_queues(struct uk_netdev *dev)
{
	uint16_t i;
	struct em_tx_queue *txq;
	struct em_rx_queue *rxq;
	struct e1000_hw *hw;

	hw = to_e1000dev(dev);

	for (i = 0; i < hw->nb_tx_queues; i++) {
		txq = (struct em_tx_queue *)dev->_tx_queue[i];
		if (txq != NULL) {
			em_tx_queue_release_mbufs(txq);
			em_reset_tx_queue(txq);
		}
	}

	for (i = 0; i < hw->nb_rx_queues; i++) {
		rxq = (struct em_rx_queue *)dev->_rx_queue[i];
		if (rxq != NULL) {
			em_rx_queue_release_mbufs(rxq);
			em_reset_rx_queue(rxq);
		}
	}
}

/*
 * Takes as input/output parameter RX buffer size.
 * Returns (BSIZE | BSEX | FLXBUF) fields of RCTL register.
 */
static uint32_t
em_rctl_bsize(__unused enum e1000_mac_type hwtyp, uint32_t *bufsz)
{
	/*
	 * For BSIZE & BSEX all configurable sizes are:
	 * 16384: rctl |= (E1000_RCTL_SZ_16384 | E1000_RCTL_BSEX);
	 *  8192: rctl |= (E1000_RCTL_SZ_8192  | E1000_RCTL_BSEX);
	 *  4096: rctl |= (E1000_RCTL_SZ_4096  | E1000_RCTL_BSEX);
	 *  2048: rctl |= E1000_RCTL_SZ_2048;
	 *  1024: rctl |= E1000_RCTL_SZ_1024;
	 *   512: rctl |= E1000_RCTL_SZ_512;
	 *   256: rctl |= E1000_RCTL_SZ_256;
	 */
	static const struct {
		uint32_t bufsz;
		uint32_t rctl;
	} bufsz_to_rctl[] = {
		{16384, (E1000_RCTL_SZ_16384 | E1000_RCTL_BSEX)},
		{8192,  (E1000_RCTL_SZ_8192  | E1000_RCTL_BSEX)},
		{4096,  (E1000_RCTL_SZ_4096  | E1000_RCTL_BSEX)},
		{2048,  E1000_RCTL_SZ_2048},
		{1024,  E1000_RCTL_SZ_1024},
		{512,   E1000_RCTL_SZ_512},
		{256,   E1000_RCTL_SZ_256},
	};

	int i;
	uint32_t rctl_bsize;

	debug_uk_pr_info("em_rctl_bsize\n");

	rctl_bsize = *bufsz;

	/*
	 * Starting from 82571 it is possible to specify RX buffer size
	 * by RCTL.FLXBUF. When this field is different from zero, the
	 * RX buffer size = RCTL.FLXBUF * 1K
	 * (e.g. t is possible to specify RX buffer size  1,2,...,15KB).
	 * It is working ok on real HW, but by some reason doesn't work
	 * on VMware emulated 82574L.
	 * So for now, always use BSIZE/BSEX to setup RX buffer size.
	 * If you don't plan to use it on VMware emulated 82574L and
	 * would like to specify RX buffer size in 1K granularity,
	 * uncomment the following lines:
	 * ***************************************************************
	 * if (hwtyp >= e1000_82571 && hwtyp <= e1000_82574 &&
	 *		rctl_bsize >= EM_RCTL_FLXBUF_STEP) {
	 *	rctl_bsize /= EM_RCTL_FLXBUF_STEP;
	 *	*bufsz = rctl_bsize;
	 *	return (rctl_bsize << E1000_RCTL_FLXBUF_SHIFT &
	 *		E1000_RCTL_FLXBUF_MASK);
	 * }
	 * ***************************************************************
	 */

	for (i = 0; i != sizeof(bufsz_to_rctl) / sizeof(bufsz_to_rctl[0]);
			i++) {
		if (rctl_bsize >= bufsz_to_rctl[i].bufsz) {
			*bufsz = bufsz_to_rctl[i].bufsz;
			return bufsz_to_rctl[i].rctl;
		}
	}

	/* Should never happen. */
	return -EINVAL;
}

static int
em_alloc_rx_queue_mbufs(struct em_rx_queue *rxq)
{
	struct em_rx_entry *rxe = rxq->sw_ring;
	uint64_t dma_addr;
	unsigned i;
	static const struct e1000_rx_desc rxd_init = {
		.buffer_addr = 0,
	};

	/* Initialize software ring entries */
	debug_uk_pr_info("rxq->nb_rx_desc %d\n", rxq->nb_rx_desc);
	for (i = 0; i < rxq->nb_rx_desc; i++) {
		volatile struct e1000_rx_desc *rxd;
		struct uk_netbuf *mbufs[1];
		rxq->alloc_rxpkts(rxq->alloc_rxpkts_argp, mbufs, 1);
		struct uk_netbuf *mbuf = mbufs[0];

		if (mbuf == NULL) {
			uk_pr_err("RX mbuf alloc failed "
				     "queue_id=%hu\n", rxq->queue_id);
			return -ENOMEM;
		}

		dma_addr = (uint64_t)mbuf->buf;

		/* Clear HW ring memory */
		rxq->rx_ring[i] = rxd_init;

		rxd = &rxq->rx_ring[i];
		rxd->buffer_addr = dma_addr;
		rxe[i].mbuf = mbuf;
	}

	return 0;
}

/*********************************************************************
 *
 *  Enable receive unit.
 *
 **********************************************************************/
int
eth_em_rx_init(struct e1000_hw *hw)
{
	struct em_rx_queue *rxq;
	struct uk_netdev *dev;
	uint32_t rctl;
	uint32_t rfctl;
	uint32_t rxcsum;
	uint32_t rctl_bsize;
	uint16_t i;
	int ret;

	dev = &hw->netdev;
	debug_uk_pr_info("hw = %p, dev = %p\n", hw, dev);

	/*
	 * Make sure receives are disabled while setting
	 * up the descriptor ring.
	 */
	rctl = E1000_READ_REG(hw, E1000_RCTL);
	E1000_WRITE_REG(hw, E1000_RCTL, rctl & ~E1000_RCTL_EN);

	rfctl = E1000_READ_REG(hw, E1000_RFCTL);

	/* Disable extended descriptor type. */
	rfctl &= ~E1000_RFCTL_EXTEN;

	E1000_WRITE_REG(hw, E1000_RFCTL, rfctl);

	/* Determine RX bufsize. */
	rctl_bsize = EM_MAX_BUF_SIZE;

	rctl |= em_rctl_bsize(hw->mac.type, &rctl_bsize);

	/* Configure and enable each RX queue. */
	for (i = 0; i < hw->nb_rx_queues; i++) {
		uint64_t bus_addr;
		uint32_t rxdctl;

		rxq = (struct em_rx_queue *)dev->_rx_queue[i];

		/* Allocate buffers for descriptor rings and setup queue */
		ret = em_alloc_rx_queue_mbufs(rxq);
		if (ret)
			return ret;

		/*
		 * Reset crc_len in case it was changed after queue setup by a
		 *  call to configure
		 */
		rxq->crc_len = 0;

		bus_addr = rxq->rx_ring_phys_addr;
		E1000_WRITE_REG(hw, E1000_RDLEN(i),
				rxq->nb_rx_desc *
				sizeof(*rxq->rx_ring));
		E1000_WRITE_REG(hw, E1000_RDBAH(i),
				(uint32_t)(bus_addr >> 32));
		E1000_WRITE_REG(hw, E1000_RDBAL(i), (uint32_t)bus_addr);

		E1000_WRITE_REG(hw, E1000_RDH(i), 0);
		E1000_WRITE_REG(hw, E1000_RDT(i), rxq->nb_rx_desc - 1);

		rxdctl = E1000_READ_REG(hw, E1000_RXDCTL(0));
		rxdctl &= 0xFE000000;
		rxdctl |= rxq->pthresh & 0x3F;
		rxdctl |= (rxq->hthresh & 0x3F) << 8;
		rxdctl |= (rxq->wthresh & 0x3F) << 16;
		rxdctl |= E1000_RXDCTL_GRAN;
		E1000_WRITE_REG(hw, E1000_RXDCTL(i), rxdctl);
	}

	/*
	 * Setup the Checksum Register.
	 * Receive Full-Packet Checksum Offload is mutually exclusive with RSS.
	 */
	rxcsum = E1000_READ_REG(hw, E1000_RXCSUM);
	rxcsum &= ~E1000_RXCSUM_IPOFL;
	E1000_WRITE_REG(hw, E1000_RXCSUM, rxcsum);

	/* No MRQ or RSS support for now */

	rctl &= ~(3 << E1000_RCTL_MO_SHIFT);
	rctl |= E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_LBM_NO |
		E1000_RCTL_RDMTS_HALF |
		(hw->mac.mc_filter_type << E1000_RCTL_MO_SHIFT);

	/* Make sure VLAN Filters are off. */
	rctl &= ~E1000_RCTL_VFE;
	/* Don't store bad packets. */
	rctl &= ~E1000_RCTL_SBP;
	/* Legacy descriptor type. */
	rctl &= ~E1000_RCTL_DTYP_MASK;

	/*
	 * Configure support of jumbo frames, if any.
	 */
	rctl &= ~E1000_RCTL_LPE;

	/* Enable Receives. */
	E1000_WRITE_REG(hw, E1000_RCTL, rctl);

	return 0;
}

/*********************************************************************
 *
 *  Enable transmit unit.
 *
 **********************************************************************/
void
eth_em_tx_init(struct e1000_hw *hw)
{
	struct uk_netdev *dev;
	struct em_tx_queue *txq;
	uint32_t tctl;
	uint32_t txdctl;
	uint16_t i;

	dev = &hw->netdev;

	/* Setup the Base and Length of the Tx Descriptor Rings. */
	for (i = 0; i < hw->nb_tx_queues; i++) {
		uint64_t bus_addr;
		txq = (struct em_tx_queue *)dev->_tx_queue[i];
		bus_addr = txq->tx_ring_phys_addr;
		E1000_WRITE_REG(hw, E1000_TDLEN(i),
				txq->nb_tx_desc *
				sizeof(*txq->tx_ring));
		E1000_WRITE_REG(hw, E1000_TDBAH(i),
				(uint32_t)(bus_addr >> 32));
		E1000_WRITE_REG(hw, E1000_TDBAL(i), (uint32_t)bus_addr);

		/* Setup the HW Tx Head and Tail descriptor pointers. */
		E1000_WRITE_REG(hw, E1000_TDT(i), 0);
		E1000_WRITE_REG(hw, E1000_TDH(i), 0);

		/* Setup Transmit threshold registers. */
		txdctl = E1000_READ_REG(hw, E1000_TXDCTL(i));
		/*
		 * bit 22 is reserved, on some models should always be 0,
		 * on others  - always 1.
		 */
		txdctl &= E1000_TXDCTL_COUNT_DESC;
		txdctl |= txq->pthresh & 0x3F;
		txdctl |= (txq->hthresh & 0x3F) << 8;
		txdctl |= (txq->wthresh & 0x3F) << 16;
		txdctl |= E1000_TXDCTL_GRAN;
		E1000_WRITE_REG(hw, E1000_TXDCTL(i), txdctl);
	}

	/* Program the Transmit Control Register. */
	tctl = E1000_READ_REG(hw, E1000_TCTL);
	tctl &= ~E1000_TCTL_CT;
	tctl |= (E1000_TCTL_PSP | E1000_TCTL_RTLC | E1000_TCTL_EN |
		 (E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT));

	/* This write will effectively turn on the transmit unit. */
	E1000_WRITE_REG(hw, E1000_TCTL, tctl);
}

int
em_rxq_info_get(__unused struct uk_netdev *dev, __unused uint16_t queue_id,
	__unused struct uk_netdev_queue_info *qinfo)
{
	return 0;
}

int
em_txq_info_get(__unused struct uk_netdev *dev, __unused uint16_t queue_id,
	__unused struct uk_netdev_queue_info *qinfo)
{
	return 0;
}

/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *   Copyright(c) 2016 Takuya ASADA. All rights reserved.
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

/*
 * Forked from http://dpdk.org/browse/dpdk/tree/examples/bridge/main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_arp.h>

static volatile bool force_quit;

#define RTE_LOGTYPE_BRIDGE RTE_LOGTYPE_USER1

#define MAX_ETHPORTS 2

#define NB_MBUF   8192

#define MAX_PKT_BURST 32
#define MEMPOOL_CACHE_SIZE 256

/* copied from DPDK 16.07 */
#define ETH_LINK_DOWN           0 /**< Link is down. */
#define ETH_LINK_UP             1 /**< Link is up. */

/*
 * Configurable number of RX/TX ring descriptors
 */
static uint16_t nb_rxd = 128;
static uint16_t nb_txd = 512;

static const struct rte_eth_conf port_conf = {
	.rxmode = {
		.split_hdr_size = 0,
		.header_split   = 0, /**< Header Split disabled */
		.hw_ip_checksum = 0, /**< IP checksum offload disabled */
		.hw_vlan_filter = 0, /**< VLAN filtering disabled */
		.jumbo_frame	= 0, /**< Jumbo Frame Support disabled */
		.hw_strip_crc   = 0, /**< CRC stripped by hardware */
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

struct rte_mempool * bridge_pktmbuf_pool = NULL;


#define SRC_IP_1	10
#define SRC_IP_2	0
#define SRC_IP_3	0
#define SRC_IP_4	3

#define REQ_IP_1	10
#define REQ_IP_2	0
#define REQ_IP_3	0
#define REQ_IP_4	1

/* main processing loop */
static void
bridge_main_loop(void)
{
	unsigned lcore_id;
	struct rte_mbuf *m;
	struct ether_hdr *eth_hdr;
	struct arp_hdr *arp_hdr;
	size_t pkt_size;
	uint32_t src_ip, req_ip;

	src_ip = SRC_IP_1 | (SRC_IP_2 << 8) |
				(SRC_IP_3 << 16) | (SRC_IP_4 << 24);
	req_ip = REQ_IP_1 | (REQ_IP_2 << 8) |
				(REQ_IP_3 << 16) | (REQ_IP_4 << 24);

	lcore_id = rte_lcore_id();

	if (lcore_id != rte_get_master_lcore()) {
		RTE_LOG(INFO, BRIDGE, "lcore %u has nothing to do\n", lcore_id);
		return;
	}

	RTE_LOG(INFO, BRIDGE, "entering main loop on lcore %u\n", lcore_id);

	while(!force_quit) {
		m = rte_pktmbuf_alloc(bridge_pktmbuf_pool);
		if (!m)
			return;
		pkt_size = sizeof(struct ether_hdr) + sizeof(struct arp_hdr);
		m->data_len = pkt_size;
		m->pkt_len = pkt_size;
		eth_hdr = rte_pktmbuf_mtod(m, struct ether_hdr *);
		rte_eth_macaddr_get(0, &eth_hdr->s_addr);
		memset(&eth_hdr->d_addr, 0xFF, ETHER_ADDR_LEN);
		eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_ARP);

		arp_hdr = (struct arp_hdr *)((char *)eth_hdr + sizeof(struct ether_hdr));
		arp_hdr->arp_hrd = rte_cpu_to_be_16(ARP_HRD_ETHER);
		arp_hdr->arp_pro = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
		arp_hdr->arp_hln = ETHER_ADDR_LEN;
		arp_hdr->arp_pln = sizeof(uint32_t);
		arp_hdr->arp_op = rte_cpu_to_be_16(ARP_OP_REQUEST);
	
		rte_eth_macaddr_get(0, &arp_hdr->arp_data.arp_sha);
		arp_hdr->arp_data.arp_sip = src_ip;
		memset(&arp_hdr->arp_data.arp_tha, 0, ETHER_ADDR_LEN);
		arp_hdr->arp_data.arp_tip = req_ip;
		rte_eth_tx_burst(0, 0, &m, 1);
	
		rte_delay_ms(100);
	}
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(void)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return;
		all_ports_up = 1;
		for (portid = 0; portid < MAX_ETHPORTS; portid++) {
			if (force_quit)
				return;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf("Port %d Link Up\n", (uint8_t)portid);
				else
					printf("Port %d Link Down\n", (uint8_t)portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
	}
}

int
main(int argc, char **argv)
{
	int ret;
	uint8_t nb_ports;
	uint8_t portid = 0;

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	if (rte_lcore_count() < MAX_ETHPORTS)
		rte_exit(EXIT_FAILURE, "Too few lcores\n");

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* create the mbuf pool */
	bridge_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", NB_MBUF,
		MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
		rte_socket_id());
	if (bridge_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	nb_ports = rte_eth_dev_count();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");
	if (nb_ports != MAX_ETHPORTS)
		rte_exit(EXIT_FAILURE, "Ethernet ports != %d - bye\n", MAX_ETHPORTS);

	/* init port */
	printf("Initializing port %u... ", (unsigned) portid);
	fflush(stdout);
	ret = rte_eth_dev_configure(portid, 1, 1, &port_conf);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
			  ret, (unsigned) portid);

	/* init one RX queue */
	fflush(stdout);
	ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
					 rte_eth_dev_socket_id(portid),
					 NULL,
					 bridge_pktmbuf_pool);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
			  ret, (unsigned) portid);

	/* init one TX queue on each port */
	fflush(stdout);
	ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
			rte_eth_dev_socket_id(portid),
			NULL);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
			ret, (unsigned) portid);

	/* Start device */
	ret = rte_eth_dev_start(portid);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
			  ret, (unsigned) portid);

	printf("done\n");

	rte_eth_promiscuous_enable(portid);

	check_all_ports_link_status();

	ret = 0;
	bridge_main_loop();

	for (portid = 0; portid < nb_ports; portid++) {
		printf("Closing port %d...", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}
	printf("Bye...\n");

	return ret;
}

/*
 * ZMap Copyright 2013 Regents of the University of Michigan
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

#include "recv.h"

#include <assert.h>

#include "../lib/includes.h"
#include "cachehash.h"
#include "../lib/util.h"
#include "../lib/logger.h"
#include "../lib/pbm.h"

#include <pthread.h>
#include <unistd.h>

#include "recv-internal.h"
#include "state.h"
#include "validate.h"
#include "fieldset.h"
#include "shard.h"
#include "expression.h"
#include "probe_modules/packet.h"
#include "probe_modules/probe_modules.h"
#include "output_modules/output_modules.h"

static u_char fake_eth_hdr[65535];
// bitmap of observed IP addresses
static uint8_t **seen = NULL;
static cachehash *ch = NULL;

void handle_packet(uint32_t buflen, const u_char *bytes,
		   const struct timespec ts)
{
	if ((sizeof(struct ip) + zconf.data_link_size) > buflen) {
		// buffer not large enough to contain ethernet
		// and ip headers. further action would overrun buf
		return;
	}
	struct ip *ip_hdr = (struct ip *)&bytes[zconf.data_link_size];
	uint32_t src_ip = ip_hdr->ip_src.s_addr;
	uint16_t src_port = 0;

	uint32_t len_ip_and_payload =
	    buflen - (zconf.send_ip_pkts ? 0 : sizeof(struct ether_header));
	// extract port if TCP or UDP packet to both generate validation data and to
	// check if the response is a duplicate
	if (ip_hdr->ip_p == IPPROTO_TCP) {
		struct tcphdr *tcp = get_tcp_header(ip_hdr, len_ip_and_payload);
		if (tcp) {
			src_port = tcp->th_sport;
		}
	} else if (ip_hdr->ip_p == IPPROTO_UDP) {
		struct udphdr *udp = get_udp_header(ip_hdr, len_ip_and_payload);
		if (udp) {
			src_port = udp->uh_sport;
		}
	}

	uint32_t validation[VALIDATE_BYTES / sizeof(uint32_t)];
	// TODO: for TTL exceeded messages, ip_hdr->saddr is going to be
	// different and we must calculate off potential payload message instead
	validate_gen(ip_hdr->ip_dst.s_addr, ip_hdr->ip_src.s_addr, src_port,
		     (uint8_t *)validation);

	if (!zconf.probe_module->validate_packet(
		ip_hdr, len_ip_and_payload, &src_ip, validation, zconf.ports)) {
		zrecv.validation_failed++;
		return;
	} else {
		zrecv.validation_passed++;
	}
	// woo! We've validated that the packet is a response to our scan
	int is_repeat = 0;
	if (zconf.dedup_method == DEDUP_METHOD_FULL) {
		is_repeat = pbm_check(seen, ntohl(src_ip));
	} else if (zconf.dedup_method == DEDUP_METHOD_WINDOW) {
		target_t t = {.ip = src_ip, .port = src_port, .status = 0};
		if (cachehash_get(ch, &t, sizeof(target_t))) {
			is_repeat = 1;
		} else {
			cachehash_put(ch, &t, sizeof(target_t), (void *)1);
		}
	}
	// track whether this is the first packet in an IP fragment.
	if (ip_hdr->ip_off & IP_MF) {
		zrecv.ip_fragments++;
	}

	fieldset_t *fs = fs_new_fieldset(&zconf.fsconf.defs);
	fs_add_ip_fields(fs, ip_hdr);
	// HACK:
	// probe modules expect the full ethernet frame
	// in process_packet. For VPN, we only get back an IP frame.
	// Here, we fake an ethernet frame (which is initialized to
	// have ETH_P_IP proto and 00s for dest/src).
	if (zconf.send_ip_pkts) {
		static const uint32_t available_space = sizeof(fake_eth_hdr) - sizeof(struct ether_header);
		assert(buflen > (uint32_t)zconf.data_link_size);
		buflen -= zconf.data_link_size;
		if (buflen > available_space) {
			buflen = available_space;
		}
		memcpy(&fake_eth_hdr[sizeof(struct ether_header)],
		       bytes + zconf.data_link_size, buflen);
		bytes = fake_eth_hdr;
		buflen += sizeof(struct ether_header);
	}
	zconf.probe_module->process_packet(bytes, buflen, fs, validation, ts);
	fs_add_system_fields(fs, is_repeat, zsend.complete, ts);
	int success_index = zconf.fsconf.success_index;
	assert(success_index < fs->len);
	int is_success = fs_get_uint64_by_index(fs, success_index);

	if (is_success) {
		zrecv.success_total++;
		if (!is_repeat) {
			zrecv.success_unique++;
			if (zconf.dedup_method == DEDUP_METHOD_FULL) {
				pbm_set(seen, ntohl(src_ip));
			} else if (zconf.dedup_method == DEDUP_METHOD_WINDOW) {
			}
		}
		if (zsend.complete) {
			zrecv.cooldown_total++;
			if (!is_repeat) {
				zrecv.cooldown_unique++;
			}
		}
	} else {
		zrecv.failure_total++;
	}
	// probe module includes app_success field
	if (zconf.fsconf.app_success_index >= 0) {
		int is_app_success =
		    fs_get_uint64_by_index(fs, zconf.fsconf.app_success_index);
		if (is_app_success) {
			zrecv.app_success_total++;
			if (!is_repeat) {
				zrecv.app_success_unique++;
			}
		}
	}

	fieldset_t *o = NULL;
	// we need to translate the data provided by the probe module
	// into a fieldset that can be used by the output module
	if (!is_success && zconf.default_mode) {
		goto cleanup;
	}
	if (is_repeat && zconf.default_mode) {
		goto cleanup;
	}
	if (!evaluate_expression(zconf.filter.expression, fs)) {
		goto cleanup;
	}
	zrecv.filter_success++;
	o = translate_fieldset(fs, &zconf.fsconf.translation);
	if (zconf.output_module && zconf.output_module->process_ip) {
		zconf.output_module->process_ip(o);
	}
cleanup:
	fs_free(fs);
	free(o);
	if (zconf.output_module && zconf.output_module->update &&
	    !(zrecv.success_unique % zconf.output_module->update_interval)) {
		zconf.output_module->update(&zconf, &zsend, &zrecv);
	}
}

int recv_run(pthread_mutex_t *recv_ready_mutex)
{
	log_trace("recv", "recv thread started");
	log_debug("recv", "capturing responses on %s", zconf.iface);
	if (!zconf.dryrun) {
		recv_init();
	}
	if (zconf.send_ip_pkts) {
		struct ether_header *eth = (struct ether_header *)fake_eth_hdr;
		memset(fake_eth_hdr, 0, sizeof(fake_eth_hdr));
		eth->ether_type = htons(ETHERTYPE_IP);
	}
	// initialize paged bitmap
	if (zconf.dedup_method == DEDUP_METHOD_FULL) {
		seen = pbm_init();
	} else if (zconf.dedup_method == DEDUP_METHOD_WINDOW) {
		ch = cachehash_init(zconf.dedup_window_size, NULL);
	}
	if (zconf.default_mode) {
		log_info("recv",
			 "duplicate responses will be excluded from output");
		log_info("recv",
			 "unsuccessful responses will be excluded from output");
	} else {
		log_info(
		    "recv",
		    "duplicate responses will be passed to the output module");
		log_info(
		    "recv",
		    "unsuccessful responses will be passed to the output module");
	}
	pthread_mutex_lock(recv_ready_mutex);
	zconf.recv_ready = 1;
	pthread_mutex_unlock(recv_ready_mutex);
	zrecv.start = now();
	if (zconf.max_results == 0) {
		zconf.max_results = -1;
	}

	do {
		if (zconf.dryrun) {
			sleep(1);
		} else {
			recv_packets();
			if (zconf.max_results &&
			    zrecv.filter_success >= zconf.max_results) {
				break;
			}
		}
	} while (
	    !(zsend.complete && (now() - zsend.finish > zconf.cooldown_secs)));
	zrecv.finish = now();
	// get final pcap statistics before closing
	recv_update_stats();
	if (!zconf.dryrun) {
		pthread_mutex_lock(recv_ready_mutex);
		recv_cleanup();
		pthread_mutex_unlock(recv_ready_mutex);
	}
	zrecv.complete = 1;
	log_debug("recv", "thread finished");
	return 0;
}

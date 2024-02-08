/*
 * ZMap Copyright 2013 Regents of the University of Michigan
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef ZMAP_GET_GATEWAY_LINUX_H
#define ZMAP_GET_GATEWAY_LINUX_H

#ifdef ZMAP_GET_GATEWAY_BSD_H
#error "Don't include both get_gateway-bsd.h and get_gateway-linux.h"
#endif

#include <string.h>
#include <sys/ioctl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <arpa/inet.h>
#include <pcap/pcap.h>

#define GW_BUFFER_SIZE 64000

int read_nl_sock(int sock, char *buf, int buf_len)
{
	int msg_len = 0;
	char *pbuf = buf;
	do {
		int len = recv(sock, pbuf, buf_len - msg_len, 0);
		if (len <= 0) {
			log_debug("get-gw", "recv failed: %s", strerror(errno));
			return -1;
		}
		struct nlmsghdr *nlhdr = (struct nlmsghdr *)pbuf;
		if (NLMSG_OK(nlhdr, ((unsigned int)len)) == 0 ||
		    nlhdr->nlmsg_type == NLMSG_ERROR) {
			log_debug("get-gw", "recv failed: %s", strerror(errno));
			return -1;
		}
		if (nlhdr->nlmsg_type == NLMSG_DONE) {
			break;
		} else {
			msg_len += len;
			pbuf += len;
		}
		if ((nlhdr->nlmsg_flags & NLM_F_MULTI) == 0) {
			break;
		}
	} while (1);
	return msg_len;
}

int send_nl_req(uint16_t msg_type, uint32_t seq, void *payload,
		uint32_t payload_len)
{
	int sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (sock < 0) {
		log_error("get-gw", "unable to get socket: %s",
			  strerror(errno));
		return -1;
	}
	if (NLMSG_SPACE(payload_len) < payload_len) {
		close(sock);
		// Integer overflow
		return -1;
	}
	struct nlmsghdr *nlmsg;
	nlmsg = xmalloc(NLMSG_SPACE(payload_len));

	memset(nlmsg, 0, NLMSG_SPACE(payload_len));
	memcpy(NLMSG_DATA(nlmsg), payload, payload_len);
	nlmsg->nlmsg_type = msg_type;
	nlmsg->nlmsg_len = NLMSG_LENGTH(payload_len);
	nlmsg->nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
	nlmsg->nlmsg_seq = seq;
	nlmsg->nlmsg_pid = getpid();

	if (send(sock, nlmsg, nlmsg->nlmsg_len, 0) < 0) {
		log_error("get-gw", "failure sending: %s", strerror(errno));
		return -1;
	}
	free(nlmsg);
	return sock;
}

int get_hw_addr(struct in_addr *gw_ip, char *iface, unsigned char *hw_mac)
{
	// TODOPhillip - what was gw_ip used for? NDA_DST
	// Looks like getting the IP address of the gateway, talk to Zakir about what that was used for
	int sockfd;
	struct ifreq ifr;
	struct sockaddr_in *sin;
	// Open a socket
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return 1;
	}
	// Get the MAC address of the interface
	strncpy(ifr.ifr_name, iface, strlen(iface));
	if (ioctl(sockfd, SIOCGIFHWADDR, &ifr) < 0) {
		perror("ioctl");
		close(sockfd);
		log_error("get-gw", "error with getting hardware MAC address of interface %s: %s", iface, strerror(errno));
		return 1;
	}
	memcpy(hw_mac, ifr.ifr_hwaddr.sa_data, IFHWADDRLEN);
	// Get the gateway IP address
	if (ioctl(sockfd, SIOCGIFADDR, &ifr) < 0) {
		perror("ioctl");
		close(sockfd);
		return 1;
	}
	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	memcpy(gw_ip, &sin->sin_addr, sizeof(struct in_addr));
	close(sockfd);
	return 0;
}

// gw and iface[IF_NAMESIZE] MUST be allocated
int _get_default_gw(struct in_addr *gw, char *iface)
{
	struct rtmsg req;
	unsigned int nl_len;
	char buf[8192];
	struct nlmsghdr *nlhdr;

	if (!gw || !iface) {
		return -1;
	}

	// Send RTM_GETROUTE request
	memset(&req, 0, sizeof(req));
	int sock = send_nl_req(RTM_GETROUTE, 0, &req, sizeof(req));

	// Read responses
	nl_len = read_nl_sock(sock, buf, sizeof(buf));
	if (nl_len <= 0) {
		return -1;
	}

	// Parse responses
	nlhdr = (struct nlmsghdr *)buf;
	while (NLMSG_OK(nlhdr, nl_len)) {
		struct rtattr *rt_attr;
		struct rtmsg *rt_msg;
		int rt_len;
		int has_gw = 0;

		rt_msg = (struct rtmsg *)NLMSG_DATA(nlhdr);

		// There could be multiple routing tables. Loop until we find the
		// correct one.
		if ((rt_msg->rtm_family != AF_INET) ||
		    (rt_msg->rtm_table != RT_TABLE_MAIN)) {
			nlhdr = NLMSG_NEXT(nlhdr, nl_len);
			continue;
		}

		rt_attr = (struct rtattr *)RTM_RTA(rt_msg);
		rt_len = RTM_PAYLOAD(nlhdr);
		while (RTA_OK(rt_attr, rt_len)) {
			switch (rt_attr->rta_type) {
			case RTA_OIF:
				if_indextoname(*(int *)RTA_DATA(rt_attr),
					       iface);
				break;
			case RTA_GATEWAY:
				gw->s_addr = *(unsigned int *)RTA_DATA(rt_attr);
				has_gw = 1;
				break;
			}
			rt_attr = RTA_NEXT(rt_attr, rt_len);
		}

		if (has_gw) {
			return 0;
		}
		nlhdr = NLMSG_NEXT(nlhdr, nl_len);
	}
	return -1;
}

char *get_default_iface(void)
{
	struct in_addr gw;
	char *iface;

	iface = malloc(IF_NAMESIZE);
	memset(iface, 0, IF_NAMESIZE);

	if(_get_default_gw(&gw, iface)) {
		log_fatal(
		    "send",
		    "ZMap could not detect your default network interface. "
		    "You likely do not have sufficient privileges to open a raw packet socket. "
		    "Are you running as root or with the CAP_NET_RAW capability? If you are, you "
		    "may need to manually set interface using the \"-i\" flag.");
	} else {
		return iface;
	}
}

int get_default_gw(struct in_addr *gw, char *iface)
{
	char _iface[IF_NAMESIZE];
	memset(_iface, 0, IF_NAMESIZE);

	_get_default_gw(gw, _iface);
	if (strcmp(iface, _iface)) {
		log_fatal(
		    "get-gateway",
		    "The specified network (\"%s\") does not match "
		    "the interface associated with the default gateway (%s). You will "
		    "need to manually specify the MAC address of your gateway using "
		    "the \"--gateway-mac\" flag.",
		    iface, _iface);
	}
	return EXIT_SUCCESS;
}

int get_iface_ip(char *iface, struct in_addr *ip)
{
	int sock;
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(struct ifreq));
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		log_fatal("get-iface-ip", "failure opening socket: %s",
			  strerror(errno));
	}
	ifr.ifr_addr.sa_family = AF_INET;
	strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);

	if (ioctl(sock, SIOCGIFADDR, &ifr) < 0) {
		close(sock);
		log_fatal(
		    "get-iface-ip",
		    "Unable to automatically identify the correct "
		    "source address for %s interface. ioctl failure: %s. "
		    "If this is the unexpected interface, you can manually specify "
		    "the correct interface with \"-i\" flag. If this is the correct "
		    "interface, you likely need to manually specify the source IP "
		    "address to use with the \"-S\" flag.",
		    iface, strerror(errno));
	}
	ip->s_addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
	close(sock);
	return EXIT_SUCCESS;
}

int get_iface_hw_addr(char *iface, unsigned char *hw_mac)
{
	int s;
	struct ifreq buffer;

	// Load the hwaddr from a dummy socket
	s = socket(PF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		log_error("get_iface_hw_addr", "Unable to open socket: %s",
			  strerror(errno));
		return EXIT_FAILURE;
	}
	memset(&buffer, 0, sizeof(buffer));
	strncpy(buffer.ifr_name, iface, IFNAMSIZ);
	ioctl(s, SIOCGIFHWADDR, &buffer);
	close(s);
	memcpy(hw_mac, buffer.ifr_hwaddr.sa_data, 6);
	return EXIT_SUCCESS;
}

#endif /* ZMAP_GET_GATEWAY_LINUX_H */

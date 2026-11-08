/*
 * dl-rule.c — install a downlink filter rule into the MODEM's IPA via the
 * AP's QMI_IPA channel, replacing the stock ipacm step on Debian.
 *
 * Chain: /dev/wwan_ioctl WAN_IOC_ADD_FLT_RULE_EX
 *        -> rmnet_ipa_fd_ioctl.c -> ipa3_qmi_filter_request_ex_send()
 *        -> QMI_IPA_INSTALL_FILTER_RULE_EX_REQ -> modem.
 * The gates (ipa3_qmi_indication_fin && ipa3_qmi_modem_init_fin && ipa_q6_clnt)
 * are open once the QMI_IPA handshake completes (ns-lite v3 fix).
 *
 * The rule tells the modem's IPA: DL packets matching <dst-ip> (or all, with
 * --match-all) -> route via the AP's default WAN routing table
 * ("ipa_dflt_wan_rt", created by the ingress ioctl, dst = APPS_WAN_CONS)
 * with QMAP mux id <mux>. Routing-table indices live in the shared IPA index
 * space (the AP allocates a slice to the modem in INIT_MODEM_DRIVER), so the
 * modem may reference the AP-side table.
 *
 * Build on the phone:  gcc -O2 -I. -o dl-rule dl-rule.c
 * Usage: dl-rule <call-ipv4> [mux_id]        — dst-IP match
 *        dl-rule --match-all [mux_id]        — catch-all (one-call setups)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>

#include "linux/msm_ipa.h"
#include "linux/ipa_qmi_service_v01.h"
#include "linux/rmnet_ipa_fd_ioctl.h"

int main(int argc, char **argv)
{
	int fd_ipa, fd_wwan, rc;
	struct ipa_ioc_get_rt_tbl_indx q;
	uint32_t mux_id = 1;
	int match_all = 0;
	uint32_t dst_ip = 0;   /* network byte order */
	const char *ip_str = NULL;

	int exception = 0;

	if (argc >= 2 && strcmp(argv[1], "--match-all") == 0) {
		match_all = 1;
		if (argc >= 3) mux_id = (uint32_t)atoi(argv[2]);
	} else if (argc >= 2 && strcmp(argv[1], "--exception") == 0) {
		exception = 1;
		match_all = 1;
		if (argc >= 3) mux_id = (uint32_t)atoi(argv[2]);
	} else if (argc >= 2) {
		ip_str = argv[1];
		if (inet_pton(AF_INET, ip_str, &dst_ip) != 1) {
			fprintf(stderr, "bad IPv4: %s\n", ip_str);
			return 2;
		}
		if (argc >= 3) mux_id = (uint32_t)atoi(argv[2]);
	} else {
		fprintf(stderr,
			"usage: dl-rule <call-ipv4> [mux_id] | --match-all [mux_id] | --exception [mux_id]\n");
		return 2;
	}

	/* 1. find the AP's default WAN v4 routing table index (shared index space) */
	memset(&q, 0, sizeof(q));
	q.ip = IPA_IP_v4;
	strncpy(q.name, "ipa_dflt_wan_rt", IPA_RESOURCE_NAME_MAX - 1);
	fd_ipa = open("/dev/ipa", O_RDWR);
	if (fd_ipa < 0) { perror("open /dev/ipa"); return 1; }
	rc = ioctl(fd_ipa, IPA_IOC_QUERY_RT_TBL_INDEX, &q);
	if (rc) { perror("QUERY_RT_TBL_INDEX ipa_dflt_wan_rt"); close(fd_ipa); return 1; }
	printf("dflt wan v4 rt idx = %u\n", q.idx);
	close(fd_ipa);

	/* 2. build the ex request: one v4 spec */
	static struct ipa_install_fltr_rule_req_ex_msg_v01 req;
	memset(&req, 0, sizeof(req));
	req.filter_spec_ex_list_valid = 1;
	req.filter_spec_ex_list_len = 1;
	req.num_ipv4_filters_valid = 1;
	req.num_ipv4_filters = 1;

	struct ipa_filter_spec_ex_type_v01 *s = &req.filter_spec_ex_list[0];
	s->ip_type = QMI_IPA_IP_TYPE_V4_V01;
	s->filter_action = exception ? QMI_IPA_FILTER_ACTION_EXCEPTION_V01
				     : QMI_IPA_FILTER_ACTION_ROUTING_V01;
	s->is_routing_table_index_valid = 1;
	s->route_table_index = q.idx;
	s->is_mux_id_valid = 1;
	s->mux_id = mux_id;
	s->rule_id = 0;
	s->is_rule_hashable = 0;

	if (!match_all) {
		/* dst-ip match: offset_meq_32[0] = {offset 16 (v4 dst), mask, value} */
		s->filter_rule.num_offset_meq_32 = 1;
		s->filter_rule.offset_meq_32[0].offset = 16;
		s->filter_rule.offset_meq_32[0].mask = 0xffffffffU;
		s->filter_rule.offset_meq_32[0].value = dst_ip;
	}
	/* match_all: empty eq attribs = match every v4 packet */

	/* 3. send to the modem */
	fd_wwan = open("/dev/wwan_ioctl", O_RDWR);
	if (fd_wwan < 0) { perror("open /dev/wwan_ioctl"); return 1; }
	rc = ioctl(fd_wwan, WAN_IOC_ADD_FLT_RULE_EX, &req);
	if (rc) {
		perror("WAN_IOC_ADD_FLT_RULE_EX");
		close(fd_wwan);
		return 1;
	}
	printf("DL filter rule installed on modem: %s action=%s rt_idx %u, mux %u\n",
	       match_all ? (exception ? "match-all-v4" : "match-all-v4") : ip_str,
	       exception ? "EXCEPTION" : "ROUTING", q.idx, mux_id);
	close(fd_wwan);
	return 0;
}

/*
 * dl-rule2.c — replicate stock ipacm's call-time DL activation on the d2q.
 *
 * Stock activates the modem data path at call time by installing TWO DL filter
 * rules on the modem via QMI_IPA_INSTALL_FILTER_RULE_EX (0x37): a 0-rule CLEAR
 * first, then 2 rules (v4 + v6), each ROUTING into the AP's default WAN route
 * table (idx from QUERY_RT_TBL_INDEX of "ipa_dflt_wan_rt"). The modem answers
 * FILTER_INSTALLED_NOTIF and its data path comes up (UL self-install + Q6 pipe).
 *
 * This tool: CLEAR (0 rules) -> v4 catch-all + v6 catch-all -> reads the
 * FILTER_INSTALLED_NOTIF.
 *
 * Run AFTER the WDS call is up (wds-call3), matching stock order.
 * Build on the phone:  gcc -O2 -I. -o dl-rule2 dl-rule2.c
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "linux/msm_ipa.h"
#include "linux/ipa_qmi_service_v01.h"
#include "linux/rmnet_ipa_fd_ioctl.h"

static int get_rt_idx(int fd_ipa, enum ipa_ip_type ip, const char *name)
{
	struct ipa_ioc_get_rt_tbl_indx q;
	memset(&q, 0, sizeof(q));
	q.ip = ip;
	strncpy(q.name, name, IPA_RESOURCE_NAME_MAX - 1);
	if (ioctl(fd_ipa, IPA_IOC_QUERY_RT_TBL_INDEX, &q)) {
		perror("QUERY_RT_TBL_INDEX");
		return -1;
	}
	return (int)q.idx;
}

static int send_req(struct ipa_install_fltr_rule_req_ex_msg_v01 *req, const char *tag)
{
	int fd = open("/dev/wwan_ioctl", O_RDWR);
	if (fd < 0) { perror("open wwan_ioctl"); return -1; }
	int rc = ioctl(fd, WAN_IOC_ADD_FLT_RULE_EX, req);
	if (rc) perror("WAN_IOC_ADD_FLT_RULE_EX");
	else printf("%s: sent (rc=0)\n", tag);
	close(fd);
	return rc;
}

int main(void)
{
	int fd_ipa = open("/dev/ipa", O_RDWR);
	if (fd_ipa < 0) { perror("open /dev/ipa"); return 1; }
	int v4 = get_rt_idx(fd_ipa, IPA_IP_v4, "ipa_dflt_wan_rt");
	int v6 = get_rt_idx(fd_ipa, IPA_IP_v6, "ipa_dflt_wan_rt");
	close(fd_ipa);
	printf("dflt_wan_rt idx: v4=%d v6=%d\n", v4, v6);
	if (v4 < 0) return 1;

	/* 1. CLEAR: 0-rule request */
	static struct ipa_install_fltr_rule_req_ex_msg_v01 clr;
	memset(&clr, 0, sizeof(clr));
	clr.filter_spec_ex_list_valid = 1;
	clr.filter_spec_ex_list_len = 0;
	clr.num_ipv4_filters_valid = 1; clr.num_ipv4_filters = 0;
	clr.num_ipv6_filters_valid = 1; clr.num_ipv6_filters = 0;
	if (getenv("NOCLEAR")==NULL) { if (send_req(&clr, "clear(0 rules)")) fprintf(stderr,"clear failed (non-fatal)\n"); }

	/* 2. two rules: v4 catch-all + v6 catch-all, ROUTING via dflt_wan_rt */
	static struct ipa_install_fltr_rule_req_ex_msg_v01 req;
	memset(&req, 0, sizeof(req));
	req.filter_spec_ex_list_valid = 1;
	req.filter_spec_ex_list_len = 2;
	req.num_ipv4_filters_valid = 1; req.num_ipv4_filters = 1;
	if (v6 >= 0) { req.num_ipv6_filters_valid = 1; req.num_ipv6_filters = 1; }

	struct ipa_filter_spec_ex_type_v01 *s;
	s = &req.filter_spec_ex_list[0];           /* v4 catch-all */
	s->ip_type = QMI_IPA_IP_TYPE_V4_V01;
	s->filter_action = QMI_IPA_FILTER_ACTION_ROUTING_V01;
	s->is_routing_table_index_valid = 1;
	s->route_table_index = v4;
	s->is_mux_id_valid = 1; s->mux_id = 1;
	s->is_rule_hashable = 0;                   /* empty eq = match all v4 */
	if (v6 >= 0) {
		s = &req.filter_spec_ex_list[1];   /* v6 catch-all */
		s->ip_type = QMI_IPA_IP_TYPE_V6_V01;
		s->filter_action = QMI_IPA_FILTER_ACTION_ROUTING_V01;
		s->is_routing_table_index_valid = 1;
		s->route_table_index = v6;
		s->is_mux_id_valid = 1; s->mux_id = 1;
		s->is_rule_hashable = 0;
	}
	if (send_req(&req, "2 rules (v4+v6 catch-all)")) return 1;
	puts("done — check FILTER_INSTALLED_NOTIF + WDS rx_ok");
	return 0;
}

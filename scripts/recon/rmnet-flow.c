/*
 * rmnet-flow: single-shot QMAP/rmnet flow-setup trigger for the d2q (SM8150)
 * Qualcomm CAF data path. Sends exactly ONE rtnetlink client-setup request to
 * the kernel's qmi_rmnet driver (via the rmnet changelink), which triggers the
 * WDA powersave handshake (wda_qmi_client_init -> wda_svc_config ->
 * WDA_SET_POWERSAVE_CONFIG). This is the step Qualcomm's ipacm daemon does on
 * Android, and its absence is why the modem rejects WDS data calls.
 *
 * SAFETY (per the bring-up constraints):
 *  - read-only by default (--dry-run prints the full message, sends nothing)
 *  - exactly one RTM_NEWLINK (create the rmnet child if missing) + one
 *    RTM_SETLINK (the client setup); never retries, never rolls back,
 *    never unbinds/reloads/restarts any driver, never writes /dev/ipa twice.
 *  - validates the target interface exists and reports the rtnetlink ACK.
 *
 * Build (host, for the arm64 phone):
 *   clang --target=aarch64-linux-gnu -O2 -o rmnet-flow rmnet-flow.c
 *   (or on the phone: gcc -O2 -o rmnet-flow rmnet-flow.c)
 *
 * Usage:
 *   rmnet-flow --dry-run                 # print the exact message, send nothing
 *   rmnet-flow <real_dev> <child> <mux_id>   # live: create child + one setup
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <net/if.h>

/* ---- CAF rmnet extended attributes (past mainline __IFLA_RMNET_MAX=3) ---- */
#ifndef IFLA_RMNET_MUX_ID
#define IFLA_RMNET_MUX_ID 1
#endif
#ifndef IFLA_RMNET_FLAGS
#define IFLA_RMNET_FLAGS 2
#endif
#define IFLA_RMNET_DFC_QOS      3   /* CAF extension: carries a tcmsg */
#define IFLA_RMNET_UL_AGG_PARAMS 4

/* qmi_rmnet flow message types (stored in tcmsg.tcm_family) */
#define NLMSG_CLIENT_SETUP 4

/* qmi_rmnet flags (in tcm_ifindex) */
#define FLAG_POWERSAVE_MASK 0x0010

struct ifla_rmnet_flags_caf { uint32_t flags, mask; };

/* --- netlink attribute helpers --- */
static struct nlattr *putattr(struct nlmsghdr *n, int *len, int type,
                              const void *data, int dlen)
{
	struct nlattr *a = (struct nlattr *)((char *)n + *len);
	a->nla_type = type;
	a->nla_len = NLA_HDRLEN + dlen;
	if (dlen) memcpy((char *)a + NLA_HDRLEN, data, dlen);
	*len += NLA_ALIGN(a->nla_len);
	return a;
}
static struct nlattr *nest_start(struct nlmsghdr *n, int *len, int type)
{
	struct nlattr *a = (struct nlattr *)((char *)n + *len);
	a->nla_type = NLA_F_NESTED | type;
	*len += NLA_HDRLEN;
	return a;
}
static void nest_end(struct nlmsghdr *n, int *len, struct nlattr *a)
{
	a->nla_len = ((char *)n + *len) - (char *)a;
}

/* Build the flow-setup changelink message for a rmnet child interface. */
static int build_setup_msg(char *buf, int buflen, int child_ifindex,
                           int real_ifindex, int mux_id,
                           uint32_t ep_type, uint32_t iface_id, uint32_t flags,
                           int *out_len)
{
	struct nlmsghdr *n = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifi;
	struct nlattr *li, *id;
	struct tcmsg tcm;
	int len = NLMSG_HDRLEN;

	memset(buf, 0, buflen);
	n->nlmsg_len = 0;
	n->nlmsg_type = RTM_SETLINK;
	n->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	n->nlmsg_seq = 1;
	n->nlmsg_pid = 0;

	ifi = (struct ifinfomsg *)(buf + len);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_type = 0;
	ifi->ifi_index = child_ifindex;   /* the rmnet child */
	ifi->ifi_flags = 0;
	ifi->ifi_change = 0;
	len += NLMSG_ALIGN(sizeof(*ifi));

	/* the real (lower) device the rmnet child sits on */
	putattr(n, &len, IFLA_LINK, &real_ifindex, sizeof(real_ifindex));

	/* IFLA_LINKINFO { IFLA_INFO_KIND="rmnet", IFLA_INFO_DATA{ ... } } */
	li = nest_start(n, &len, IFLA_LINKINFO);
	putattr(n, &len, IFLA_INFO_KIND, "rmnet", 6);
	id = nest_start(n, &len, IFLA_INFO_DATA);

	/* the DFC_QOS tcmsg: the client-setup that triggers the WDA handshake */
	memset(&tcm, 0, sizeof(tcm));
	tcm.tcm_family  = NLMSG_CLIENT_SETUP;  /* 4 */
	tcm.tcm_handle  = 0;                    /* service instance 0 -> WDA path */
	tcm.tcm_info    = ep_type;              /* endpoint type */
	tcm.tcm_parent  = iface_id;             /* endpoint interface id */
	tcm.tcm_ifindex = flags;                /* config flags (FLAG_POWERSAVE_MASK) */
	putattr(n, &len, IFLA_RMNET_DFC_QOS, &tcm, sizeof(tcm));

	nest_end(n, &len, id);
	nest_end(n, &len, li);
	n->nlmsg_len = len;
	*out_len = len;
	return 0;
}

/* Build the rmnet-child creation message (RTM_NEWLINK). */
static int build_newlink_msg(char *buf, int buflen, const char *child,
                             int real_ifindex, int mux_id, int *out_len)
{
	struct nlmsghdr *n = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifi;
	struct nlattr *li, *id;
	int len = NLMSG_HDRLEN;

	memset(buf, 0, buflen);
	n->nlmsg_type = RTM_NEWLINK;
	n->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
	n->nlmsg_seq = 1;

	ifi = (struct ifinfomsg *)(buf + len);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = 0;
	ifi->ifi_flags = 0;
	ifi->ifi_change = 0;
	len += NLMSG_ALIGN(sizeof(*ifi));

	putattr(n, &len, IFLA_IFNAME, child, strlen(child) + 1);
	putattr(n, &len, IFLA_LINK, &real_ifindex, sizeof(real_ifindex));

	li = nest_start(n, &len, IFLA_LINKINFO);
	putattr(n, &len, IFLA_INFO_KIND, "rmnet", 6);
	id = nest_start(n, &len, IFLA_INFO_DATA);
	putattr(n, &len, IFLA_RMNET_MUX_ID, &mux_id, sizeof(uint16_t));
	nest_end(n, &len, id);
	nest_end(n, &len, li);
	n->nlmsg_len = len;
	*out_len = len;
	return 0;
}

static void dump_hex(const char *tag, const void *p, int len)
{
	const unsigned char *b = p;
	int i;
	printf("%s (%d bytes):\n", tag, len);
	for (i = 0; i < len; i++) {
		printf("%02x ", b[i]);
		if ((i & 15) == 15) printf("\n");
	}
	if (len & 15) printf("\n");
}

int main(int argc, char **argv)
{
	const char *real_name, *child_name;
	int real_ifindex, child_ifindex, mux_id;
	uint32_t ep_type, iface_id, flags;
	int dry_run = 0;
	int fd, len, rc;
	char buf[512];
	struct sockaddr_nl sa;
	char rbuf[256];

	/* Defaults derived from the local kernel source (see report):
	 *  ep_type = DATA_EP_DESC_TYPE_EMB_FLOW_CTL_PROD_V01 (0x0B) for the
	 *           embedded flow-control producer endpoint (rmnet_ipa path)
	 *  iface_id = the QMI data-endpoint interface number (CONFIGURABLE —
	 *           confirm against the stock N9750 config before going live)
	 *  flags = FLAG_POWERSAVE_MASK (0x10) — required to route into the WDA path
	 */
	ep_type = 0x0B; iface_id = 1; flags = FLAG_POWERSAVE_MASK;

	if (argc > 1 && !strcmp(argv[1], "--dry-run")) dry_run = 1;

	if (dry_run) {
		int l;
		printf("DRY RUN — the exact message the helper would send:\n");
		printf("  RTM_SETLINK changelink on the rmnet child, IFLA_LINKINFO/rmnet\n");
		printf("  IFLA_RMNET_DFC_QOS = tcmsg{ tcm_family=%d (NLMSG_CLIENT_SETUP),\n",
		       NLMSG_CLIENT_SETUP);
		printf("    tcm_handle=0 (svc instance 0 -> WDA), tcm_info=0x%x (ep_type),\n", ep_type);
		printf("    tcm_parent=0x%x (iface_id), tcm_ifindex=0x%x (FLAG_POWERSAVE_MASK) }\n",
		       iface_id, flags);
		build_setup_msg(buf, sizeof(buf), 0, 0, 0, ep_type, iface_id, flags, &l);
		dump_hex("changelink message", buf, l);
		printf("\nKernel path: rmnet_changelink -> qmi_rmnet_change_link ->\n");
		printf("  qmi_rmnet_setup_client -> wda_qmi_client_init -> (WDA svc arrival)\n");
		printf("  wda_svc_config -> WDA_SET_POWERSAVE_CONFIG (0x002D).\n");
		printf("NOTE: the local WDA impl sends POWERSAVE_CONFIG only, NOT SET_DATA_FORMAT.\n");
		return 0;
	}

	if (argc < 4) {
		fprintf(stderr, "usage: %s [--dry-run] | <real_dev> <child> <mux_id> [ep_type] [iface_id] [flags]\n", argv[0]);
		return 1;
	}
	real_name = argv[1]; child_name = argv[2]; mux_id = atoi(argv[3]);
	if (argc > 4) ep_type = (uint32_t)strtoul(argv[4], NULL, 0);
	if (argc > 5) iface_id = (uint32_t)strtoul(argv[5], NULL, 0);
	if (argc > 6) flags = (uint32_t)strtoul(argv[6], NULL, 0);

	real_ifindex = if_nametoindex(real_name);
	if (!real_ifindex) { fprintf(stderr, "no such interface: %s\n", real_name); return 1; }
	printf("real dev %s ifindex %d\n", real_name, real_ifindex);

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0) { perror("socket"); return 1; }
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }

	/* 1. create the rmnet child (if missing) */
	child_ifindex = if_nametoindex(child_name);
	if (!child_ifindex) {
		build_newlink_msg(buf, sizeof(buf), child_name, real_ifindex, mux_id, &len);
		rc = send(fd, buf, len, 0);
		if (rc < 0) { perror("newlink send"); close(fd); return 1; }
		/* read the ACK */
		rc = recv(fd, rbuf, sizeof(rbuf), 0);
		if (rc > 0) {
			struct nlmsghdr *rn = (struct nlmsghdr *)rbuf;
			if (rn->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(rn);
				if (e->error) { fprintf(stderr, "newlink ACK error: %s\n", strerror(-e->error)); close(fd); return 1; }
				printf("child %s created (rtnetlink ACK ok)\n", child_name);
			}
		}
		child_ifindex = if_nametoindex(child_name);
	} else {
		printf("child %s exists (ifindex %d)\n", child_name, child_ifindex);
	}
	if (!child_ifindex) { fprintf(stderr, "child %s missing after create\n", child_name); close(fd); return 1; }

	/* 2. one client-setup (the WDA handshake trigger) */
	build_setup_msg(buf, sizeof(buf), child_ifindex, real_ifindex, mux_id, ep_type, iface_id, flags, &len);
	dump_hex("sending changelink", buf, len);
	rc = send(fd, buf, len, 0);
	if (rc < 0) { perror("setlink send"); close(fd); return 1; }
	rc = recv(fd, rbuf, sizeof(rbuf), 0);
	if (rc > 0) {
		struct nlmsghdr *rn = (struct nlmsghdr *)rbuf;
		if (rn->nlmsg_type == NLMSG_ERROR) {
			struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(rn);
			if (e->error) { fprintf(stderr, "setlink ACK error: %s\n", strerror(-e->error)); close(fd); return 1; }
			printf("client-setup ACK ok (rtnetlink accepted)\n");
		}
	}
	printf("NOTE: rtnetlink ACK only proves rmnet_changelink accepted the message.\n");
	printf("Confirm the WDA handshake separately (dmesg: 'Connection established with the WDA Service').\n");
	close(fd);
	return 0;
}

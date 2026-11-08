/* rmnet-rtm — RTM_NEWLINK(changelink) on an existing rmnet VND, reproducing
 * stock netmgrd's rtrmnet_ctl_changevnd + rtrmnet_set_uplink_aggregation_params.
 *
 *   rmnet-rtm <vnd_name> <phys_name> <flags_hex> <mask_hex> <agg_size> <agg_count> <agg_features> <agg_time>
 *
 * Example (stock msmnile):
 *   rmnet-rtm rmnet_data1 rmnet_ipa0 0xC000000D 0xC000000D 16384 32 0 2987712
 *
 * Sends: nlmsghdr(RTM_NEWLINK) + ifinfomsg(ifi_index=vnd) + IFLA_IFNAME(vnd)
 *        + IFLA_LINK(phys) + IFLA_LINKINFO{ KIND="rmnet", DATA{
 *          FLAGS(len 8: flags, mask), UL_AGG_PARAMS(len 8: u16 size, u8 count,
 *          u8 features, u32 time) } }
 * Exits 0 iff NLMSG_ERROR err == 0.
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
#include <net/if.h>
#include <stdlib.h>

#ifndef IFLA_LINKINFO
#define IFLA_LINKINFO 18
#endif
#ifndef IFLA_INFO_KIND
#define IFLA_INFO_KIND 1
#define IFLA_INFO_DATA 2
#endif
#ifndef IFLA_LINK
#define IFLA_LINK 5
#endif
#define IFLA_RMNET_MUX_ID 1
#define IFLA_RMNET_FLAGS 2
#define IFLA_RMNET_UL_AGG_PARAMS 4

#define NLMSG_TAIL(nmsg) ((struct rtattr *)(((void *)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

static void addattr(struct nlmsghdr *n, int type, const void *data, int alen)
{
	struct rtattr *rta = NLMSG_TAIL(n);
	rta->rta_type = type;
	rta->rta_len = RTA_LENGTH(alen);
	if (alen)
		memcpy(RTA_DATA(rta), data, alen);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(rta->rta_len);
}

static struct rtattr *nest_start(struct nlmsghdr *n, int type)
{
	struct rtattr *nest = NLMSG_TAIL(n);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(RTA_LENGTH(0));
	nest->rta_type = type | NLA_F_NESTED;
	nest->rta_len = RTA_LENGTH(0);
	return nest;
}

static void nest_end(struct nlmsghdr *n, struct rtattr *nest)
{
	nest->rta_len = (void *)NLMSG_TAIL(n) - (void *)nest;
}

int main(int argc, char **argv)
{
	char buf[512];
	struct nlmsghdr *n = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifi;
	struct rtattr *li, *data;
	int fd, rc;
	uint32_t flags, mask, agg_time;
	uint16_t agg_size;
	uint8_t agg_count, agg_features;
	uint32_t phys_idx, vnd_idx;

	if (argc != 9) {
		fprintf(stderr, "usage: %s <vnd> <phys> <flags> <mask> <agg_size> <agg_count> <agg_features> <agg_time>\n",
			argv[0]);
		return 2;
	}
	flags = (uint32_t)strtoul(argv[3], NULL, 0);
	mask = (uint32_t)strtoul(argv[4], NULL, 0);
	agg_size = (uint16_t)strtoul(argv[5], NULL, 0);
	agg_count = (uint8_t)strtoul(argv[6], NULL, 0);
	agg_features = (uint8_t)strtoul(argv[7], NULL, 0);
	agg_time = (uint32_t)strtoul(argv[8], NULL, 0);

	phys_idx = if_nametoindex(argv[2]);
	vnd_idx = if_nametoindex(argv[1]);
	if (!phys_idx || !vnd_idx) {
		printf("if_nametoindex: %s=%u %s=%u\n", argv[2], phys_idx, argv[1], vnd_idx);
		return 1;
	}

	memset(buf, 0, sizeof(buf));
	n->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	n->nlmsg_type = RTM_NEWLINK;
	n->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	n->nlmsg_seq = 1;
	ifi = NLMSG_DATA(n);
	ifi->ifi_family = AF_UNSPEC;
	/* name-only changelink: leave ifi_index = 0 so do_setlink does NOT treat
	   index+name as a rename (which EBUSYs on an UP device). The lookup is by
	   IFLA_IFNAME; the existing rmnet_dataN is found by name. */
	ifi->ifi_index = 0;
	(void)vnd_idx;

	addattr(n, IFLA_IFNAME, argv[1], strlen(argv[1]) + 1);
	addattr(n, IFLA_LINK, &phys_idx, 4);

	li = nest_start(n, IFLA_LINKINFO);
	addattr(n, IFLA_INFO_KIND, "rmnet", 6);
	data = nest_start(n, IFLA_INFO_DATA);
	{
		uint32_t fm[2] = { flags, mask };
		uint8_t agg[8];
		addattr(n, IFLA_RMNET_FLAGS, fm, 8);
		agg[0] = agg_size & 0xff;
		agg[1] = agg_size >> 8;
		agg[2] = agg_count;
		agg[3] = agg_features;
		agg[4] = agg_time & 0xff;
		agg[5] = (agg_time >> 8) & 0xff;
		agg[6] = (agg_time >> 16) & 0xff;
		agg[7] = (agg_time >> 24) & 0xff;
		addattr(n, IFLA_RMNET_UL_AGG_PARAMS, agg, 8);
	}
	nest_end(n, data);
	nest_end(n, li);

	if (getenv("RMNET_RTM_DRYRUN")) {
		int i;
		printf("DRY-RUN, %u bytes:", n->nlmsg_len);
		for (i = 0; i < (int)n->nlmsg_len; i++)
			printf("%s%02x", (i % 16) ? " " : "\n  ", ((unsigned char *)n)[i]);
		printf("\n");
		return 0;
	}
	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0) {
		printf("socket: %s\n", strerror(errno));
		return 1;
	}
	rc = send(fd, n, n->nlmsg_len, 0);
	if (rc < 0) {
		printf("send: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	rc = recv(fd, buf, sizeof(buf), 0);
	if (rc < 0) {
		printf("recv: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	n = (struct nlmsghdr *)buf;
	if (n->nlmsg_type == NLMSG_ERROR) {
		struct nlmsgerr *err = NLMSG_DATA(n);
		printf("NLMSG_ERROR err=%d (%s)\n", err->error,
		       err->error ? strerror(-err->error) : "success");
		close(fd);
		return err->error ? 1 : 0;
	}
	printf("unexpected reply type %u\n", n->nlmsg_type);
	close(fd);
	return 1;
}

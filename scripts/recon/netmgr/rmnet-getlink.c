/* rmnet-getlink — READ-ONLY RTM_GETLINK dump of a rmnet VND's linkinfo nest.
 * Decodes IFLA_RMNET_MUX_ID, IFLA_RMNET_FLAGS, IFLA_RMNET_UL_AGG_PARAMS.
 * Usage: rmnet-getlink <ifname>
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

#define IFLA_RMNET_MUX_ID 1
#define IFLA_RMNET_FLAGS 2
#define IFLA_RMNET_UL_AGG_PARAMS 4

static void parse_nest(struct rtattr *nest, int depth);

static void parse_data(struct rtattr *rta)
{
	int len = rta->rta_len - RTA_LENGTH(0);
	struct rtattr *d = (struct rtattr *)((char *)rta + RTA_ALIGN(rta->rta_len));
	/* caller handles */
	(void)len; (void)d;
}

static void parse_info(struct rtattr *data, int len)
{
	while (len >= (int)sizeof(struct rtattr)) {
		struct rtattr *a = (struct rtattr *)data;
		int alen = a->rta_len;
		int type = a->rta_type & 0x3fff;
		if (alen < 4 || alen > len)
			break;
		void *v = (char *)data + 4;
		switch (type) {
		case IFLA_RMNET_MUX_ID:
			printf("  mux_id = %u\n", *(uint16_t *)v);
			break;
		case IFLA_RMNET_FLAGS: {
			uint32_t *fm = v;
			printf("  flags = 0x%08x  mask = 0x%08x\n", fm[0], fm[1]);
			break;
		}
		case IFLA_RMNET_UL_AGG_PARAMS: {
			uint8_t *p = v;
			uint16_t size; uint8_t count, feat; uint32_t time;
			memcpy(&size, p, 2); count = p[2]; feat = p[3]; memcpy(&time, p + 4, 4);
			printf("  ul_agg_params: size=%u count=%u features=0x%02x time=%u\n",
			       size, count, feat, time);
			break;
		}
		default:
			printf("  attr type=%d len=%d\n", type, alen);
		}
		len -= RTA_ALIGN(alen);
		data = (struct rtattr *)((char *)data + RTA_ALIGN(alen));
	}
}

int main(int argc, char **argv)
{
	char buf[4096];
	struct nlmsghdr *n = (struct nlmsghdr *)buf;
	struct ifinfomsg *ifi;
	int fd, rc;
	uint32_t idx;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <ifname>\n", argv[0]);
		return 2;
	}
	idx = if_nametoindex(argv[1]);
	if (!idx) {
		printf("no such dev %s\n", argv[1]);
		return 1;
	}
	memset(buf, 0, sizeof(buf));
	n->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	n->nlmsg_type = RTM_GETLINK;
	n->nlmsg_flags = NLM_F_REQUEST;
	n->nlmsg_seq = 1;
	ifi = NLMSG_DATA(n);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_index = idx;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0) { perror("socket"); return 1; }
	rc = send(fd, n, n->nlmsg_len, 0);
	if (rc < 0) { perror("send"); return 1; }
	rc = recv(fd, buf, sizeof(buf), 0);
	if (rc < 0) { perror("recv"); return 1; }
	n = (struct nlmsghdr *)buf;
	if (n->nlmsg_type == NLMSG_ERROR) {
		struct nlmsgerr *e = NLMSG_DATA(n);
		printf("NLMSG_ERROR err=%d (%s)\n", e->error, e->error ? strerror(-e->error) : "ok");
		return e->error ? 1 : 0;
	}
	if (n->nlmsg_type != RTM_NEWLINK) {
		printf("unexpected reply type %u\n", n->nlmsg_type);
		return 1;
	}
	ifi = NLMSG_DATA(n);
	printf("ifindex=%d flags=0x%x\n", ifi->ifi_index, ifi->ifi_flags);
	{
		struct rtattr *tb = (struct rtattr *)((char *)ifi + NLMSG_ALIGN(sizeof(*ifi)));
		int len = n->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi));
		while (len >= (int)sizeof(struct rtattr)) {
			int alen = tb->rta_len;
			int type = tb->rta_type & 0x3fff;
			if (alen < 4 || alen > len) break;
			if (type == IFLA_IFNAME)
				printf("  ifname = %s\n", (char *)tb + 4);
			else if (type == IFLA_LINK)
				printf("  link(lower) ifindex = %u\n", *(uint32_t *)((char *)tb + 4));
			else if (type == IFLA_LINKINFO) {
				/* walk to INFO_DATA */
				struct rtattr *li = (struct rtattr *)((char *)tb + 4);
				int llen = alen - 4;
				while (llen >= (int)sizeof(struct rtattr)) {
					int lalen = li->rta_len;
					int ltype = li->rta_type & 0x3fff;
					if (lalen < 4 || lalen > llen) break;
					if (ltype == 1) /* KIND */
						printf("  kind = %s\n", (char *)li + 4);
					else if (ltype == 2) /* DATA */
						parse_info((struct rtattr *)((char *)li + 4), lalen - 4);
					llen -= RTA_ALIGN(lalen);
					li = (struct rtattr *)((char *)li + RTA_ALIGN(lalen));
				}
			}
			len -= RTA_ALIGN(alen);
			tb = (struct rtattr *)((char *)tb + RTA_ALIGN(alen));
		}
	}
	return 0;
}

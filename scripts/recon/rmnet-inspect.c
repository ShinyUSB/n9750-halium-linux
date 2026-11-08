/*
 * rmnet-inspect: READ-ONLY inspector for the rmnet_ipa0 extended ioctls.
 * Calls only GET_* sub-commands (GET_SUPPORTED_FEATURES, GET_DRIVER_NAME,
 * GET_EPID, GET_EP_PAIR). Never sends SET/ADD/DELETE/modifying ioctls.
 *
 * Build on the phone: gcc -O2 -o rmnet-inspect rmnet-inspect.c
 * Run: rmnet-inspect [ifname]   (default rmnet_ipa0)
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>

#define RMNET_IOCTL_EXTENDED 0x000089FD

/* sub-commands (GET only) */
#define GET_SUPPORTED_FEATURES 0x0000
#define GET_EPID               0x0003
#define GET_DRIVER_NAME        0x0004
#define GET_EP_PAIR            0x0010

#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif

struct rmnet_ioctl_extended_s {
	uint32_t extended_ioctl;
	union {
		uint32_t data;
		int8_t if_name[IFNAMSIZ];
		struct { uint32_t mux_id; int8_t vchannel_name[IFNAMSIZ]; } rmnet_mux_val;
		struct { uint32_t consumer_pipe_num; uint32_t producer_pipe_num; } ipa_ep_pair;
		struct { uint32_t __data; uint32_t agg_size; uint32_t agg_count; } ingress_format;
	} u;
};

static int do_ioctl(int fd, const char *ifn, uint32_t sub, struct rmnet_ioctl_extended_s *io)
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	memset(io, 0, sizeof(*io));
	io->extended_ioctl = sub;
	strncpy(ifr.ifr_name, ifn, IFNAMSIZ - 1);
	ifr.ifr_data = (void *)io;
	return ioctl(fd, RMNET_IOCTL_EXTENDED, &ifr);
}

int main(int argc, char **argv)
{
	const char *ifn = argc > 1 ? argv[1] : "rmnet_ipa0";
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) { perror("socket"); return 1; }
	struct rmnet_ioctl_extended_s io;
	int rc;

	printf("== rmnet extended ioctls on %s (READ-ONLY) ==\n", ifn);

	/* GET_SUPPORTED_FEATURES */
	rc = do_ioctl(fd, ifn, GET_SUPPORTED_FEATURES, &io);
	if (rc == 0) printf("SUPPORTED_FEATURES: 0x%x\n", io.u.data);
	else printf("SUPPORTED_FEATURES: rc=%d (%s)\n", rc, strerror(errno));

	/* GET_DRIVER_NAME */
	rc = do_ioctl(fd, ifn, GET_DRIVER_NAME, &io);
	if (rc == 0) printf("DRIVER_NAME: %s\n", io.u.if_name);
	else printf("DRIVER_NAME: rc=%d (%s)\n", rc, strerror(errno));

	/* GET_EPID */
	rc = do_ioctl(fd, ifn, GET_EPID, &io);
	if (rc == 0) printf("EPID: 0x%x\n", io.u.data);
	else printf("EPID: rc=%d (%s)\n", rc, strerror(errno));

	/* GET_EP_PAIR (consumer/producer pipe numbers) */
	rc = do_ioctl(fd, ifn, GET_EP_PAIR, &io);
	if (rc == 0) printf("EP_PAIR: consumer=%u producer=%u\n",
		io.u.ipa_ep_pair.consumer_pipe_num, io.u.ipa_ep_pair.producer_pipe_num);
	else printf("EP_PAIR: rc=%d (%s)\n", rc, strerror(errno));

	close(fd);
	return 0;
}

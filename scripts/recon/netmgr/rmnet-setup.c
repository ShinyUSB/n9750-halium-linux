/* rmnet-setup — issue RMNET_IOCTL_EXTENDED sub-ioctls on rmnet_ipa0,
 * reproducing stock netmgrd's netmgr_kif_ifioctl_* path.
 *
 * Usage:
 *   rmnet-setup features                (GET_SUPPORTED_FEATURES, read-only)
 *   rmnet-setup eppair                  (GET_EP_PAIR, read-only)
 *   rmnet-setup addmux <mux_id> <vname> (ADD_MUX_CHANNEL)
 *   rmnet-setup egress <flags_hex>      (SET_EGRESS_DATA_FORMAT, u.data=flags)
 *   rmnet-setup ingress <flags_hex> <agg_size> <agg_count>
 *                                       (SET_INGRESS_DATA_FORMAT)
 *
 * All writes go through: socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC, 0),
 * ifreq{ifr_name="rmnet_ipa0", ifr_data=&ext}, ioctl(fd, 0x89FD, &ifr).
 * Exit 0 iff ioctl rc == 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>

#define RMNET_IOCTL_EXTENDED 0x89FD

/* from include/uapi/linux/msm_rmnet.h */
struct rmnet_ioctl_extended_s {
	uint32_t extended_ioctl;
	union {
		uint32_t data;
		int8_t if_name[IFNAMSIZ];
		struct {
			uint32_t mux_id;
			int8_t vchannel_name[IFNAMSIZ];
		} rmnet_mux_val;
		struct {
			uint8_t flow_mode;
			uint8_t mux_id;
		} flow_control_prop;
		struct {
			uint32_t consumer_pipe_num;
			uint32_t producer_pipe_num;
		} ipa_ep_pair;
		struct {
			uint32_t __data;
			uint32_t agg_size;
			uint32_t agg_count;
		} ingress_format;
		struct {
			uint32_t flags;
			uint8_t mux_id;
		} offload_params;
	} u;
};

#define EXT_GET_FEATURES 0x0000
#define EXT_GET_EP_PAIR  0x0010
#define EXT_ADD_MUX      0x0005
#define EXT_SET_EGRESS   0x0006
#define EXT_SET_INGRESS  0x0007

static int do_ioctl(const char *dev, struct rmnet_ioctl_extended_s *ext)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct ifreq ifr;
	int rc;

	if (fd < 0) {
		printf("socket: %s\n", strerror(errno));
		return -1;
	}
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", dev);
	ifr.ifr_data = (void *)ext;
	rc = ioctl(fd, RMNET_IOCTL_EXTENDED, &ifr);
	if (rc < 0)
		printf("ioctl(0x89FD, sub=0x%x) on %s: rc=%d errno=%d (%s)\n",
		       ext->extended_ioctl, dev, rc, errno, strerror(errno));
	close(fd);
	return rc;
}

int main(int argc, char **argv)
{
	struct rmnet_ioctl_extended_s ext;
	const char *dev = "rmnet_ipa0";
	int rc;

	if (argc < 2) {
		fprintf(stderr, "usage: %s features|eppair|addmux <id> <vname>|"
			"egress <hex>|ingress <hex> <agg_size> <agg_count>\n", argv[0]);
		return 2;
	}
	memset(&ext, 0, sizeof(ext));

	if (!strcmp(argv[1], "features")) {
		ext.extended_ioctl = EXT_GET_FEATURES;
		rc = do_ioctl(dev, &ext);
		printf("features rc=%d value=0x%x\n", rc, ext.u.data);
		return rc ? 1 : 0;
	}
	if (!strcmp(argv[1], "eppair")) {
		ext.extended_ioctl = EXT_GET_EP_PAIR;
		rc = do_ioctl(dev, &ext);
		printf("eppair rc=%d consumer=%u producer=%u\n", rc,
		       ext.u.ipa_ep_pair.consumer_pipe_num,
		       ext.u.ipa_ep_pair.producer_pipe_num);
		return rc ? 1 : 0;
	}
	if (!strcmp(argv[1], "addmux") && argc == 4) {
		ext.extended_ioctl = EXT_ADD_MUX;
		ext.u.rmnet_mux_val.mux_id = (uint32_t)strtoul(argv[2], NULL, 0);
		snprintf(ext.u.rmnet_mux_val.vchannel_name, IFNAMSIZ, "%s", argv[3]);
		rc = do_ioctl(dev, &ext);
		printf("addmux mux=%u vname=%s rc=%d\n",
		       ext.u.rmnet_mux_val.mux_id, ext.u.rmnet_mux_val.vchannel_name, rc);
		return rc ? 1 : 0;
	}
	if (!strcmp(argv[1], "egress") && argc == 3) {
		ext.extended_ioctl = EXT_SET_EGRESS;
		ext.u.data = (uint32_t)strtoul(argv[2], NULL, 0);
		rc = do_ioctl(dev, &ext);
		printf("egress flags=0x%x rc=%d\n", ext.u.data, rc);
		return rc ? 1 : 0;
	}
	if (!strcmp(argv[1], "ingress") && argc == 5) {
		ext.extended_ioctl = EXT_SET_INGRESS;
		ext.u.ingress_format.__data = (uint32_t)strtoul(argv[2], NULL, 0);
		ext.u.ingress_format.agg_size = (uint32_t)strtoul(argv[3], NULL, 0);
		ext.u.ingress_format.agg_count = (uint32_t)strtoul(argv[4], NULL, 0);
		rc = do_ioctl(dev, &ext);
		printf("ingress flags=0x%x agg_size=%u agg_count=%u rc=%d\n",
		       ext.u.ingress_format.__data, ext.u.ingress_format.agg_size,
		       ext.u.ingress_format.agg_count, rc);
		return rc ? 1 : 0;
	}
	fprintf(stderr, "bad args\n");
	return 2;
}

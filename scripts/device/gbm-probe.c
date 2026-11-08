/* gbm-probe: reproduce the wlroots gbm allocator path on d2q card0 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <gbm.h>
#include <xf86drm.h>

int main(int argc, char **argv)
{
	const char *node = argc > 1 ? argv[1] : "/dev/dri/card0";
	unsigned usage = GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT;
	if (argc > 2) usage = strtoul(argv[2], NULL, 0);
	int fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) { printf("open %s: %s\n", node, strerror(errno)); return 1; }
	drmVersion *v = drmGetVersion(fd);
	printf("drm node %s: driver=%s version=%d.%d.%d\n", node,
	       v ? v->name : "?", v ? v->version_major : -1,
	       v ? v->version_minor : -1, v ? v->version_patchlevel : -1);
	if (v) drmFreeVersion(v);

	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm) { printf("gbm_create_device failed: %s\n", strerror(errno)); return 1; }
	printf("gbm backend: %s\n", gbm_device_get_backend_name(gbm));

	/* what wlroots asks for: renderable + scanout 32bpp */
	printf("usage=0x%x (R=1 S=4 L=16 T=32)\n", usage);
	struct gbm_bo *bo = gbm_bo_create(gbm, 64, 64, GBM_FORMAT_ARGB8888, usage);
	if (!bo) { printf("gbm_bo_create: %s\n", strerror(errno)); return 1; }
	printf("bo created: %dx%d stride=%u\n", gbm_bo_get_width(bo),
	       gbm_bo_get_height(bo), gbm_bo_get_stride(bo));

	int fd_bo = gbm_bo_get_fd_for_plane(bo, 0);
	if (fd_bo < 0) {
		printf("gbm_bo_get_fd_for_plane: %s (errno=%d)\n", strerror(errno), errno);
		/* legacy API fallback — what wlroots 0.16 does NOT try */
		fd_bo = gbm_bo_get_fd(bo);
		if (fd_bo >= 0) printf("legacy gbm_bo_get_fd: WORKS fd=%d\n", fd_bo);
		else printf("legacy gbm_bo_get_fd: %s\n", strerror(errno));
	} else {
		printf("gbm_bo_get_fd_for_plane: WORKS fd=%d\n", fd_bo);
	}
	if (fd_bo >= 0) close(fd_bo);
	gbm_bo_destroy(bo);
	gbm_device_destroy(gbm);
	close(fd);
	return 0;
}

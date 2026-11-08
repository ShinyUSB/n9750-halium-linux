/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fb_helper.h>

#include <linux/workqueue.h>

#include "msm_drv.h"
#include "msm_kms.h"

extern int msm_gem_mmap_obj(struct drm_gem_object *obj,
					struct vm_area_struct *vma);
static int msm_fbdev_mmap(struct fb_info *info, struct vm_area_struct *vma);

/*
 * fbdev funcs, to implement legacy fbdev interface on top of drm driver
 */

#define to_msm_fbdev(x) container_of(x, struct msm_fbdev, base)

struct msm_fbdev {
	struct drm_fb_helper base;
	struct drm_framebuffer *fb;
};

static struct fb_ops msm_fb_ops = {
	.owner = THIS_MODULE,
	DRM_FB_HELPER_DEFAULT_OPS,

	/* Note: to properly handle manual update displays, we wrap the
	 * basic fbdev ops which write to the framebuffer
	 */
	.fb_read = drm_fb_helper_sys_read,
	.fb_write = drm_fb_helper_sys_write,
	.fb_fillrect = drm_fb_helper_sys_fillrect,
	.fb_copyarea = drm_fb_helper_sys_copyarea,
	.fb_imageblit = drm_fb_helper_sys_imageblit,
	.fb_mmap = msm_fbdev_mmap,
};

static int msm_fbdev_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct drm_fb_helper *helper = (struct drm_fb_helper *)info->par;
	struct msm_fbdev *fbdev = to_msm_fbdev(helper);
	struct drm_gem_object *bo = msm_framebuffer_bo(fbdev->fb, 0);
	int ret = 0;

	ret = drm_gem_mmap_obj(bo, bo->size, vma);
	if (ret) {
		pr_err("%s:drm_gem_mmap_obj fail\n", __func__);
		return ret;
	}

	return msm_gem_mmap_obj(bo, vma);
}

static int msm_fbdev_create(struct drm_fb_helper *helper,
		struct drm_fb_helper_surface_size *sizes)
{
	struct msm_fbdev *fbdev = to_msm_fbdev(helper);
	struct drm_device *dev = helper->dev;
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_framebuffer *fb = NULL;
	struct drm_gem_object *bo;
	struct fb_info *fbi = NULL;
	uint64_t paddr;
	uint32_t format;
	int ret, pitch;

	format = drm_mode_legacy_fb_format(sizes->surface_bpp, sizes->surface_depth);

	DBG("create fbdev: %dx%d@%d (%dx%d)", sizes->surface_width,
			sizes->surface_height, sizes->surface_bpp,
			sizes->fb_width, sizes->fb_height);

	pitch = align_pitch(sizes->surface_width, sizes->surface_bpp);
	fb = msm_alloc_stolen_fb(dev, sizes->surface_width,
			sizes->surface_height, pitch, format);

	if (IS_ERR(fb)) {
		dev_err(dev->dev, "failed to allocate fb\n");
		return PTR_ERR(fb);
	}

	bo = msm_framebuffer_bo(fb, 0);

	mutex_lock(&dev->struct_mutex);

	/*
	 * NOTE: if we can be guaranteed to be able to map buffer
	 * in panic (ie. lock-safe, etc) we could avoid pinning the
	 * buffer now:
	 */
	ret = msm_gem_get_iova(bo, priv->kms->aspace, &paddr);
	if (ret) {
		dev_err(dev->dev, "failed to get buffer obj iova: %d\n", ret);
		goto fail_unlock;
	}

	fbi = drm_fb_helper_alloc_fbi(helper);
	if (IS_ERR(fbi)) {
		dev_err(dev->dev, "failed to allocate fb info\n");
		ret = PTR_ERR(fbi);
		goto fail_unlock;
	}

	DBG("fbi=%pK, dev=%pK", fbi, dev);

	fbdev->fb = fb;
	helper->fb = fb;

	fbi->par = helper;
	fbi->fbops = &msm_fb_ops;

	strcpy(fbi->fix.id, "msm");

	drm_fb_helper_fill_fix(fbi, fb->pitches[0], fb->format->depth);
	drm_fb_helper_fill_var(fbi, helper, sizes->fb_width, sizes->fb_height);

	dev->mode_config.fb_base = paddr;

	fbi->screen_base = msm_gem_get_vaddr(bo);
	if (IS_ERR(fbi->screen_base)) {
		ret = PTR_ERR(fbi->screen_base);
		goto fail_unlock;
	}
	fbi->screen_size = bo->size;
	fbi->fix.smem_start = paddr;
	fbi->fix.smem_len = bo->size;

	DBG("par=%pK, %dx%d", fbi->par, fbi->var.xres, fbi->var.yres);
	DBG("allocated %dx%d fb", fbdev->fb->width, fbdev->fb->height);

	mutex_unlock(&dev->struct_mutex);

	return 0;

fail_unlock:
	mutex_unlock(&dev->struct_mutex);
	drm_framebuffer_remove(fb);
	return ret;
}

static const struct drm_fb_helper_funcs msm_fb_helper_funcs = {
	.fb_probe = msm_fbdev_create,
};

/*
 * d2q kick (async): this cmd-mode AMOLED only shows a frame when an atomic
 * commit kicks the DPU, and userspace flushes frontbuffer rendering through
 * DRM_IOCTL_MODE_DIRTYFB from Xorg's block handler.  The sync-kick version
 * (image 299) committed the plane re-assert inline, which cost one TE
 * interval (~8-17 ms measured) INSIDE Xorg's dispatch cycle: under
 * high-rate mouse/touch motion the flushes alone saturated the server
 * loop, client drawing and present copies queued behind them, and the
 * screen froze until the cursor stopped (MC menu freeze, lagging selection
 * box) even though every individual commit succeeded.
 *
 * The kick is therefore ASYNCHRONOUS and COLLAPSED: the dirtyfb handler
 * only arms a work item; a dedicated single-threaded workqueue performs
 * the plane re-assert (a plane-only atomic commit, no modeset, no fbcon
 * restore) at most one per burst, bounded by the panel's TE rate.  Xorg's
 * dirtyfb returns in microseconds and dispatch runs at full input rate
 * while the panel refreshes from whatever glamor has already written into
 * the scanned fb.  If no CRTC is active (DPMS off) the kick is skipped;
 * if the dirtied fb is not on any plane yet (fbcon-era handoff), fall back
 * to the proven fbcon restore.
 */
static struct workqueue_struct *d2q_kick_wq;
static struct drm_device *d2q_kick_dev;
static atomic_t d2q_kick_req = ATOMIC_INIT(0);
static unsigned long d2q_kick_armed;
static int d2q_kick_logs;
static atomic_t d2q_kick_in_progress = ATOMIC_INIT(0);

static void d2q_kick_work_fn(struct work_struct *work);

static DECLARE_DELAYED_WORK(d2q_kick_work, d2q_kick_work_fn);
/* d2q: shared with msm_atomic.c so every atomic commit (wlroots, apps)
 * gets the same presentation kick the dirtyfb path provides. */
void d2q_kick_arm(struct drm_device *dev)
{
	d2q_kick_dev = dev;
	atomic_inc(&d2q_kick_req);
	if (d2q_kick_wq)
		mod_delayed_work(d2q_kick_wq, &d2q_kick_work,
				 msecs_to_jiffies(20));
}

bool d2q_kick_active(void)
{
	return atomic_read(&d2q_kick_in_progress) != 0;
}

static void d2q_kick_work_fn(struct work_struct *work)
{
	struct drm_device *dev = d2q_kick_dev;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct drm_plane *plane;
	int ret;

	if (!dev)
		goto done;

	do {
		atomic_set(&d2q_kick_req, 0);

		state = drm_atomic_state_alloc(dev);
		if (!state)
			break;

		drm_modeset_acquire_init(&ctx, 0);
		state->acquire_ctx = &ctx;

retry:
		drm_for_each_plane(plane, dev) {
			struct drm_plane_state *plane_state;

			if (!plane->state || !plane->state->crtc ||
			    !plane->state->fb)
				continue;
			plane_state = drm_atomic_get_plane_state(state, plane);
			if (IS_ERR(plane_state)) {
				ret = PTR_ERR(plane_state);
				goto out;
			}
		}

		atomic_set(&d2q_kick_in_progress, 1);
		ret = drm_atomic_commit(state);
		atomic_set(&d2q_kick_in_progress, 0);
		if (ret == -EDEADLK) {
			drm_atomic_state_clear(state);
			drm_modeset_backoff(&ctx);
			goto retry;
		}

		if (ret && d2q_kick_logs < 20) {
			d2q_kick_logs++;
			pr_info("D2QDIRTY: async kick ret=%d\n", ret);
		}
out:
		drm_atomic_state_put(state);
		drm_modeset_drop_locks(&ctx);
		drm_modeset_acquire_fini(&ctx);
	} while (atomic_read(&d2q_kick_req) > 0);

done:
	clear_bit(0, &d2q_kick_armed);
	/* raced request; re-arm shortly (collapsed) */
	if (atomic_read(&d2q_kick_req) > 0 && d2q_kick_wq)
		mod_delayed_work(d2q_kick_wq, &d2q_kick_work,
				 msecs_to_jiffies(5));
}


/* initialize fbdev helper */
struct drm_fb_helper *msm_fbdev_init(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_fbdev *fbdev = NULL;
	struct drm_fb_helper *helper;
	int ret;

	d2q_kick_wq = alloc_workqueue("d2qkick", 0, 1);

	fbdev = kzalloc(sizeof(*fbdev), GFP_KERNEL);
	if (!fbdev)
		goto fail;

	helper = &fbdev->base;

	drm_fb_helper_prepare(dev, helper, &msm_fb_helper_funcs);

	ret = drm_fb_helper_init(dev, helper, priv->num_connectors);
	if (ret) {
		dev_err(dev->dev, "could not init fbdev: ret=%d\n", ret);
		goto fail;
	}

	ret = drm_fb_helper_single_add_all_connectors(helper);
	if (ret)
		goto fini;

	ret = drm_fb_helper_initial_config(helper, 32);
	if (ret)
		goto fini;

	priv->fbdev = helper;

	return helper;

fini:
	drm_fb_helper_fini(helper);
fail:
	kfree(fbdev);
	return NULL;
}

int msm_fbdev_dirty(struct drm_framebuffer *fb,
		struct drm_file *file_priv, unsigned int flags,
		unsigned int color, struct drm_clip_rect *clips,
		unsigned int num_clips)
{
	struct drm_device *dev = fb->dev;
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_crtc *crtc;
	struct drm_plane *plane;
	bool crtc_on = false, fb_on_plane = false;

	if (!priv || !priv->fbdev)
		return -ENODEV;

	drm_for_each_crtc(crtc, dev)
		if (crtc->state && crtc->state->enable && crtc->state->active)
			crtc_on = true;

	if (!crtc_on)
		return 0;   /* DPMS off: nothing to kick */

	drm_for_each_plane(plane, dev)
		if (plane->state && plane->state->crtc &&
		    plane->state->fb == fb)
			fb_on_plane = true;

	if (!fb_on_plane) {
		/* fb not scanned by any plane: legacy fbcon-era fallback */
		return drm_fb_helper_restore_fbdev_mode_unlocked(priv->fbdev);
	}

	d2q_kick_arm(dev);

	return 0;
}

void msm_fbdev_free(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_fb_helper *helper = priv->fbdev;
	struct msm_fbdev *fbdev;

	DBG();

	if (d2q_kick_wq) {
		destroy_workqueue(d2q_kick_wq);
		d2q_kick_wq = NULL;
	}

	drm_fb_helper_unregister_fbi(helper);

	drm_fb_helper_fini(helper);

	fbdev = to_msm_fbdev(priv->fbdev);

	/* this will free the backing object */
	if (fbdev->fb) {
		struct drm_gem_object *bo =
			msm_framebuffer_bo(fbdev->fb, 0);
		msm_gem_put_vaddr(bo);
		drm_framebuffer_remove(fbdev->fb);
	}

	kfree(fbdev);

	priv->fbdev = NULL;
}

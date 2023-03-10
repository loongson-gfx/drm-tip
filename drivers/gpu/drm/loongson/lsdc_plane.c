// SPDX-License-Identifier: GPL-2.0

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_plane_helper.h>

#include "lsdc_drv.h"
#include "lsdc_regs.h"
#include "lsdc_ttm.h"

static const u32 lsdc_primary_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const u32 lsdc_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

static const u64 lsdc_fb_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static int lsdc_cursor_atomic_check(struct drm_plane *plane,
				   struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc *crtc = new_plane_state->crtc;
	struct drm_crtc_state *new_crtc_state;

	if (!crtc)
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	return drm_atomic_helper_check_plane_state(new_plane_state,
						   new_crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   true,
						   true);
}

static int lsdc_primary_atomic_check(struct drm_plane *plane,
				     struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc *crtc = new_plane_state->crtc;
	struct drm_crtc_state *new_crtc_state;

	if (!crtc)
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	return drm_atomic_helper_check_plane_state(new_plane_state,
						   new_crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false,
						   true);
}

static unsigned int lsdc_get_fb_offset(struct drm_framebuffer *fb,
				       struct drm_plane_state *state)
{
	unsigned int offset = fb->offsets[0];

	offset += fb->format->cpp[0] * (state->src_x >> 16);
	offset += fb->pitches[0] * (state->src_y >> 16);

	return offset;
}

static void lsdc_primary_atomic_update(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc *crtc = new_plane_state->crtc;
	struct drm_framebuffer *fb = new_plane_state->fb;
	struct ttm_buffer_object *tbo = to_ttm_bo(fb->obj[0]);
	unsigned int pipe = drm_crtc_index(crtc);
	unsigned int fb_offset = lsdc_get_fb_offset(fb, new_plane_state);
	u64 bo_offset = lsdc_bo_gpu_offset(tbo);
	u64 fb_addr = ldev->vram_base + bo_offset + fb_offset;
	u32 stride = fb->pitches[0];
	u32 cfg;
	u32 lo, hi;

	/* 40-bit width physical address bus */
	lo = fb_addr & 0xFFFFFFFF;
	hi = (fb_addr >> 32) & 0xFF;

	cfg = lsdc_crtc_rreg32(ldev, LSDC_CRTC0_CFG_REG, pipe);
	if (cfg & CFG_FB_IN_USING) {
		lsdc_crtc_wreg32(ldev, LSDC_CRTC0_FB1_LO_ADDR_REG, pipe, lo);
		lsdc_crtc_wreg32(ldev, LSDC_CRTC0_FB1_HI_ADDR_REG, pipe, hi);
	} else {
		lsdc_crtc_wreg32(ldev, LSDC_CRTC0_FB0_LO_ADDR_REG, pipe, lo);
		lsdc_crtc_wreg32(ldev, LSDC_CRTC0_FB0_HI_ADDR_REG, pipe, hi);
	}

	lsdc_crtc_wreg32(ldev, LSDC_CRTC0_STRIDE_REG, pipe, stride);

	cfg &= ~CFG_PIX_FMT_MASK;
	cfg |= LSDC_PF_XRGB8888;

	lsdc_crtc_wreg32(ldev, LSDC_CRTC0_CFG_REG, pipe, cfg);
}

static void lsdc_primary_atomic_disable(struct drm_plane *plane,
					struct drm_atomic_state *state)
{
	/* Do nothing, just prevent call into atomic_update().
	 * Writing the format as LSDC_PF_NONE can disable the primary,
	 * But it seems not necessary...
	 */
	drm_dbg(plane->dev, "%s disabled\n", plane->name);
}

static int lsdc_plane_prepare_fb(struct drm_plane *plane,
				 struct drm_plane_state *new_state)
{
	struct drm_framebuffer *fb = new_state->fb;
	struct lsdc_bo *lbo;
	struct drm_gem_object *obj;
	u32 flags = TTM_PL_FLAG_CONTIGUOUS;
	int ret;

	if (!fb)
		return 0;

	if (plane->type == DRM_PLANE_TYPE_CURSOR)
		flags |= TTM_PL_FLAG_TOPDOWN;

	obj = fb->obj[0];
	if (!obj) {
		drm_err(plane->dev, "%s: no obj\n", plane->name);
		return -EINVAL;
	}

	lbo = gem_to_lsdc_bo(obj);

	lsdc_bo_set_placement(lbo, LSDC_GEM_DOMAIN_VRAM, flags);

	ret = lsdc_bo_pin(obj);
	if (ret)
		return ret;

	ret = drm_gem_plane_helper_prepare_fb(plane, new_state);
	if (ret)
		lsdc_bo_unpin(obj);

	return 0;
}

static void lsdc_plane_cleanup_fb(struct drm_plane *plane,
				  struct drm_plane_state *old_state)
{
	struct drm_framebuffer *fb = old_state->fb;

	if (!fb)
		return;

	lsdc_bo_unpin(fb->obj[0]);
}

static const struct drm_plane_helper_funcs lsdc_primary_helper_funcs = {
	.prepare_fb = lsdc_plane_prepare_fb,
	.cleanup_fb = lsdc_plane_cleanup_fb,
	.atomic_check = lsdc_primary_atomic_check,
	.atomic_update = lsdc_primary_atomic_update,
	.atomic_disable = lsdc_primary_atomic_disable,
};

/*
 * Update location, format, enable and disable state of the cursor,
 * For those who have two hardware cursor, cursor 0 is attach it to CRTC-0,
 * cursor 1 is attached to CRTC-1. Compositing the primary and cursor plane
 * is automatically done by hardware, the cursor is alway on the top of the
 * primary, there is no depth property can be set, pretty convenient.
 */
static void ls7a1000_cursor_atomic_update(struct drm_plane *plane,
					  struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct lsdc_display_pipe *dispipe = cursor_to_display_pipe(plane);
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *cursor_fb = new_plane_state->fb;
	struct ttm_buffer_object *tbo = to_ttm_bo(cursor_fb->obj[0]);
	u64 addr = ldev->vram_base + lsdc_bo_gpu_offset(tbo);
	u32 cfg = CURSOR_FORMAT_ARGB8888;
	int x = new_plane_state->crtc_x;
	int y = new_plane_state->crtc_y;

	if (x < 0)
		x = 0;

	if (y < 0)
		y = 0;

	lsdc_wreg32(ldev, LSDC_CURSOR0_POSITION_REG, (y << 16) | x);

	lsdc_wreg32(ldev, LSDC_CURSOR0_ADDR_HI_REG, (addr >> 32) & 0xFF);
	lsdc_wreg32(ldev, LSDC_CURSOR0_ADDR_LO_REG, addr);

	/*
	 * If bit 4(CURSOR_LOCATION) of LSDC_CURSOR0_CFG_REG is 1, cursor will
	 * be locate at CRTC-1, if bit 4 of LSDC_CURSOR0_CFG_REG is 0, cursor
	 * will be locate at CRTC-0. For the old device we made the single hw
	 * cursor shared by two CRTC. Switch to software cursor is also ok.
	 */
	lsdc_wreg32(ldev, LSDC_CURSOR0_CFG_REG, dispipe->index ? cfg | CURSOR_LOCATION : cfg);
}

static void ls7a1000_cursor_atomic_disable(struct drm_plane *plane,
					   struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	u32 cfg;

	cfg = lsdc_rreg32(ldev, LSDC_CURSOR0_CFG_REG);
	/* write 0 to cursor format bits, it will be invisiable */
	cfg &= ~CURSOR_FORMAT_MASK;
	lsdc_wreg32(ldev, LSDC_CURSOR0_CFG_REG, cfg);
}

static const struct drm_plane_helper_funcs ls7a1000_cursor_helper_funcs = {
	.prepare_fb = lsdc_plane_prepare_fb,
	.cleanup_fb = lsdc_plane_cleanup_fb,
	.atomic_check = lsdc_cursor_atomic_check,
	.atomic_update = ls7a1000_cursor_atomic_update,
	.atomic_disable = ls7a1000_cursor_atomic_disable,
};

/* update the format, size and location of the cursor */
static void lsdc_cursor0_atomic_update(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *cursor_fb = new_plane_state->fb;
	struct ttm_buffer_object *tbo = to_ttm_bo(cursor_fb->obj[0]);
	u64 addr = ldev->vram_base + lsdc_bo_gpu_offset(tbo);
	int x = new_plane_state->crtc_x;
	int y = new_plane_state->crtc_y;

	if (x < 0)
		x = 0;

	if (y < 0)
		y = 0;

	lsdc_wreg32(ldev, LSDC_CURSOR0_POSITION_REG, (y << 16) | x);

	lsdc_wreg32(ldev, LSDC_CURSOR0_ADDR_HI_REG, (addr >> 32) & 0xFF);
	lsdc_wreg32(ldev, LSDC_CURSOR0_ADDR_LO_REG, addr);

	lsdc_wreg32(ldev, LSDC_CURSOR0_CFG_REG, CURSOR_FORMAT_ARGB8888 | CURSOR_SIZE_64X64);
}

/* update the format, size and location of the cursor */
static void lsdc_cursor1_atomic_update(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *cursor_fb = new_plane_state->fb;
	struct ttm_buffer_object *tbo = to_ttm_bo(cursor_fb->obj[0]);
	u64 addr = ldev->vram_base + lsdc_bo_gpu_offset(tbo);
	int x = new_plane_state->crtc_x;
	int y = new_plane_state->crtc_y;

	if (x < 0)
		x = 0;

	if (y < 0)
		y = 0;

	lsdc_wreg32(ldev, LSDC_CURSOR1_POSITION_REG, (y << 16) | x);

	lsdc_wreg32(ldev, LSDC_CURSOR1_ADDR_HI_REG, (addr >> 32) & 0xFF);
	lsdc_wreg32(ldev, LSDC_CURSOR1_ADDR_LO_REG, addr);

	lsdc_wreg32(ldev, LSDC_CURSOR1_CFG_REG,
		     CURSOR_FORMAT_ARGB8888 | CURSOR_SIZE_64X64 | CURSOR_LOCATION);
}

static void lsdc_cursor0_atomic_disable(struct drm_plane *plane,
					struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	u32 cfg;

	cfg = lsdc_rreg32(ldev, LSDC_CURSOR0_CFG_REG);
	/* write 0 to cursor format bits, it will be invisiable */
	cfg &= ~CURSOR_FORMAT_MASK;
	lsdc_wreg32(ldev, LSDC_CURSOR0_CFG_REG, cfg);
}

static void lsdc_cursor1_atomic_disable(struct drm_plane *plane,
					struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	u32 cfg;

	cfg = lsdc_rreg32(ldev, LSDC_CURSOR1_CFG_REG);
	/* write 0 to cursor format bits, it will be invisiable */
	cfg &= ~CURSOR_FORMAT_MASK;
	lsdc_wreg32(ldev, LSDC_CURSOR1_CFG_REG, cfg);
}

static const struct drm_plane_helper_funcs ls7a2000_cursor_helper_funcs[2] = {
	{
		.prepare_fb = lsdc_plane_prepare_fb,
		.cleanup_fb = lsdc_plane_cleanup_fb,
		.atomic_check = lsdc_cursor_atomic_check,
		.atomic_update = lsdc_cursor0_atomic_update,
		.atomic_disable = lsdc_cursor0_atomic_disable,
	},
	{
		.prepare_fb = lsdc_plane_prepare_fb,
		.cleanup_fb = lsdc_plane_cleanup_fb,
		.atomic_check = lsdc_cursor_atomic_check,
		.atomic_update = lsdc_cursor1_atomic_update,
		.atomic_disable = lsdc_cursor1_atomic_disable,
	}
};

static const struct drm_plane_funcs lsdc_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

int lsdc_primary_plane_init(struct lsdc_device *ldev,
			    struct drm_plane *plane,
			    unsigned int index)
{
	int ret;

	ret = drm_universal_plane_init(&ldev->base,
				       plane,
				       1 << index,
				       &lsdc_plane_funcs,
				       lsdc_primary_formats,
				       ARRAY_SIZE(lsdc_primary_formats),
				       lsdc_fb_format_modifiers,
				       DRM_PLANE_TYPE_PRIMARY,
				       "primary-%u",
				       index);
	if (ret)
		return ret;

	drm_plane_helper_add(plane, &lsdc_primary_helper_funcs);

	return 0;
}

int lsdc_cursor_plane_init(struct lsdc_device *ldev,
			   struct drm_plane *plane,
			   unsigned int index)
{
	const struct lsdc_desc *descp = ldev->descp;
	int ret;

	ret = drm_universal_plane_init(&ldev->base,
				       plane,
				       1 << index,
				       &lsdc_plane_funcs,
				       lsdc_cursor_formats,
				       ARRAY_SIZE(lsdc_cursor_formats),
				       lsdc_fb_format_modifiers,
				       DRM_PLANE_TYPE_CURSOR,
				       "cursor-%u",
				       index);
	if (ret)
		return ret;

	/* The hw cursor become normal since ls7a2000(including ls2k2000) */
	if (descp->chip == CHIP_LS7A2000)
		drm_plane_helper_add(plane, &ls7a2000_cursor_helper_funcs[index]);
	else
		drm_plane_helper_add(plane, &ls7a1000_cursor_helper_funcs);

	return 0;
}

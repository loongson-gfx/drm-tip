// SPDX-License-Identifier: GPL-2.0

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_gem_vram_helper.h>
#include "lsdc_drv.h"
#include "lsdc_regs.h"
#include "lsdc_pll.h"

static const u32 lsdc_primary_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const u32 lsdc_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

static const u64 lsdc_fb_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static void lsdc_update_fb_format(struct lsdc_device *ldev,
				  struct drm_crtc *crtc,
				  const struct drm_format_info *fmt_info)
{
	unsigned int index = drm_crtc_index(crtc);
	u32 val;
	u32 fmt;

	switch (fmt_info->format) {
	case DRM_FORMAT_XRGB8888:
		fmt = LSDC_PF_XRGB8888;
		break;
	case DRM_FORMAT_ARGB8888:
		fmt = LSDC_PF_XRGB8888;
		break;
	default:
		fmt = LSDC_PF_XRGB8888;
		break;
	}

	if (index == 0) {
		val = lsdc_rreg32(ldev, LSDC_CRTC0_CFG_REG);
		val = (val & ~CFG_PIX_FMT_MASK) | fmt;
		lsdc_wreg32(ldev, LSDC_CRTC0_CFG_REG, val);
	} else if (index == 1) {
		val = lsdc_rreg32(ldev, LSDC_CRTC1_CFG_REG);
		val = (val & ~CFG_PIX_FMT_MASK) | fmt;
		lsdc_wreg32(ldev, LSDC_CRTC1_CFG_REG, val);
	}
}

static void lsdc_update_fb_start_addr(struct lsdc_device *ldev,
				      struct drm_crtc *crtc,
				      u64 paddr)
{
	struct drm_device *ddev = &ldev->base;
	unsigned int index = drm_crtc_index(crtc);
	u32 lo_addr_reg;
	u32 hi_addr_reg;
	u32 val;

	/*
	 * Find which framebuffer address register should update.
	 * if FB_ADDR0_REG is in using, we write the address to FB_ADDR0_REG,
	 * if FB_ADDR1_REG is in using, we write the address to FB_ADDR1_REG
	 * for each CRTC, the switch using one fb register to another is
	 * trigger by triggered by set CFG_PAGE_FLIP bit of LSDC_CRTCx_CFG_REG
	 */
	if (index == 0) {
		val = lsdc_rreg32(ldev, LSDC_CRTC0_CFG_REG);
		if (val & CFG_FB_IN_USING) {
			lo_addr_reg = LSDC_CRTC0_FB1_LO_ADDR_REG;
			hi_addr_reg = LSDC_CRTC0_FB1_HI_ADDR_REG;
			drm_dbg(ddev, "Currently, FB1 is in using by CRTC-0\n");
		} else {
			lo_addr_reg = LSDC_CRTC0_FB0_LO_ADDR_REG;
			hi_addr_reg = LSDC_CRTC0_FB0_HI_ADDR_REG;
			drm_dbg(ddev, "Currently, FB0 is in using by CRTC-0\n");
		}
	} else if (index == 1) {
		val = lsdc_rreg32(ldev, LSDC_CRTC1_CFG_REG);
		if (val & CFG_FB_IN_USING) {
			lo_addr_reg = LSDC_CRTC1_FB1_LO_ADDR_REG;
			hi_addr_reg = LSDC_CRTC1_FB1_HI_ADDR_REG;
			drm_dbg(ddev, "Currently, FB1 is in using by CRTC-1\n");
		} else {
			lo_addr_reg = LSDC_CRTC1_FB0_LO_ADDR_REG;
			hi_addr_reg = LSDC_CRTC1_FB0_HI_ADDR_REG;
			drm_dbg(ddev, "Currently, FB0 is in using by CRTC-1\n");
		}
	}

	/* 40-bit width physical address bus */
	lsdc_wreg32(ldev, lo_addr_reg, paddr);
	lsdc_wreg32(ldev, hi_addr_reg, (paddr >> 32) & 0xFF);

	drm_dbg(ddev, "CRTC-%u scantout from 0x%llx\n", index, paddr);
}

static unsigned int lsdc_get_fb_offset(struct drm_framebuffer *fb,
				       struct drm_plane_state *state,
				       unsigned int plane)
{
	unsigned int offset = fb->offsets[plane];

	offset += fb->format->cpp[plane] * (state->src_x >> 16);
	offset += fb->pitches[plane] * (state->src_y >> 16);

	return offset;
}

static s64 lsdc_get_vram_bo_offset(struct drm_framebuffer *fb)
{
	struct drm_gem_vram_object *gbo;
	s64 gpu_addr;

	gbo = drm_gem_vram_of_gem(fb->obj[0]);
	gpu_addr = drm_gem_vram_offset(gbo);

	return gpu_addr;
}

static int lsdc_primary_plane_atomic_check(struct drm_plane *plane,
					   struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc *crtc = plane_state->crtc;
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	if (!crtc)
		return 0;

	return drm_atomic_helper_check_plane_state(plane_state,
						   crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false,
						   true);
}

static void lsdc_update_fb_stride(struct lsdc_device *ldev,
				  struct drm_crtc *crtc,
				  unsigned int stride)
{
	unsigned int index = drm_crtc_index(crtc);

	if (index == 0)
		lsdc_wreg32(ldev, LSDC_CRTC0_STRIDE_REG, stride);
	else if (index == 1)
		lsdc_wreg32(ldev, LSDC_CRTC1_STRIDE_REG, stride);

	drm_dbg(crtc->dev, "update stride to %u\n", stride);
}

static void lsdc_primary_plane_atomic_update(struct drm_plane *plane,
					     struct drm_atomic_state *state)
{
	struct lsdc_device *ldev = to_lsdc(plane->dev);
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc *crtc = new_plane_state->crtc;
	struct drm_framebuffer *fb = new_plane_state->fb;
	u32 fb_offset = lsdc_get_fb_offset(fb, new_plane_state, 0);
	dma_addr_t fb_addr;
	s64 gpu_addr;

	gpu_addr = lsdc_get_vram_bo_offset(fb);
	if (gpu_addr < 0)
		return;

	fb_addr = ldev->vram_base + gpu_addr + fb_offset;

	lsdc_update_fb_start_addr(ldev, crtc, fb_addr);

	lsdc_update_fb_stride(ldev, crtc, fb->pitches[0]);

	lsdc_update_fb_format(ldev, crtc, fb->format);
}

static void lsdc_primary_plane_atomic_disable(struct drm_plane *plane,
					      struct drm_atomic_state *state)
{
	drm_dbg(plane->dev, "%s disabled\n", plane->name);
}

static const struct drm_plane_helper_funcs lsdc_primary_plane_helpers = {
	.prepare_fb = drm_gem_vram_plane_helper_prepare_fb,
	.cleanup_fb = drm_gem_vram_plane_helper_cleanup_fb,
	.atomic_check = lsdc_primary_plane_atomic_check,
	.atomic_update = lsdc_primary_plane_atomic_update,
	.atomic_disable = lsdc_primary_plane_atomic_disable,
};

static int lsdc_cursor_atomic_check(struct drm_plane *plane,
				    struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = new_plane_state->fb;
	struct drm_crtc *crtc = new_plane_state->crtc;
	struct drm_crtc_state *crtc_state;
	int ret;

	/* no need for further checks if the plane is being disabled */
	if (!crtc || !fb)
		return 0;

	if (!new_plane_state->visible)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state,
						   new_plane_state->crtc);

	ret = drm_atomic_helper_check_plane_state(new_plane_state,
						  crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  true,
						  true);

	return ret;
}

/*
 * There is only one hardware cursor in ls7a1000, ls2k1000 and ls2k0500.
 * we made it shared by the two CRTC, which can satisfy peoples who use
 * double screen extend mode only. On clone screen usage case, the cursor
 * on display pipe 1 will not be able to display.
 *
 * Update location of the cursor, attach it to CRTC0 or CRTC1 on the runtime.
 */
static void lsdc_cursor_update_location_quirks(struct lsdc_device *ldev,
					       struct drm_crtc *crtc)
{
	u32 val = CURSOR_FORMAT_ARGB8888;

	/*
	 * If bit 4 of LSDC_CURSOR0_CFG_REG is 1, then the cursor will be
	 * locate at CRTC-1, if bit 4 of LSDC_CURSOR0_CFG_REG is 0, then
	 * the cursor will be locate at CRTC-0. The cursor is alway on the
	 * top of the primary. Compositing the primary plane and cursor
	 * plane is automatically done by hardware.
	 */
	if (drm_crtc_index(crtc))
		val |= CURSOR_LOCATION;

	lsdc_wreg32(ldev, LSDC_CURSOR0_CFG_REG, val);
}

/* update the position of the cursor */
static void lsdc_cursor_update_position_quirks(struct lsdc_device *ldev,
					       int x,
					       int y)
{
	if (x < 0)
		x = 0;

	if (y < 0)
		y = 0;

	lsdc_wreg32(ldev, LSDC_CURSOR0_POSITION_REG, (y << 16) | x);
}

static void lsdc_cursor_atomic_update_quirks(struct drm_plane *plane,
					     struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_framebuffer *new_fb = new_plane_state->fb;
	struct drm_framebuffer *old_fb = old_plane_state->fb;

	if (new_fb != old_fb) {
		s64 offset = lsdc_get_vram_bo_offset(new_fb);
		u64 cursor_addr = ldev->vram_base + offset;

		drm_dbg_kms(ddev, "%s offset: %llx\n", plane->name, offset);

		lsdc_wreg32(ldev, LSDC_CURSOR0_ADDR_LO_REG, cursor_addr);
		lsdc_wreg32(ldev, LSDC_CURSOR0_ADDR_HI_REG, (cursor_addr >> 32) & 0xFF);
	}

	lsdc_cursor_update_position_quirks(ldev, new_plane_state->crtc_x, new_plane_state->crtc_y);

	lsdc_cursor_update_location_quirks(ldev, new_plane_state->crtc);
}

/* update the format, size and location of the cursor */
static void lsdc_cursor_atomic_update(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct lsdc_display_pipe *dispipe = cursor_to_display_pipe(plane);
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *new_fb = new_plane_state->fb;
	int x = new_plane_state->crtc_x;
	int y = new_plane_state->crtc_y;
	u32 conf = CURSOR_FORMAT_ARGB8888 | CURSOR_SIZE_64X64;
	u64 cursor_addr = ldev->vram_base + lsdc_get_vram_bo_offset(new_fb);

	if (x < 0)
		x = 0;

	if (y < 0)
		y = 0;

	if (dispipe->index == 0) {
		lsdc_wreg32(ldev, LSDC_CURSOR0_ADDR_HI_REG, (cursor_addr >> 32) & 0xFF);
		lsdc_wreg32(ldev, LSDC_CURSOR0_ADDR_LO_REG, cursor_addr);
		/* Attach Cursor-0 to CRTC-0 */
		lsdc_wreg32(ldev, LSDC_CURSOR0_CFG_REG, conf & ~CURSOR_LOCATION);
		lsdc_wreg32(ldev, LSDC_CURSOR0_POSITION_REG, (y << 16) | x);
		return;
	}

	if (dispipe->index == 1) {
		lsdc_wreg32(ldev, LSDC_CURSOR1_ADDR_HI_REG, (cursor_addr >> 32) & 0xFF);
		lsdc_wreg32(ldev, LSDC_CURSOR1_ADDR_LO_REG, cursor_addr);
		/* Attach Cursor-1 to CRTC-1 */
		lsdc_wreg32(ldev, LSDC_CURSOR1_CFG_REG, conf | CURSOR_LOCATION);
		lsdc_wreg32(ldev, LSDC_CURSOR1_POSITION_REG, (y << 16) | x);
		return;
	}
}

static void lsdc_cursor_atomic_disable_quirks(struct drm_plane *plane,
					      struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);

	/* Set the format to 0 actually display the cursor */
	lsdc_wreg32(ldev, LSDC_CURSOR0_CFG_REG, 0);

	drm_dbg(ddev, "%s disabled\n", plane->name);
}

static void lsdc_cursor_atomic_disable(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct lsdc_display_pipe *dispipe = cursor_to_display_pipe(plane);

	if (dispipe->index == 0)
		lsdc_wreg32(ldev, LSDC_CURSOR0_CFG_REG, 0);
	else if (dispipe->index == 1)
		lsdc_wreg32(ldev, LSDC_CURSOR1_CFG_REG, 0);

	drm_dbg(ddev, "%s disabled\n", plane->name);
}

static const struct drm_plane_helper_funcs lsdc_cursor_helpers_quirk = {
	.prepare_fb = drm_gem_vram_plane_helper_prepare_fb,
	.cleanup_fb = drm_gem_vram_plane_helper_cleanup_fb,
	.atomic_check = lsdc_cursor_atomic_check,
	.atomic_update = lsdc_cursor_atomic_update_quirks,
	.atomic_disable = lsdc_cursor_atomic_disable_quirks,
};

static const struct drm_plane_helper_funcs lsdc_cursor_plane_helpers = {
	.prepare_fb = drm_gem_vram_plane_helper_prepare_fb,
	.cleanup_fb = drm_gem_vram_plane_helper_cleanup_fb,
	.atomic_check = lsdc_cursor_atomic_check,
	.atomic_update = lsdc_cursor_atomic_update,
	.atomic_disable = lsdc_cursor_atomic_disable,
};

static const struct drm_plane_funcs lsdc_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static const struct drm_plane_helper_funcs *
lsdc_get_cursor_helper_funcs(const struct lsdc_desc *descp)
{
	if (descp->chip == CHIP_LS7A2000)
		return &lsdc_cursor_plane_helpers;

	return &lsdc_cursor_helpers_quirk;
}

int lsdc_plane_init(struct lsdc_device *ldev,
		    struct drm_plane *plane,
		    enum drm_plane_type type,
		    unsigned int index)
{
	const struct lsdc_desc *descp = ldev->descp;
	struct drm_device *ddev = &ldev->base;
	unsigned int format_count;
	const u32 *formats;
	const char *name;
	int ret;

	switch (type) {
	case DRM_PLANE_TYPE_PRIMARY:
		formats = lsdc_primary_formats;
		format_count = ARRAY_SIZE(lsdc_primary_formats);
		name = "primary-%u";
		break;
	case DRM_PLANE_TYPE_CURSOR:
		formats = lsdc_cursor_formats;
		format_count = ARRAY_SIZE(lsdc_cursor_formats);
		name = "cursor-%u";
		break;
	case DRM_PLANE_TYPE_OVERLAY:
		drm_err(ddev, "overlay plane is not supported\n");
		break;
	}

	ret = drm_universal_plane_init(ddev, plane, 1 << index,
				       &lsdc_plane_funcs,
				       formats, format_count,
				       lsdc_fb_format_modifiers,
				       type, name, index);
	if (ret) {
		drm_err(ddev, "%s failed: %d\n", __func__, ret);
		return ret;
	}

	switch (type) {
	case DRM_PLANE_TYPE_PRIMARY:
		drm_plane_helper_add(plane, &lsdc_primary_plane_helpers);
		break;
	case DRM_PLANE_TYPE_CURSOR:
		drm_plane_helper_add(plane, lsdc_get_cursor_helper_funcs(descp));
		break;
	case DRM_PLANE_TYPE_OVERLAY:
		drm_err(ddev, "overlay plane is not supported\n");
		break;
	}

	return 0;
}

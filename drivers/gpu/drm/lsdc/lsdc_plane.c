// SPDX-License-Identifier: GPL-2.0

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_shmem_helper.h>
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


static unsigned int lsdc_get_fb_offset(struct drm_framebuffer *fb,
				       struct drm_plane_state *state,
				       unsigned int plane)
{
	unsigned int offset = fb->offsets[plane];

	offset += fb->format->cpp[plane] * (state->src_x >> 16);
	offset += fb->pitches[plane] * (state->src_y >> 16);

	return offset;
}

static void lsdc_update_base_addr_pipe_0(struct lsdc_primary * const this, u64 paddr)
{
	struct lsdc_device *ldev = this->ldev;
	struct drm_device *ddev = &ldev->base;
	struct drm_plane *plane = &this->base;
	struct drm_plane_state *plane_state = plane->state;
	struct drm_framebuffer *fb = plane_state->fb;
	u32 lo_addr_reg;
	u32 hi_addr_reg;
	u32 fb_offset;
	u32 val;

	/* TODO: testing all(clone, extend, resizing) */
	fb_offset = lsdc_get_fb_offset(fb, plane_state, 0);

	/*
	 * Find which framebuffer address register should update.
	 * if FB_ADDR0_REG is in using, we write the address to FB_ADDR0_REG,
	 * if FB_ADDR1_REG is in using, we write the address to FB_ADDR1_REG
	 * for each CRTC, the switch using one fb register to another is
	 * trigger by triggered by set CFG_PAGE_FLIP bit of LSDC_CRTCx_CFG_REG
	 */
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

	drm_dbg(plane->dev, "fb_offset: %u\n", fb_offset);

	paddr += fb_offset;

	/* 40-bit width physical address bus */
	lsdc_wreg32(ldev, lo_addr_reg, paddr);
	lsdc_wreg32(ldev, hi_addr_reg, (paddr >> 32) & 0xFF);

	drm_dbg(ddev, "CRTC-0 scantout from 0x%llx\n", paddr);
}

static void lsdc_update_base_addr_pipe_1(struct lsdc_primary * const this, u64 paddr)
{
	struct lsdc_device *ldev = this->ldev;
	struct drm_device *ddev = &ldev->base;
	struct drm_plane *plane = &this->base;
	struct drm_plane_state *plane_state = plane->state;
	struct drm_framebuffer *fb = plane_state->fb;
	u32 lo_addr_reg;
	u32 hi_addr_reg;
	u32 fb_offset;
	u32 val;

	/*
	 * Find which framebuffer address register should update.
	 * if FB0_ADDR_REG is in using, we write the address to FB0_ADDR_REG,
	 * if FB1_ADDR_REG is in using, we write the address to FB1_ADDR_REG
	 * for each CRTC, the switch using one fb register to another is
	 * trigger by triggered by set CFG_PAGE_FLIP bit of LSDC_CRTCx_CFG_REG
	 */
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

	/* TODO: testing all(clone, extend, resizing) */
	fb_offset = lsdc_get_fb_offset(fb, plane_state, 0);

	drm_dbg(plane->dev, "fb_offset: %u\n", fb_offset);

	paddr += fb_offset;

	/* 40-bit width physical address bus */
	lsdc_wreg32(ldev, lo_addr_reg, paddr);
	lsdc_wreg32(ldev, hi_addr_reg, (paddr >> 32) & 0xFF);

	drm_dbg(ddev, "CRTC-1 scantout from 0x%llx\n", paddr);
}

static void lsdc_update_stride_pipe_0(struct lsdc_primary * const this, u32 pitch)
{
	struct lsdc_device *ldev = this->ldev;

	lsdc_wreg32(ldev, LSDC_CRTC0_STRIDE_REG, pitch);
}

static void lsdc_update_stride_pipe_1(struct lsdc_primary * const this, u32 pitch)
{
	struct lsdc_device *ldev = this->ldev;

	lsdc_wreg32(ldev, LSDC_CRTC1_STRIDE_REG, pitch);
}


static void lsdc_update_format_pipe_0(struct lsdc_primary * const this, u32 fmt)
{
	struct lsdc_device *ldev = this->ldev;

	u32 val;
	u32 cfg;

	switch (fmt) {
	case DRM_FORMAT_RGB565:
		val = LSDC_PF_RGB565;
		break;
	case DRM_FORMAT_XRGB8888:
		val = LSDC_PF_XRGB8888;
		break;
	case DRM_FORMAT_ARGB8888:
		val = LSDC_PF_XRGB8888;
		break;
	default:
		val = LSDC_PF_XRGB8888;
		break;
	}

	cfg = lsdc_rreg32(ldev, LSDC_CRTC0_CFG_REG);
	cfg = (cfg & ~CFG_PIX_FMT_MASK) | val;
	lsdc_wreg32(ldev, LSDC_CRTC0_CFG_REG, cfg);
}

static void lsdc_update_format_pipe_1(struct lsdc_primary * const this, u32 fmt)
{
	struct lsdc_device *ldev = this->ldev;

	u32 val;
	u32 cfg;

	switch (fmt) {
	case DRM_FORMAT_RGB565:
		val = LSDC_PF_RGB565;
		break;
	case DRM_FORMAT_XRGB8888:
		val = LSDC_PF_XRGB8888;
		break;
	case DRM_FORMAT_ARGB8888:
		val = LSDC_PF_XRGB8888;
		break;
	default:
		val = LSDC_PF_XRGB8888;
		break;
	}

	cfg = lsdc_rreg32(ldev, LSDC_CRTC1_CFG_REG);
	cfg = (cfg & ~CFG_PIX_FMT_MASK) | val;
	lsdc_wreg32(ldev, LSDC_CRTC1_CFG_REG, cfg);
}

static void lsdc_handle_damage(struct lsdc_device *ldev,
			       const struct iosys_map *vmap,
			       struct drm_framebuffer *fb,
			       struct drm_rect *clip)
{
	struct iosys_map dst = IOSYS_MAP_INIT_VADDR_IOMEM(ldev->vram);

	iosys_map_incr(&dst, drm_fb_clip_offset(fb->pitches[0], fb->format, clip));
	drm_fb_memcpy(&dst, fb->pitches, vmap, fb, clip);
}

static const struct lsdc_primary_lowing_funcs lsdc_primary_funcs_pipe_0_ = {
	.update_stride = lsdc_update_stride_pipe_0,
	.update_format = lsdc_update_format_pipe_0,
	.update_base_addr = lsdc_update_base_addr_pipe_0,
};

static const struct lsdc_primary_lowing_funcs lsdc_primary_funcs_pipe_1_ = {
	.update_stride = lsdc_update_stride_pipe_1,
	.update_format = lsdc_update_format_pipe_1,
	.update_base_addr = lsdc_update_base_addr_pipe_1,
};


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


static void lsdc_primary_plane_atomic_update(struct drm_plane *plane,
					     struct drm_atomic_state *old_state)
{
	struct lsdc_device *ldev = to_lsdc(plane->dev);
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(old_state, plane);
	struct drm_plane_state *plane_state = plane->state;
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;
	const struct drm_format_info *fmt_info = fb->format;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect damage;
	struct lsdc_primary *primary;
	const struct lsdc_primary_lowing_funcs *funcs;

	u64 fb_addr = ldev->vram_base ;

	drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &damage) {
		lsdc_handle_damage(ldev, shadow_plane_state->data, fb, &damage);
	}

	primary = to_lsdc_primary(plane);
	funcs = primary->funcs;

	funcs->update_format(primary, fmt_info->format);
	funcs->update_stride(primary, fb->pitches[0]);
	funcs->update_base_addr(primary, fb_addr);

	drm_dbg(plane->dev, "fb_addr: 0x%llx\n", fb_addr);
}

static void lsdc_primary_plane_atomic_disable(struct drm_plane *plane,
					      struct drm_atomic_state *state)
{
	struct lsdc_primary *primary = to_lsdc_primary(plane);
	const struct lsdc_primary_lowing_funcs *funcs= primary->funcs;

	funcs->update_format(primary, LSDC_PF_NONE);

	drm_dbg(plane->dev, "%s disabled\n", plane->name);
}

static int lsdc_cursor_atomic_check(struct drm_plane *plane,
				    struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc *crtc = new_plane_state->crtc;
	struct drm_crtc_state *crtc_state;
	int ret;

	/* no need for further checks if the plane is being disabled */
	if (!crtc)
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

static void lsdc_cursor_update_image(struct lsdc_cursor * const this, const u8 *src, int width, int height)
{
	struct lsdc_device *ldev = this->ldev;
	u8 __iomem *dst = this->offset + ldev->vram;
	int pitch = width * 4;
	int y = 0;

	for (y = 0; y < height; y++) {
		memcpy_toio(dst, src, pitch);
		dst += pitch;
		src += pitch;
	}
}

static void lsdc_cursor0_update_cfg_quirk(struct lsdc_cursor *this, u32 cfg)
{
	struct lsdc_device *ldev = this->ldev;
	struct lsdc_display_pipe *disp = cursor_to_display_pipe(&this->base);

	if (this->cfg != cfg) {
		this->cfg = cfg;
		lsdc_wreg32(ldev, LSDC_CURSOR0_CFG_REG, disp->index ?
			cfg | CURSOR_LOCATION : cfg & ~CURSOR_LOCATION);
	}
}

static void lsdc_cursor0_update_start_addr(struct lsdc_cursor * const this, u64 addr)
{
	struct lsdc_device *ldev = this->ldev;

	if (this->dma_addr != addr) {
		this->dma_addr = addr;
		lsdc_wreg32(ldev, LSDC_CURSOR0_ADDR_HI_REG, (addr >> 32) & 0xFF);
		lsdc_wreg32(ldev, LSDC_CURSOR0_ADDR_LO_REG, addr);
	}
}

static void lsdc_cursor0_update_cfg(struct lsdc_cursor * const this, u32 cfg)
{
	struct lsdc_device *ldev = this->ldev;

	this->cfg = cfg;
	lsdc_wreg32(ldev, LSDC_CURSOR0_CFG_REG, cfg & ~CURSOR_LOCATION);
}

static void lsdc_cursor0_update_pos(struct lsdc_cursor * const this, int x, int y)
{
	struct lsdc_device *ldev = this->ldev;

	if (x < 0)
		x = 0;

	if (y < 0)
		y = 0;

	lsdc_wreg32(ldev, LSDC_CURSOR0_POSITION_REG, (y << 16) | x);
}

static void lsdc_cursor1_update_start_addr(struct lsdc_cursor * const this, u64 addr)
{
	struct lsdc_device *ldev = this->ldev;

	if (this->dma_addr != addr) {
		this->dma_addr = addr;
		lsdc_wreg32(ldev, LSDC_CURSOR1_ADDR_HI_REG, (addr >> 32) & 0xFF);
		lsdc_wreg32(ldev, LSDC_CURSOR1_ADDR_LO_REG, addr);
	}
}

static void lsdc_cursor1_update_cfg(struct lsdc_cursor * const this, u32 cfg)
{
	struct lsdc_device *ldev = this->ldev;

	this->cfg = cfg;
	lsdc_wreg32(ldev, LSDC_CURSOR1_CFG_REG, cfg | CURSOR_LOCATION);
}

static void lsdc_cursor1_update_pos(struct lsdc_cursor * const this, int x, int y)
{
	struct lsdc_device *ldev = this->ldev;

	if (x < 0)
		x = 0;

	if (y < 0)
		y = 0;

	lsdc_wreg32(ldev, LSDC_CURSOR1_POSITION_REG, (y << 16) | x);
}

/* update the format, size and location of the cursor */
static void lsdc_cursor_atomic_update(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct drm_device *ddev = plane->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct lsdc_cursor *cursor = to_lsdc_cursor(plane);
	const struct lsdc_cursor_lowing_funcs *cfuncs = cursor->funcs;
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_framebuffer *new_fb = plane_state->fb;
	struct drm_framebuffer *old_fb = old_plane_state->fb;
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(plane_state);
	struct iosys_map src_map = shadow_plane_state->data[0];
	const u8 *src = src_map.vaddr;

	if (new_fb != old_fb)
		cfuncs->update_image(cursor, src, new_fb->width, new_fb->height);

	cfuncs->update_base_addr(cursor, ldev->vram_base + cursor->offset);
	cfuncs->update_config(cursor, CURSOR_FORMAT_ARGB8888 | CURSOR_SIZE_64X64);
	cfuncs->update_position(cursor, plane_state->crtc_x, plane_state->crtc_y);
}

static void lsdc_cursor_atomic_disable(struct drm_plane *plane,
					struct drm_atomic_state *state)
{
	struct lsdc_cursor *cursor = to_lsdc_cursor(plane);
	const struct lsdc_cursor_lowing_funcs *cfuncs = cursor->funcs;

	cfuncs->update_config(cursor, 0);
}

static const struct drm_plane_helper_funcs lsdc_primary_plane_helpers = {
	.begin_fb_access = drm_gem_begin_shadow_fb_access,
	.end_fb_access = drm_gem_end_shadow_fb_access,
	.atomic_check = lsdc_primary_plane_atomic_check,
	.atomic_update = lsdc_primary_plane_atomic_update,
	.atomic_disable = lsdc_primary_plane_atomic_disable,
};

static const struct drm_plane_helper_funcs lsdc_cursor_plane_helpers = {
	.begin_fb_access = drm_gem_begin_shadow_fb_access,
	.end_fb_access = drm_gem_end_shadow_fb_access,
	.atomic_check = lsdc_cursor_atomic_check,
	.atomic_update = lsdc_cursor_atomic_update,
	.atomic_disable = lsdc_cursor_atomic_disable,
};

static const struct drm_plane_funcs lsdc_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_gem_reset_shadow_plane,
	.atomic_duplicate_state = drm_gem_duplicate_shadow_plane_state,
	.atomic_destroy_state = drm_gem_destroy_shadow_plane_state,
};

static const struct lsdc_cursor_lowing_funcs lcursor_funcs_0_ = {
	.update_position = lsdc_cursor0_update_pos,
	.update_config = lsdc_cursor0_update_cfg,
	.update_image = lsdc_cursor_update_image,
	.update_base_addr = lsdc_cursor0_update_start_addr,
};

static const struct lsdc_cursor_lowing_funcs lcursor_funcs_1_ = {
	.update_position = lsdc_cursor1_update_pos,
	.update_config = lsdc_cursor1_update_cfg,
	.update_image = lsdc_cursor_update_image,
	.update_base_addr = lsdc_cursor1_update_start_addr,
};

static const struct lsdc_cursor_lowing_funcs single_cursor_funcs_quirk = {
	.update_position = lsdc_cursor0_update_pos,
	.update_config = lsdc_cursor0_update_cfg_quirk,
	.update_image = lsdc_cursor_update_image,
	.update_base_addr = lsdc_cursor0_update_start_addr,
};

static void lsdc_cursor_plane_preinit(struct drm_plane *plane,
				      struct lsdc_device *ldev,
				      unsigned int index)
{
	struct lsdc_cursor *cursor = to_lsdc_cursor(plane);
	const struct lsdc_desc *descp = ldev->descp;

	drm_plane_helper_add(plane, &lsdc_cursor_plane_helpers);

	cursor->ldev = ldev;
	/* alloc offset manually, index start from 0, from top to down */
	cursor->offset = ldev->vram_size - 64*64*4 * (index + 1);
	cursor->vaddr = ldev->vram + cursor->offset;

	if (descp->chip == CHIP_LS7A2000 || descp->chip == CHIP_LS2K2000) {
		if (index == 0) {
			cursor->funcs = &lcursor_funcs_0_;
			return;
		}

		if (index == 1) {
			cursor->funcs = &lcursor_funcs_1_;
			return;
		}
	}

	/* ls2k1000/ls7a1000/ls2k0500 */
	cursor->funcs = &single_cursor_funcs_quirk;
}

static void lsdc_primary_plane_preinit(struct drm_plane *plane,
				      struct lsdc_device *ldev,
				      unsigned int index)
{
	struct lsdc_primary *primary = to_lsdc_primary(plane);

	drm_plane_helper_add(plane, &lsdc_primary_plane_helpers);

	primary->ldev = ldev;

	if (index == 0) {
		primary->funcs = &lsdc_primary_funcs_pipe_0_;
		return;
	}

	if (index == 1) {
		primary->funcs = &lsdc_primary_funcs_pipe_1_;
		return;
	}
}


int lsdc_plane_init(struct lsdc_device *ldev,
		    struct drm_plane *plane,
		    enum drm_plane_type type,
		    unsigned int index)
{
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
		lsdc_primary_plane_preinit(plane, ldev, index);
		drm_plane_enable_fb_damage_clips(plane);
		break;
	case DRM_PLANE_TYPE_CURSOR:
		lsdc_cursor_plane_preinit(plane, ldev, index);
		break;
	case DRM_PLANE_TYPE_OVERLAY:
		drm_err(ddev, "overlay plane is not supported\n");
		break;
	}

	return 0;
}

// SPDX-License-Identifier: GPL-2.0

#include <drm/drm_vblank.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include "lsdc_drv.h"
#include "lsdc_regs.h"
#include "lsdc_pll.h"

static int lsdc_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct lsdc_device *ldev = to_lsdc(crtc->dev);
	unsigned int index = drm_crtc_index(crtc);
	u32 val;

	val = lsdc_rreg32(ldev, LSDC_INT_REG);

	if (index == 0)
		val |= INT_CRTC0_VS_EN;
	else if (index == 1)
		val |= INT_CRTC1_VS_EN;

	lsdc_wreg32(ldev, LSDC_INT_REG, val);

	return 0;
}

static void lsdc_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct lsdc_device *ldev = to_lsdc(crtc->dev);
	unsigned int index = drm_crtc_index(crtc);
	u32 val;

	val = lsdc_rreg32(ldev, LSDC_INT_REG);

	if (index == 0)
		val &= ~INT_CRTC0_VS_EN;
	else if (index == 1)
		val &= ~INT_CRTC1_VS_EN;

	lsdc_wreg32(ldev, LSDC_INT_REG, val);
}

static void lsdc_crtc_reset(struct drm_crtc *crtc)
{
	struct drm_device *ddev = crtc->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	unsigned int index = drm_crtc_index(crtc);
	struct lsdc_crtc_state *priv_crtc_state;
	u32 val = CFG_RESET_N | LSDC_PF_XRGB8888;

	if (index == 0)
		lsdc_wreg32(ldev, LSDC_CRTC0_CFG_REG, val);
	else if (index == 1)
		lsdc_wreg32(ldev, LSDC_CRTC1_CFG_REG, val);

	if (crtc->state) {
		priv_crtc_state = to_lsdc_crtc_state(crtc->state);
		__drm_atomic_helper_crtc_destroy_state(&priv_crtc_state->base);
		kfree(priv_crtc_state);
	}

	priv_crtc_state = kzalloc(sizeof(*priv_crtc_state), GFP_KERNEL);
	if (!priv_crtc_state)
		return;

	__drm_atomic_helper_crtc_reset(crtc, &priv_crtc_state->base);

	drm_info(ddev, "CRTC-%u reset\n", index);
}

static void lsdc_crtc_atomic_destroy_state(struct drm_crtc *crtc,
					   struct drm_crtc_state *state)
{
	struct lsdc_crtc_state *priv_crtc_state = to_lsdc_crtc_state(state);

	__drm_atomic_helper_crtc_destroy_state(&priv_crtc_state->base);

	kfree(priv_crtc_state);
}

static struct drm_crtc_state *
lsdc_crtc_atomic_duplicate_state(struct drm_crtc *crtc)
{
	struct lsdc_crtc_state *new_priv_state;
	struct lsdc_crtc_state *old_priv_state;

	new_priv_state = kmalloc(sizeof(*new_priv_state), GFP_KERNEL);
	if (!new_priv_state)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &new_priv_state->base);

	old_priv_state = to_lsdc_crtc_state(crtc->state);

	memcpy(&new_priv_state->pparms, &old_priv_state->pparms,
	       sizeof(new_priv_state->pparms));

	return &new_priv_state->base;
}

static u32 lsdc_get_vblank_counter(struct drm_crtc *crtc)
{
	struct drm_device *ddev = crtc->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	const struct lsdc_desc *descp = ldev->descp;
	struct lsdc_display_pipe *dispipe = crtc_to_display_pipe(crtc);
	unsigned int index = dispipe->index;

	/* fallback to software emulated VBLANK counter */
	if (!descp->has_vblank_counter)
		goto fallback;

	if (index == 0)
		return lsdc_rreg32(ldev, LSDC_CRTC0_VSYNC_COUNTER_REG);

	if (index == 1)
		return lsdc_rreg32(ldev, LSDC_CRTC1_VSYNC_COUNTER_REG);

fallback:
	return (u32)drm_crtc_vblank_count(crtc);
}

static const struct drm_crtc_funcs lsdc_crtc_funcs = {
	.reset = lsdc_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = lsdc_crtc_atomic_duplicate_state,
	.atomic_destroy_state = lsdc_crtc_atomic_destroy_state,
	.get_vblank_counter = lsdc_get_vblank_counter,
	.enable_vblank = lsdc_crtc_enable_vblank,
	.disable_vblank = lsdc_crtc_disable_vblank,
	.get_vblank_timestamp = drm_crtc_vblank_helper_get_vblank_timestamp,
};

static enum drm_mode_status
lsdc_crtc_mode_valid(struct drm_crtc *crtc,
		     const struct drm_display_mode *mode)
{
	struct drm_device *ddev = crtc->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	const struct lsdc_desc *descp = ldev->descp;

	if (mode->hdisplay > descp->max_width)
		return MODE_BAD_HVALUE;
	if (mode->vdisplay > descp->max_height)
		return MODE_BAD_VVALUE;

	if (mode->clock > descp->max_pixel_clk) {
		drm_dbg(ddev, "mode %dx%d, pixel clock=%d is too high\n",
			mode->hdisplay, mode->vdisplay, mode->clock);
		return MODE_CLOCK_HIGH;
	}

	if ((mode->hdisplay * 4) % descp->pitch_align) {
		drm_dbg(ddev, "stride is require to not %u bytes aligned\n",
			descp->pitch_align);
		return MODE_BAD;
	}

	return MODE_OK;
}

static int lsdc_pixpll_atomic_check(struct drm_crtc *crtc,
				    struct drm_crtc_state *state)
{
	struct lsdc_display_pipe *dispipe = crtc_to_display_pipe(crtc);
	struct lsdc_pll *pixpll = &dispipe->pixpll;
	const struct lsdc_pixpll_funcs *pfuncs = pixpll->funcs;
	struct lsdc_crtc_state *priv_state = to_lsdc_crtc_state(state);
	bool ret;

	ret = pfuncs->compute(pixpll, state->mode.clock, &priv_state->pparms);
	if (ret)
		return 0;

	drm_warn(crtc->dev, "failed find PLL parameters for %u\n", state->mode.clock);

	return -EINVAL;
}

static int lsdc_crtc_helper_atomic_check(struct drm_crtc *crtc,
					 struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	if (!crtc_state->enable)
		return 0; /* no mode checks if CRTC is being disabled */

	return lsdc_pixpll_atomic_check(crtc, crtc_state);
}

static void lsdc_update_pixclk(struct lsdc_display_pipe *dispipe,
			       struct lsdc_crtc_state *priv_state)
{
	struct lsdc_pll *pixpll = &dispipe->pixpll;
	const struct lsdc_pixpll_funcs *clkfunc = pixpll->funcs;

	clkfunc->update(pixpll, &priv_state->pparms);
}

static void lsdc_crtc_helper_mode_set_nofb(struct drm_crtc *crtc)
{
	struct drm_device *ddev = crtc->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct drm_display_mode *mode = &crtc->state->mode;
	struct lsdc_display_pipe *dispipe = crtc_to_display_pipe(crtc);
	struct lsdc_crtc_state *priv_state = to_lsdc_crtc_state(crtc->state);
	unsigned int index = dispipe->index;
	u32 h_sync, v_sync, h_val, v_val;
	u32 val;

	/* 26:16 total pixels, 10:0 visiable pixels, in horizontal */
	h_val = (mode->crtc_htotal << 16) | mode->crtc_hdisplay;

	/* 26:16 total pixels, 10:0 visiable pixels, in vertical */
	v_val = (mode->crtc_vtotal << 16) | mode->crtc_vdisplay;
	/* 26:16 hsync end, 10:0 hsync start, bit 30 is hsync enable */
	h_sync = (mode->crtc_hsync_end << 16) | mode->crtc_hsync_start | CFG_HSYNC_EN;
	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		h_sync |= CFG_HSYNC_INV;

	/* 26:16 vsync end, 10:0 vsync start, bit 30 is vsync enable */
	v_sync = (mode->crtc_vsync_end << 16) | mode->crtc_vsync_start | CFG_VSYNC_EN;
	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		v_sync |= CFG_VSYNC_INV;

	if (index == 0) {
		lsdc_wreg32(ldev, LSDC_CRTC0_HDISPLAY_REG, h_val);
		lsdc_wreg32(ldev, LSDC_CRTC0_VDISPLAY_REG, v_val);
		lsdc_wreg32(ldev, LSDC_CRTC0_HSYNC_REG, h_sync);
		lsdc_wreg32(ldev, LSDC_CRTC0_VSYNC_REG, v_sync);

		val = lsdc_rreg32(ldev, LSDC_CRTC0_CFG_REG);
		val = (val & ~LSDC_DMA_STEP_MASK) | LSDC_DMA_STEP_256_BYTES;
		lsdc_wreg32(ldev, LSDC_CRTC0_CFG_REG, val);
	} else if (index == 1) {
		lsdc_wreg32(ldev, LSDC_CRTC1_HDISPLAY_REG, h_val);
		lsdc_wreg32(ldev, LSDC_CRTC1_VDISPLAY_REG, v_val);
		lsdc_wreg32(ldev, LSDC_CRTC1_HSYNC_REG, h_sync);
		lsdc_wreg32(ldev, LSDC_CRTC1_VSYNC_REG, v_sync);

		val = lsdc_rreg32(ldev, LSDC_CRTC1_CFG_REG);
		val = (val & ~LSDC_DMA_STEP_MASK) | LSDC_DMA_STEP_256_BYTES;
		lsdc_wreg32(ldev, LSDC_CRTC1_CFG_REG, val);
	}

	drm_dbg(ddev, "%s modeset: %ux%u\n",
		crtc->name, mode->hdisplay, mode->vdisplay);

	lsdc_update_pixclk(dispipe, priv_state);
}

static void lsdc_enable_display(struct lsdc_device *ldev, unsigned int index)
{
	u32 val;

	if (index == 0) {
		val = lsdc_rreg32(ldev, LSDC_CRTC0_CFG_REG);
		val |= CFG_OUTPUT_EN;
		lsdc_wreg32(ldev, LSDC_CRTC0_CFG_REG, val);
		return;
	}

	if (index == 1) {
		val = lsdc_rreg32(ldev, LSDC_CRTC1_CFG_REG);
		val |= CFG_OUTPUT_EN;
		lsdc_wreg32(ldev, LSDC_CRTC1_CFG_REG, val);
		return;
	}
}

static void lsdc_disable_display(struct lsdc_device *ldev, unsigned int index)
{
	u32 val;

	if (index == 0) {
		val = lsdc_rreg32(ldev, LSDC_CRTC0_CFG_REG);
		val &= ~CFG_OUTPUT_EN;
		lsdc_wreg32(ldev, LSDC_CRTC0_CFG_REG, val);
		return;
	}

	if (index == 1) {
		val = lsdc_rreg32(ldev, LSDC_CRTC1_CFG_REG);
		val &= ~CFG_OUTPUT_EN;
		lsdc_wreg32(ldev, LSDC_CRTC1_CFG_REG, val);
		return;
	}
}

static void lsdc_crtc_helper_atomic_enable(struct drm_crtc *crtc,
					   struct drm_atomic_state *state)
{
	struct drm_device *ddev = crtc->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);

	drm_crtc_vblank_on(crtc);

	lsdc_enable_display(ldev, drm_crtc_index(crtc));

	drm_dbg(ddev, "%s: enabled\n", crtc->name);
}

static void lsdc_crtc_helper_atomic_disable(struct drm_crtc *crtc,
					    struct drm_atomic_state *state)
{
	struct drm_device *ddev = crtc->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);

	lsdc_disable_display(ldev, drm_crtc_index(crtc));

	drm_crtc_wait_one_vblank(crtc);

	drm_crtc_vblank_off(crtc);

	drm_dbg(ddev, "%s: disabled\n", crtc->name);
}

static void lsdc_crtc_atomic_flush(struct drm_crtc *crtc,
				   struct drm_atomic_state *state)
{
	spin_lock_irq(&crtc->dev->event_lock);
	if (crtc->state->event) {
		if (drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, crtc->state->event);
		else
			drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}
	spin_unlock_irq(&crtc->dev->event_lock);
}

static bool lsdc_crtc_get_scanout_position(struct drm_crtc *crtc,
					   bool in_vblank_irq,
					   int *vpos,
					   int *hpos,
					   ktime_t *stime,
					   ktime_t *etime,
					   const struct drm_display_mode *mode)
{
	struct drm_device *ddev = crtc->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	unsigned int index = drm_crtc_index(crtc);
	int line, vsw, vbp, vactive_start, vactive_end, vfp_end;
	u32 val = 0;

	vsw = mode->crtc_vsync_end - mode->crtc_vsync_start;
	vbp = mode->crtc_vtotal - mode->crtc_vsync_end;

	vactive_start = vsw + vbp + 1;

	vactive_end = vactive_start + mode->crtc_vdisplay;

	/* last scan line before VSYNC */
	vfp_end = mode->crtc_vtotal;

	if (stime)
		*stime = ktime_get();

	if (index == 0)
		val = lsdc_rreg32(ldev, LSDC_CRTC0_SCAN_POS_REG);
	else if (index == 1)
		val = lsdc_rreg32(ldev, LSDC_CRTC1_SCAN_POS_REG);

	line = (val & 0xffff);

	if (line < vactive_start)
		line -= vactive_start;
	else if (line > vactive_end)
		line = line - vfp_end - vactive_start;
	else
		line -= vactive_start;

	*vpos = line;
	*hpos = val >> 16;

	if (etime)
		*etime = ktime_get();

	return true;
}

static const struct drm_crtc_helper_funcs lsdc_crtc_helper_funcs = {
	.mode_valid = lsdc_crtc_mode_valid,
	.mode_set_nofb = lsdc_crtc_helper_mode_set_nofb,
	.atomic_enable = lsdc_crtc_helper_atomic_enable,
	.atomic_disable = lsdc_crtc_helper_atomic_disable,
	.atomic_check = lsdc_crtc_helper_atomic_check,
	.atomic_flush = lsdc_crtc_atomic_flush,
	.get_scanout_position = lsdc_crtc_get_scanout_position,
};

int lsdc_crtc_init(struct drm_device *ddev,
		   struct drm_crtc *crtc,
		   unsigned int index,
		   struct drm_plane *primary,
		   struct drm_plane *cursor)
{
	int ret;

	ret = drm_crtc_init_with_planes(ddev, crtc, primary, cursor,
					&lsdc_crtc_funcs,
					"CRTC-%d", index);

	if (ret) {
		drm_err(ddev, "crtc init with planes failed: %d\n", ret);
		return ret;
	}

	drm_crtc_helper_add(crtc, &lsdc_crtc_helper_funcs);

	ret = drm_mode_crtc_set_gamma_size(crtc, 256);
	if (ret)
		drm_warn(ddev, "set the gamma table size failed\n");

	drm_crtc_enable_color_mgmt(crtc, 0, false, 256);

	return 0;
}

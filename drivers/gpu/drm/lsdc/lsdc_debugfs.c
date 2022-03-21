// SPDX-License-Identifier: GPL-2.0+

#include <drm/drm_debugfs.h>
#include <drm/drm_managed.h>
#include <drm/drm_gem_vram_helper.h>
#include "lsdc_drv.h"
#include "lsdc_regs.h"
#include "lsdc_pll.h"

#ifdef CONFIG_DEBUG_FS
static int lsdc_show_clock(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *ddev = node->minor->dev;
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, ddev) {
		struct lsdc_display_pipe *pipe = crtc_to_display_pipe(crtc);
		struct lsdc_pll *pixpll = &pipe->pixpll;
		const struct lsdc_pixpll_funcs *funcs = pixpll->funcs;
		struct drm_display_mode *adj = &crtc->state->mode;
		struct lsdc_pll_parms parms;
		unsigned int out_khz;

		out_khz = funcs->get_clock_rate(pixpll, &parms);

		seq_printf(m, "Display pipe %u: %dx%d\n",
			   pipe->index, adj->hdisplay, adj->vdisplay);

		seq_printf(m, "Frequency actually output: %u kHz\n", out_khz);
		seq_printf(m, "Pixel clock required: %d kHz\n", adj->clock);
		seq_printf(m, "diff: %d kHz\n", adj->clock);

		seq_printf(m, "div_ref=%u, loopc=%u, div_out=%u\n",
			   parms.div_ref, parms.loopc, parms.div_out);

		seq_printf(m, "hsync_start=%d, hsync_end=%d, htotal=%d\n",
			   adj->hsync_start, adj->hsync_end, adj->htotal);
		seq_printf(m, "vsync_start=%d, vsync_end=%d, vtotal=%d\n\n",
			   adj->vsync_start, adj->vsync_end, adj->vtotal);
	}

	return 0;
}

static int lsdc_show_mm(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *ddev = node->minor->dev;
	struct drm_printer p = drm_seq_file_printer(m);

	drm_mm_print(&ddev->vma_offset_manager->vm_addr_space_mm, &p);

	return 0;
}

#define REGDEF(reg) { __stringify_1(LSDC_##reg##_REG), LSDC_##reg##_REG }
static const struct {
	const char *name;
	u32 reg_offset;
} lsdc_regs_array[] = {
	REGDEF(CURSOR0_CFG),
	REGDEF(CURSOR0_ADDR_LO),
	REGDEF(CURSOR0_ADDR_HI),
	REGDEF(CURSOR0_POSITION),
	REGDEF(CURSOR0_BG_COLOR),
	REGDEF(CURSOR0_FG_COLOR),
	REGDEF(INT),
	REGDEF(CRTC0_CFG),
	REGDEF(CRTC0_STRIDE),
	REGDEF(CRTC0_FB_ORIGIN),
	REGDEF(CRTC0_HDISPLAY),
	REGDEF(CRTC0_HSYNC),
	REGDEF(CRTC0_VDISPLAY),
	REGDEF(CRTC0_VSYNC),
	REGDEF(CRTC0_GAMMA_INDEX),
	REGDEF(CRTC0_GAMMA_DATA),
	REGDEF(CRTC1_CFG),
	REGDEF(CRTC1_STRIDE),
	REGDEF(CRTC1_FB_ORIGIN),
	REGDEF(CRTC1_HDISPLAY),
	REGDEF(CRTC1_HSYNC),
	REGDEF(CRTC1_VDISPLAY),
	REGDEF(CRTC1_VSYNC),
	REGDEF(CRTC1_GAMMA_INDEX),
	REGDEF(CRTC1_GAMMA_DATA),
};

static int lsdc_show_regs(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *ddev = node->minor->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	int i;

	for (i = 0; i < ARRAY_SIZE(lsdc_regs_array); i++) {
		u32 offset = lsdc_regs_array[i].reg_offset;
		const char *name = lsdc_regs_array[i].name;

		seq_printf(m, "%s (0x%04x): 0x%08x\n",
			   name, offset, lsdc_rreg32(ldev, offset));
	}

	return 0;
}

static int lsdc_show_vblank_counter(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *ddev = node->minor->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);

	seq_printf(m, "CRTC-0 vblank counter: %08u\n",
		   lsdc_rreg32(ldev, LSDC_CRTC0_VSYNC_COUNTER_REG));

	seq_printf(m, "CRTC-1 vblank counter: %08u\n",
		   lsdc_rreg32(ldev, LSDC_CRTC1_VSYNC_COUNTER_REG));

	return 0;
}

static int lsdc_show_scan_position(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *ddev = node->minor->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	u32 val0 = lsdc_rreg32(ldev, LSDC_CRTC0_SCAN_POS_REG);
	u32 val1 = lsdc_rreg32(ldev, LSDC_CRTC1_SCAN_POS_REG);

	seq_printf(m, "CRTC-0: x: %08u, y: %08u\n",
		   val0 >> 16, val0 & 0xFFFF);

	seq_printf(m, "CRTC-1: x: %08u, y: %08u\n",
		   val1 >> 16, val1 & 0xFFFF);

	return 0;
}

static int lsdc_show_fb_addr(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *ddev = node->minor->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	u32 lo, hi;
	u32 val;

	val = lsdc_rreg32(ldev, LSDC_CRTC0_CFG_REG);
	if (val & CFG_FB_IN_USING) {
		lo = lsdc_rreg32(ldev, LSDC_CRTC0_FB1_LO_ADDR_REG);
		hi = lsdc_rreg32(ldev, LSDC_CRTC0_FB1_HI_ADDR_REG);
		seq_printf(m, "CRTC-0 using fb1: 0x%x:%x\n", hi, lo);
	} else {
		lo = lsdc_rreg32(ldev, LSDC_CRTC0_FB0_LO_ADDR_REG);
		hi = lsdc_rreg32(ldev, LSDC_CRTC0_FB0_HI_ADDR_REG);
		seq_printf(m, "CRTC-0 using fb0: 0x%x:%x\n", hi, lo);
	}

	val = lsdc_rreg32(ldev, LSDC_CRTC1_CFG_REG);
	if (val & CFG_FB_IN_USING) {
		lo = lsdc_rreg32(ldev, LSDC_CRTC1_FB1_LO_ADDR_REG);
		hi = lsdc_rreg32(ldev, LSDC_CRTC1_FB1_HI_ADDR_REG);
		seq_printf(m, "CRTC-1 using fb1: 0x%x:%x\n", hi, lo);
	} else {
		lo = lsdc_rreg32(ldev, LSDC_CRTC1_FB0_LO_ADDR_REG);
		hi = lsdc_rreg32(ldev, LSDC_CRTC1_FB0_HI_ADDR_REG);
		seq_printf(m, "CRTC-1 using fb0: 0x%x:%x\n", hi, lo);
	}

	return 0;
}

static struct drm_info_list lsdc_debugfs_list[] = {
	{ "clocks",   lsdc_show_clock, 0 },
	{ "mm",       lsdc_show_mm, 0, NULL },
	{ "regs",     lsdc_show_regs, 0 },
	{ "vblanks",  lsdc_show_vblank_counter, 0, NULL },
	{ "scan_pos", lsdc_show_scan_position, 0, NULL },
	{ "fb_addr",  lsdc_show_fb_addr, 0, NULL },
};

#endif

void lsdc_debugfs_init(struct drm_minor *minor)
{
#ifdef CONFIG_DEBUG_FS
	drm_debugfs_create_files(lsdc_debugfs_list,
				 ARRAY_SIZE(lsdc_debugfs_list),
				 minor->debugfs_root,
				 minor);
#endif
}

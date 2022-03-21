// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_edid.h>
#include <drm/drm_file.h>
#include <drm/drm_probe_helper.h>

#include "lsdc_drv.h"
#include "lsdc_output.h"

static int lsdc_get_modes(struct drm_connector *connector)
{
	unsigned int num = 0;
	struct edid *edid;

	if (connector->ddc) {
		edid = drm_get_edid(connector, connector->ddc);
		if (edid) {
			drm_connector_update_edid_property(connector, edid);
			num = drm_add_edid_modes(connector, edid);
			kfree(edid);
		}

		return num;
	}

	num = drm_add_modes_noedid(connector, 1920, 1200);

	drm_set_preferred_mode(connector, 1024, 768);

	return num;
}

static enum drm_connector_status
lsdc_dpi_connector_detect(struct drm_connector *connector, bool force)
{
	struct i2c_adapter *ddc = connector->ddc;

	if (ddc) {
		if (drm_probe_ddc(ddc))
			return connector_status_connected;
	} else {
		if (connector->connector_type == DRM_MODE_CONNECTOR_DPI)
			return connector_status_connected;
	}

	return connector_status_unknown;
}

static enum drm_connector_status
ls7a2000_hdmi_vga_connector_detect_pipe0(struct drm_connector *connector, bool force)
{
	struct drm_device *ddev = connector->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	u32 val;

	val = lsdc_rreg32(ldev, LSDC_HDMI_HPD_STATUS_REG);

	if (val & HDMI0_HPD_FLAG)
		return connector_status_connected;

	if (connector->ddc) {
		if (drm_probe_ddc(connector->ddc))
			return connector_status_connected;

		return connector_status_disconnected;
	}

	return connector_status_unknown;
}

static enum drm_connector_status
ls7a2000_hdmi_connector_detect_pipe1(struct drm_connector *connector, bool force)
{
	struct lsdc_device *ldev = to_lsdc(connector->dev);
	u32 val;

	val = lsdc_rreg32(ldev, LSDC_HDMI_HPD_STATUS_REG);

	if (val & HDMI1_HPD_FLAG)
		return connector_status_connected;

	return connector_status_disconnected;
}

static struct drm_encoder *
lsdc_connector_get_best_encoder(struct drm_connector *connector,
				struct drm_atomic_state *state)
{
	struct lsdc_display_pipe *pipe = connector_to_display_pipe(connector);

	return &pipe->encoder;
}

static const struct drm_connector_helper_funcs lsdc_connector_helpers = {
	.atomic_best_encoder = lsdc_connector_get_best_encoder,
	.get_modes = lsdc_get_modes,
};

#define LSDC_CONNECTOR_FUNS_GEN(pfn_detect)                                    \
	.detect = pfn_detect,                                                  \
	.fill_modes = drm_helper_probe_single_connector_modes,                 \
	.destroy = drm_connector_cleanup,                                      \
	.reset = drm_atomic_helper_connector_reset,                            \
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state, \
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state

static const struct drm_connector_funcs lsdc_dpi_connector_funcs = {
	LSDC_CONNECTOR_FUNS_GEN(lsdc_dpi_connector_detect),
};

static const struct drm_connector_funcs
ls7a2000_hdmi_connector_funcs_array[LSDC_NUM_CRTC] = {
	{
		LSDC_CONNECTOR_FUNS_GEN(ls7a2000_hdmi_vga_connector_detect_pipe0),
	},
	{
		LSDC_CONNECTOR_FUNS_GEN(ls7a2000_hdmi_connector_detect_pipe1),
	}
};

/* Even though some board has only one hdmi on display pipe 1,
 * We still need hook lsdc_encoder_funcs up on display pipe 0,
 * This is because we need its reset() callback get called, to
 * set the LSDC_HDMIx_CTRL_REG using software gpio emulated i2c.
 * Otherwise, the firmware may set LSDC_HDMIx_CTRL_REG blindly.
 */
static void ls7a2000_hdmi_encoder_reset(struct drm_encoder *encoder)
{
	struct drm_device *ddev = encoder->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct lsdc_display_pipe *dispipe = encoder_to_display_pipe(encoder);
	unsigned int index = dispipe->index;
	u32 val;

	val = lsdc_hdmi_rreg32(ldev, LSDC_HDMI0_PHY_CTRL_REG, index);
	val &= ~HDMI_PHY_RESET_N;
	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_PHY_CTRL_REG, index, val);
	udelay(9);
	val |= HDMI_PHY_RESET_N;
	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_PHY_CTRL_REG, index, val);
	udelay(9);

	val = lsdc_hdmi_rreg32(ldev, LSDC_HDMI0_INTF_CTRL_REG, index);
	val &= ~HW_I2C_EN;
	val |= HDMI_INTERFACE_EN | HDMI_PACKET_EN;
	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_INTF_CTRL_REG, index, val);

	drm_dbg(ddev, "HDMI-%u Reset\n", index);
}

#ifdef CONFIG_DEBUG_FS

#define LSDC_HDMI_REG(i, reg) {                               \
	.name = __stringify_1(LSDC_HDMI##i##_##reg##_REG),    \
	.offset = LSDC_HDMI##i##_##reg##_REG,                 \
}

static const struct debugfs_reg32 ls7a2000_hdmi_encoder_regs[][9] = {
	{
		LSDC_HDMI_REG(0, ZONE),
		LSDC_HDMI_REG(0, INTF_CTRL),
		LSDC_HDMI_REG(0, PHY_CTRL),
		LSDC_HDMI_REG(0, PHY_PLL),
		LSDC_HDMI_REG(0, AVI_INFO_CRTL),
		LSDC_HDMI_REG(0, PHY_CAL),
		LSDC_HDMI_REG(0, AUDIO_PLL_LO),
		LSDC_HDMI_REG(0, AUDIO_PLL_HI),
		{NULL, 0},
	},
	{
		LSDC_HDMI_REG(1, ZONE),
		LSDC_HDMI_REG(1, INTF_CTRL),
		LSDC_HDMI_REG(1, PHY_CTRL),
		LSDC_HDMI_REG(1, PHY_PLL),
		LSDC_HDMI_REG(1, AVI_INFO_CRTL),
		LSDC_HDMI_REG(1, PHY_CAL),
		LSDC_HDMI_REG(1, AUDIO_PLL_LO),
		LSDC_HDMI_REG(1, AUDIO_PLL_HI),
		{NULL, 0},  /* MUST be {NULL, 0} terminated */
	},
};

static int ls7a2000_hdmi_encoder_regs_show(struct seq_file *m, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *ddev = node->minor->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	const struct debugfs_reg32 *preg;

	preg = (const struct debugfs_reg32 *)node->info_ent->data;

	while (preg->name) {
		u32 offset = preg->offset;

		seq_printf(m, "%s (0x%04x): 0x%08x\n", preg->name, offset,
			   lsdc_rreg32(ldev, offset));
		++preg;
	}

	return 0;
}

static const struct drm_info_list ls7a2000_hdmi_debugfs_files[] = {
	{ "hdmi0_regs", ls7a2000_hdmi_encoder_regs_show, 0, (void *)ls7a2000_hdmi_encoder_regs[0] },
	{ "hdmi1_regs", ls7a2000_hdmi_encoder_regs_show, 0, (void *)ls7a2000_hdmi_encoder_regs[1] },
};

static int ls7a2000_hdmi_encoder_late_register(struct drm_encoder *encoder)
{
	struct lsdc_display_pipe *dispipe = encoder_to_display_pipe(encoder);
	struct drm_device *ddev = encoder->dev;
	struct drm_minor *minor = ddev->primary;

	drm_debugfs_create_files(&ls7a2000_hdmi_debugfs_files[dispipe->index],
				 1, minor->debugfs_root, minor);

	return 0;
}

#endif

static const struct drm_encoder_funcs ls7a1000_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_encoder_funcs ls7a2000_encoder_funcs = {
	.reset = ls7a2000_hdmi_encoder_reset,
	.destroy = drm_encoder_cleanup,
#ifdef CONFIG_DEBUG_FS
	.late_register = ls7a2000_hdmi_encoder_late_register,
#endif
};

static int ls7a2000_hdmi_set_avi_infoframe(struct drm_encoder *encoder,
					   struct drm_display_mode *mode)
{
	struct lsdc_display_pipe *dispipe = encoder_to_display_pipe(encoder);
	struct drm_device *ddev = encoder->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	unsigned int index = dispipe->index;
	struct hdmi_avi_infoframe infoframe;
	u8 buffer[HDMI_INFOFRAME_SIZE(AVI)];
	unsigned char *ptr = &buffer[HDMI_INFOFRAME_HEADER_SIZE];
	unsigned int content0, content1, content2, content3;
	int err;

	err = drm_hdmi_avi_infoframe_from_display_mode(&infoframe, &dispipe->connector, mode);
	if (err < 0) {
		drm_err(ddev, "failed to setup AVI infoframe: %d\n", err);
		return err;
	}

	/* Fixed infoframe configuration not linked to the mode */
	infoframe.colorspace = HDMI_COLORSPACE_RGB;
	infoframe.quantization_range = HDMI_QUANTIZATION_RANGE_DEFAULT;
	infoframe.colorimetry = HDMI_COLORIMETRY_NONE;

	err = hdmi_avi_infoframe_pack(&infoframe, buffer, sizeof(buffer));
	if (err < 0) {
		drm_err(ddev, "failed to pack AVI infoframe: %d\n", err);
			return err;
	}

	content0 = *(unsigned int *)ptr;
	content1 = *(ptr + 4);
	content2 = *(unsigned int *)(ptr + 5);
	content3 = *(unsigned int *)(ptr + 9);

	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_AVI_CONTENT0, index, content0);
	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_AVI_CONTENT1, index, content1);
	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_AVI_CONTENT2, index, content2);
	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_AVI_CONTENT3, index, content3);

	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_AVI_INFO_CRTL_REG, index,
			 AVI_PKT_ENABLE | AVI_PKT_UPDATE);

	drm_dbg(ddev, "Update HDMI-%u avi infoframe\n", index);

	return 0;
}

static void ls7a2000_hdmi_atomic_disable(struct drm_encoder *encoder,
					 struct drm_atomic_state *state)
{
	struct lsdc_display_pipe *dispipe = encoder_to_display_pipe(encoder);
	struct drm_device *ddev = encoder->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	unsigned int index = dispipe->index;
	u32 val;

	val = lsdc_hdmi_rreg32(ldev, LSDC_HDMI0_PHY_CTRL_REG, index);
	val &= ~HDMI_PHY_EN;
	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_PHY_CTRL_REG, index, val);

	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_AVI_INFO_CRTL_REG, index, 0);

	drm_dbg(ddev, "HDMI-%u disabled\n", index);
}

static void ls7a2000_hdmi_atomic_enable(struct drm_encoder *encoder,
					struct drm_atomic_state *state)
{
	struct drm_device *ddev = encoder->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct lsdc_display_pipe *dispipe = encoder_to_display_pipe(encoder);
	unsigned int index = dispipe->index;
	u32 val;

	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_ZONE_REG, index, 0x00400040);

	val = lsdc_hdmi_rreg32(ldev, LSDC_HDMI0_PHY_CTRL_REG, index);

	val |= HDMI_PHY_TERM_STATUS |
	       HDMI_PHY_TERM_DET_EN |
	       HDMI_PHY_TERM_H_EN |
	       HDMI_PHY_TERM_L_EN |
	       HDMI_PHY_EN;

	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_PHY_CTRL_REG, index, val);

	val = HDMI_CTL_PERIOD_MODE |
	      HDMI_AUDIO_EN |
	      HDMI_PACKET_EN |
	      HDMI_INTERFACE_EN |
	      (8 << HDMI_VIDEO_PREAMBLE_SHIFT);

	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_INTF_CTRL_REG, index, val);

	drm_dbg(ddev, "HDMI-%u enabled\n", index);
}

/*
 *  Fout = M * Fin
 *
 *  M = (4 * LF) / (IDF * ODF)
 *
 *  IDF: Input Division Factor
 *  ODF: Output Division Factor
 *   LF: Loop Factor
 *    M: Required Mult
 *
 *  +--------------------------------------------------------+
 *  |     Fin (kHZ)     | M  | IDF | LF | ODF |   Fout(Mhz)  |
 *  |-------------------+----+-----+----+-----+--------------|
 *  |  170000 ~ 340000  | 10 | 16  | 40 |  1  | 1700 ~ 3400  |
 *  |   85000 ~ 170000  | 10 |  8  | 40 |  2  |  850 ~ 1700  |
 *  |   42500 ~  85000  | 10 |  4  | 40 |  4  |  425 ~ 850   |
 *  |   21250 ~  42500  | 10 |  2  | 40 |  8  | 212.5 ~ 425  |
 *  |   20000 ~  21250  | 10 |  1  | 40 | 16  |  200 ~ 212.5 |
 *  +--------------------------------------------------------+
 */
static void ls7a2000_hdmi_phy_pll_config(struct lsdc_device *ldev,
					 int fin,
					 unsigned int index)
{
	struct drm_device *ddev = &ldev->base;
	int count = 0;
	u32 val;

	/* Firstly, disable phy pll */
	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_PHY_PLL_REG, index, 0x0);

	/*
	 * Most of time, loongson HDMI require M = 10
	 * for example, 10 = (4 * 40) / (8 * 2)
	 * here, write "1" to the ODF will get "2"
	 */

	if (fin >= 170000)
		val = (16 << HDMI_PLL_IDF_SHIFT) |
		      (40 << HDMI_PLL_LF_SHIFT) |
		      (0 << HDMI_PLL_ODF_SHIFT);
	else if (fin >= 85000)
		val = (8 << HDMI_PLL_IDF_SHIFT) |
		      (40 << HDMI_PLL_LF_SHIFT) |
		      (1 << HDMI_PLL_ODF_SHIFT);
	else if (fin >= 42500)
		val = (4 << HDMI_PLL_IDF_SHIFT) |
		      (40 << HDMI_PLL_LF_SHIFT) |
		      (2 << HDMI_PLL_ODF_SHIFT);
	else if  (fin >= 21250)
		val = (2 << HDMI_PLL_IDF_SHIFT) |
		      (40 << HDMI_PLL_LF_SHIFT) |
		      (3 << HDMI_PLL_ODF_SHIFT);
	else
		val = (1 << HDMI_PLL_IDF_SHIFT) |
		      (40 << HDMI_PLL_LF_SHIFT) |
		      (4 << HDMI_PLL_ODF_SHIFT);

	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_PHY_PLL_REG, index, val | HDMI_PLL_ENABLE);

	udelay(1);

	drm_dbg(ddev, "Fin of HDMI-%u: %d kHz\n", index, fin);

	/* Wait hdmi phy pll lock */
	do {
		val = lsdc_hdmi_rreg32(ldev, LSDC_HDMI0_PHY_PLL_REG, index);

		if (val & HDMI_PLL_LOCKED) {
			drm_dbg(ddev, "Setting HDMI-%u PLL take %d cycles\n",
				index, count);
			break;
		}
		++count;
	} while (count < 1000);

	lsdc_hdmi_wreg32(ldev, LSDC_HDMI0_PHY_CAL_REG, index, 0xf000ff0);

	if (count >= 1000)
		drm_err(ddev, "Setting HDMI-%u PLL failed\n", index);
}

static void ls7a2000_hdmi_atomic_mode_set(struct drm_encoder *encoder,
					  struct drm_crtc_state *crtc_state,
					  struct drm_connector_state *conn_state)
{
	struct lsdc_display_pipe *dispipe = encoder_to_display_pipe(encoder);
	struct drm_device *ddev = encoder->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct drm_display_mode *mode = &crtc_state->mode;

	ls7a2000_hdmi_phy_pll_config(ldev, mode->clock, dispipe->index);

	ls7a2000_hdmi_set_avi_infoframe(encoder, mode);

	drm_dbg(ddev, "HDMI-%u modeset finished\n", dispipe->index);
}

const struct drm_encoder_helper_funcs ls7a2000_hdmi_encoder_helper_funcs = {
	.atomic_disable = ls7a2000_hdmi_atomic_disable,
	.atomic_enable = ls7a2000_hdmi_atomic_enable,
	.atomic_mode_set = ls7a2000_hdmi_atomic_mode_set,
};

/*
 * For LS7A2000:
 *
 * 1) Some board export double hdmi output interface
 * 2) Most of board export one vga + hdmi output interface
 * 3) Still have boards export three output(2 hdmi + 1 vga).
 *
 * So let's hook hdmi helper funcs to all display pipe, don't miss.
 * writing hdmi register do no harm, except wasting a few cpu's time
 * on the case which the motherboard don't export hdmi interface on
 * display pipe 0.
 */
static int ls7a2000_output_init(struct lsdc_device *ldev,
				struct lsdc_display_pipe *dispipe,
				struct i2c_adapter *ddc)
{
	struct drm_device *ddev = &ldev->base;
	struct drm_encoder *encoder = &dispipe->encoder;
	struct drm_connector *connector = &dispipe->connector;
	unsigned int pipe = dispipe->index;
	int ret;

	ret = drm_encoder_init(ddev,
			       encoder,
			       &ls7a2000_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS,
			       "encoder-%u",
			       pipe);
	if (ret)
		return ret;

	encoder->possible_crtcs = BIT(pipe);

	drm_encoder_helper_add(encoder, &ls7a2000_hdmi_encoder_helper_funcs);

	ret = drm_connector_init_with_ddc(ddev,
					  connector,
					  &ls7a2000_hdmi_connector_funcs_array[pipe],
					  DRM_MODE_CONNECTOR_HDMIA,
					  ddc);
	if (ret)
		return ret;

	drm_info(ddev, "display pipe-%u has HDMI%s\n", pipe, pipe ? "" : " and/or VGA");

	drm_connector_helper_add(connector, &lsdc_connector_helpers);

	drm_connector_attach_encoder(connector, encoder);

	connector->polled = DRM_CONNECTOR_POLL_CONNECT |
			    DRM_CONNECTOR_POLL_DISCONNECT;

	connector->interlace_allowed = 0;
	connector->doublescan_allowed = 0;

	return 0;
}

static int ls7a1000_output_init(struct lsdc_device *ldev,
				struct lsdc_display_pipe *dispipe,
				struct i2c_adapter *ddc)
{
	struct drm_device *ddev = &ldev->base;
	struct drm_encoder *encoder = &dispipe->encoder;
	struct drm_connector *connector = &dispipe->connector;
	int ret;

	ret = drm_encoder_init(ddev,
			       encoder,
			       &ls7a1000_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS,
			       "encoder-%u",
			       dispipe->index);
	if (ret)
		return ret;

	encoder->possible_crtcs = BIT(dispipe->index);

	ret = drm_connector_init_with_ddc(ddev,
					  connector,
					  &lsdc_dpi_connector_funcs,
					  DRM_MODE_CONNECTOR_DPI,
					  ddc);
	if (ret)
		return ret;

	drm_info(ddev, "display pipe-%u has DVO\n", dispipe->index);

	drm_connector_helper_add(connector, &lsdc_connector_helpers);

	drm_connector_attach_encoder(connector, encoder);

	connector->polled = DRM_CONNECTOR_POLL_CONNECT |
			    DRM_CONNECTOR_POLL_DISCONNECT;

	connector->interlace_allowed = 0;
	connector->doublescan_allowed = 0;

	return 0;
}

typedef int (*pfn_output_init_t)(struct lsdc_device *ldev,
				 struct lsdc_display_pipe *disp,
				 struct i2c_adapter *ddc);

/* NOTE: keep this as the order listed in loongson_chip_family enum */
static const pfn_output_init_t lsdc_output_init[CHIP_LAST] = {
	ls7a1000_output_init,
	ls7a2000_output_init,
};

int lsdc_create_output(struct lsdc_device *ldev,
		       struct lsdc_display_pipe *dispipe)
{
	const struct lsdc_desc *descp = ldev->descp;
	struct i2c_adapter *ddc = NULL;
	struct lsdc_i2c *li2c;

	if (descp->has_builtin_i2c) {
		li2c = lsdc_create_i2c_chan(&ldev->base, ldev->reg_base, dispipe->index);
		if (IS_ERR(li2c))
			return PTR_ERR(li2c);

		dispipe->li2c = li2c;
		ddc = &li2c->adapter;
	}

	/* Output interfaces suffer from changes */
	return lsdc_output_init[descp->chip](ldev, dispipe, ddc);
}

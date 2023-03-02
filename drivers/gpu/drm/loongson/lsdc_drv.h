/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KMS driver for Loongson display controller
 * Copyright (C) 2022 Loongson Corporation
 *
 * Authors:
 *      Li Yi <liyi@loongson.cn>
 *      Sui Jingfeng <suijingfeng@loongson.cn>
 */

#ifndef __LSDC_DRV_H__
#define __LSDC_DRV_H__

#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>

#include <drm/drm_atomic.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>
#include <drm/ttm/ttm_device.h>

#include "lsdc_pll.h"
#include "lsdc_regs.h"

/* Currently, all display controllers of loongson have two display pipes */
#define LSDC_NUM_CRTC           2

/*
 * LS7A1000 and LS7A2000 function as north bridge of the LS3A4000/LS3A5000,
 * They are equipped on-board Video RAM. While LS2K2000/LS2K1000 are SoC,
 * they don't have dediacated Video RAM.
 *
 * The display controller in LS7A2000 has two display pipe, yet it has three
 * integrated encoders, display pipe 0 is attached with a transparent VGA
 * encoder and a HDMI phy, they are parallel. Display pipe 1 has only one
 * HDMI phy connected.
 *       ______________________                          _____________
 *      |             +-----+  |                        |             |
 *      | CRTC0 -+--> | VGA |  ----> VGA Connector ---> | VGA Monitor |<---+
 *      |        |    +-----+  |                        |_____________|    |
 *      |        |             |                         ______________    |
 *      |        |    +------+ |                        |              |   |
 *      |        +--> | HDMI | ----> HDMI Connector --> | HDMI Monitor |<--+
 *      |             +------+ |                        |______________|   |
 *      |            +------+  |                                           |
 *      |            | i2c6 |  <-------------------------------------------+
 *      |            +------+  |
 *      |                      |
 *      |    DC in LS7A2000    |
 *      |                      |
 *      |            +------+  |
 *      |            | i2c7 |  <--------------------------------+
 *      |            +------+  |                                |
 *      |                      |                          ______|_______
 *      |            +------+  |                         |              |
 *      | CRTC1 ---> | HDMI |  ----> HDMI Connector ---> | HDMI Monitor |
 *      |            +------+  |                         |______________|
 *      |______________________|
 *
 *
 * The display controller in LS7A1000 has only two-way DVO interface exported,
 * thus, external encoder(TX chip) is required except connected with DPI panel
 * directly.
 *       ___________________                                     _________
 *      |            -------|                                   |         |
 *      |  CRTC0 --> | DVO0 ----> Encoder0 ---> Connector0 ---> | Display |
 *      |  _   _     -------|        ^             ^            |_________|
 *      | | | | |  +------+ |        |             |
 *      | |_| |_|  | i2c6 | <--------+-------------+
 *      |          +------+ |
 *      |  DC in LS7A1000   |
 *      |  _   _   +------+ |
 *      | | | | |  | i2c7 | <--------+-------------+
 *      | |_| |_|  +------+ |        |             |             _________
 *      |            -------|        |             |            |         |
 *      |  CRTC1 --> | DVO1 ----> Encoder1 ---> Connector1 ---> |  Panel  |
 *      |            -------|                                   |_________|
 *      |___________________|
 *
 * There is only a 1:1 mapping of crtcs, encoders and connectors for the DC,
 * display pipe 0 = crtc0 + dvo0 + encoder0 + connector0 + cursor0 + primary0
 * display pipe 1 = crtc1 + dvo1 + encoder1 + connectro1 + cursor1 + primary1
 * Each CRTC has two FB address registers.
 *
 * The DC in LS7A1000/LS2K1000 has the pci vendor/device ID: 0x0014:0x7a06,
 * The DC in LS7A2000/LS2K2000 has the pci vendor/device ID: 0x0014:0x7a36.
 *
 * LS7A1000 and LS7A2000 can only be used with LS3A3000, LS3A4000, LS3A5000
 * desktop class CPUs, thus CPU PRID can be used to differentiate those SoC
 * and the desktop level CPU on the runtime.
 */

enum loongson_chip_family {
	CHIP_LS7A1000 = 0,
	CHIP_LS7A2000 = 1,
	CHIP_LAST,
};

struct lsdc_desc {
	enum loongson_chip_family chip;
	u32 num_of_crtc;
	u32 max_pixel_clk;
	u32 max_width;
	u32 max_height;
	u32 num_of_hw_cursor;
	u32 hw_cursor_w;
	u32 hw_cursor_h;
	u32 pitch_align;         /* CRTC DMA alignment constraint */
	u64 mc_bits;             /* physical address bus bit width */
	bool has_vblank_counter; /* 32 bit hw vsync counter */
	bool has_scan_pos;       /* CRTC scan position recorder */
	bool has_builtin_i2c;
	bool has_vram;
	bool has_hpd_reg;
	bool is_soc;
};

struct lsdc_i2c {
	struct i2c_adapter adapter;
	struct i2c_algo_bit_data bit;
	struct drm_device *ddev;
	void __iomem *reg_base;
	void __iomem *dir_reg;
	void __iomem *dat_reg;
	/* pin bit mask */
	u8 sda;
	u8 scl;
};

struct lsdc_display_pipe {
	struct drm_crtc crtc;
	struct drm_plane primary;
	struct drm_plane cursor;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct lsdc_pll pixpll;
	struct lsdc_i2c *li2c;
	unsigned int index;
};

static inline struct lsdc_display_pipe *
crtc_to_display_pipe(struct drm_crtc *crtc)
{
	return container_of(crtc, struct lsdc_display_pipe, crtc);
}

static inline struct lsdc_display_pipe *
cursor_to_display_pipe(struct drm_plane *plane)
{
	return container_of(plane, struct lsdc_display_pipe, cursor);
}

static inline struct lsdc_display_pipe *
connector_to_display_pipe(struct drm_connector *connector)
{
	return container_of(connector, struct lsdc_display_pipe, connector);
}

static inline struct lsdc_display_pipe *
encoder_to_display_pipe(struct drm_encoder *encoder)
{
	return container_of(encoder, struct lsdc_display_pipe, encoder);
}

struct lsdc_crtc_state {
	struct drm_crtc_state base;
	struct lsdc_pll_parms pparms;
};

struct lsdc_gem {
	struct mutex mutex;
	struct list_head objects;
};

struct lsdc_device {
	struct drm_device base;
	struct ttm_device bdev;
	/* @descp: features description of the DC variant */
	const struct lsdc_desc *descp;

	/* @reglock: protects concurrent access */
	spinlock_t reglock;
	void __iomem *reg_base;
	resource_size_t vram_base;
	resource_size_t vram_size;

	struct lsdc_display_pipe dispipe[LSDC_NUM_CRTC];

	struct lsdc_gem gem;

	/* @num_output: count the number of active display pipe */
	unsigned int num_output;

	u32 irq_status;
};

static inline struct lsdc_device *tdev_to_ldev(struct ttm_device *bdev)
{
	return container_of(bdev, struct lsdc_device, bdev);
}

static inline struct lsdc_device *to_lsdc(struct drm_device *ddev)
{
	return container_of(ddev, struct lsdc_device, base);
}

static inline struct lsdc_crtc_state *to_lsdc_crtc_state(struct drm_crtc_state *base)
{
	return container_of(base, struct lsdc_crtc_state, base);
}

const char *chip_to_str(enum loongson_chip_family chip);

void lsdc_debugfs_init(struct drm_minor *minor);

int lsdc_crtc_init(struct drm_device *ddev,
		   struct drm_crtc *crtc,
		   struct drm_plane *primary,
		   struct drm_plane *cursor,
		   unsigned int index);

int lsdc_primary_plane_init(struct lsdc_device *ldev,
			    struct drm_plane *plane,
			    unsigned int index);

int lsdc_cursor_plane_init(struct lsdc_device *ldev,
			   struct drm_plane *plane,
			   unsigned int index);

irqreturn_t lsdc_irq_thread_handler(int irq, void *arg);
irq_handler_t lsdc_get_irq_handler(struct lsdc_device *ldev);

static inline u32 lsdc_rreg32(struct lsdc_device *ldev, u32 offset)
{
	return readl(ldev->reg_base + offset);
}

static inline void lsdc_wreg32(struct lsdc_device *ldev, u32 offset, u32 val)
{
	writel(val, ldev->reg_base + offset);
}

static inline void lsdc_ureg32_set(struct lsdc_device *ldev,
				   u32 offset,
				   u32 bit)
{
	void __iomem *addr = ldev->reg_base + offset;
	u32 val = readl(addr);

	writel(val | bit, addr);
}

static inline void lsdc_ureg32_clr(struct lsdc_device *ldev,
				   u32 offset,
				   u32 bit)
{
	void __iomem *addr = ldev->reg_base + offset;
	u32 val = readl(addr);

	writel(val & ~bit, addr);
}

static inline u32 lsdc_pipe_rreg32(struct lsdc_device *ldev,
				   u32 offset,
				   u32 pipe)
{
	return readl(ldev->reg_base + offset + pipe * CRTC_PIPE_OFFSET);
}

#define lsdc_hdmi_rreg32 lsdc_pipe_rreg32
#define lsdc_crtc_rreg32 lsdc_pipe_rreg32

static inline void lsdc_pipe_wreg32(struct lsdc_device *ldev,
				    u32 offset,
				    u32 pipe,
				    u32 val)
{
	writel(val, ldev->reg_base + offset + pipe * CRTC_PIPE_OFFSET);
}

#define lsdc_hdmi_wreg32 lsdc_pipe_wreg32
#define lsdc_crtc_wreg32 lsdc_pipe_wreg32

static inline void lsdc_crtc_ureg32_set(struct lsdc_device *ldev,
					u32 offset,
					u32 pipe,
					u32 bit)
{
	void __iomem *addr;
	u32 val;

	addr = ldev->reg_base + offset + pipe * CRTC_PIPE_OFFSET;
	val = readl(addr);
	writel(val | bit, addr);
}

static inline void lsdc_crtc_ureg32_clr(struct lsdc_device *ldev,
					u32 offset,
					u32 pipe,
					u32 bit)
{
	void __iomem *addr;
	u32 val;

	addr = ldev->reg_base + offset + pipe * CRTC_PIPE_OFFSET;
	val = readl(addr);
	writel(val & ~bit, addr);
}

/* Helpers for chip detection */
bool lsdc_is_ls2k2000(void);
bool lsdc_is_ls2k1000(void);
unsigned int loongson_cpu_get_prid(u8 *impl, u8 *rev);

#endif

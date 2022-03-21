/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KMS driver for Loongson display controller
 * Copyright (C) 2022 Loongson Corporation
 *
 * Authors:
 *      Li Yi <liyi@loongson.cn>
 *      Li Chenyang <lichenyang@loongson.cn>
 *      Sui Jingfeng <suijingfeng@loongson.cn>
 */

#ifndef __LSDC_DRV_H__
#define __LSDC_DRV_H__

#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>

#include <drm/drm_print.h>
#include <drm/drm_device.h>
#include <drm/drm_crtc.h>
#include <drm/drm_plane.h>
#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>
#include <drm/drm_drv.h>
#include <drm/drm_atomic.h>

#include "lsdc_regs.h"
#include "lsdc_pll.h"

#define LSDC_NUM_CRTC           2

/*
 * The display controller in LS7A2000 integrate three loongson self-made
 * encoder. Display pipe 0 has a transparent VGA encoder and a HDMI phy,
 * they are parallel. Display pipe 1 has only one HDMI phy.
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
 * The display controller in LS7A1000 integrate two-way DVO, external
 * encoder is required except use directly with dpi(rgb888) panel.
 *       ___________________                                     _________
 *      |            -------|                                   |         |
 *      |  CRTC0 --> | DVO0 ----> Encoder0 ---> Connector0 ---> | Display |
 *      |  _   _     -------|        ^             ^            |_________|
 *      | | | | |    -------|        |             |
 *      | |_| |_|    | i2c6 <--------+-------------+
 *      |            -------|
 *      |  _   _     -------|
 *      | | | | |    | i2c7 <--------+-------------+
 *      | |_| |_|    -------|        |             |             _________
 *      |            -------|        |             |            |         |
 *      |  CRTC1 --> | DVO1 ----> Encoder1 ---> Connector1 ---> |  Panel  |
 *      |            -------|                                   |_________|
 *      |___________________|
 *
 *
 * The display controller in LS2K1000.
 *       ___________________                                     _________
 *      |            -------|                                   |         |
 *      |  CRTC0 --> | DVO0 ----> Encoder0 ---> Connector0 ---> | Display |
 *      |  _   _     -------|        ^              ^           |_________|
 *      | | | | |           |        |              |
 *      | |_| |_|           |     +------+          |
 *      |                   <---->| i2c0 |<---------+
 *      |  DC in LS2K1000   |     +------+
 *      |  _   _            |     +------+
 *      | | | | |           <---->| i2c1 |----------+
 *      | |_| |_|           |     +------+          |            _________
 *      |            -------|        |              |           |         |
 *      |  CRTC1 --> | DVO1 ----> Encoder1 ---> Connector1 ---> |  Panel  |
 *      |            -------|                                   |_________|
 *      |___________________|
 *
 *
 * The display controller in LS2K0500, LS2K0500 has a built-in transparent
 * VGA encoder which is connected to display pipe 1(CRTC1).
 *       ___________________                                     _________
 *      |            -------|                                   |         |
 *      |  CRTC0 --> | DVO0 ----> Encoder0 ---> Connector0 ---> | Display |
 *      |  _   _     -------|        ^              ^           |_________|
 *      | | | | |           |        |              |
 *      | |_| |_|           |     +------+          |
 *      |                   <---->| i2c4 |<---------+
 *      |  DC in LS2K0500   |     +------+
 *      |  _   _            |     +------+
 *      | | | | |           <---->| i2c5 |-------------------+
 *      | |_| |_|           |     +------+             ______|______
 *      |           +-----+ |                         |             |
 *      | CRTC1 --> | VGA | ------------------------> | VGA Monitor |
 *      |           +-----+ |                         |_____________|
 *      |___________________|
 *
 * LS7A1000 and LS7A2000 are bridge chips, has dedicated Video RAM.
 * while LS2K2000/LS2K1000/LS2K0500 are SoC, they don't have dediacated
 * Video RAM. The dc in LS2K2000 is basicly same with the dc in LS7A1000
 * except that LS2K2000 don't have a video RAM and have only one built-in
 * hdmi encoder.
 *
 * There is only a 1:1 mapping of encoders and connectors for the DC,
 * each CRTC have two FB address registers.
 */

enum loongson_chip_family {
	CHIP_UNKNOWN = 0,
	CHIP_LS7A1000 = 1,  /* North bridge of LS3A3000/LS3A4000/LS3A5000 */
	CHIP_LS7A2000 = 2,  /* Update version of LS7A1000, with built-in HDMI encoder */
	CHIP_LS2K0500 = 3,  /* Reduced version of LS2K1000, single core */
	CHIP_LS2K1000 = 4,  /* 2-Core Mips64r2/LA264 SoC, 64-bit, 1.0 Ghz */
	CHIP_LS2K2000 = 5,  /* 2-Core 64-bit LA364 SoC, 1.2 ~ 1.5 Ghz */
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
	u32 pitch_align;  /* DMA alignment constraint */
	u64 mc_bits;      /* physical address bus bit width */
	bool has_vblank_counter;
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

/*
 * struct lsdc_display_pipe - Abstraction of hardware display pipeline.
 * @crtc: CRTC control structure
 * @primary: framebuffer plane control structure
 * @cursor: cursor plane control structure
 * @output: output control structure of this display pipe bind
 * @pixpll: pixel pll control structure
 * @index: the index corresponding to the hardware display pipe
 */
struct lsdc_display_pipe {
	struct drm_crtc crtc;
	struct drm_plane primary;
	struct drm_plane cursor;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct lsdc_pll pixpll;
	struct lsdc_i2c *li2c;
	int index;
};

static inline struct lsdc_display_pipe *
crtc_to_display_pipe(struct drm_crtc *crtc)
{
	return container_of(crtc, struct lsdc_display_pipe, crtc);
}

static inline struct lsdc_display_pipe *
cursor_to_display_pipe(struct drm_plane *cursor)
{
	return container_of(cursor, struct lsdc_display_pipe, cursor);
}

static inline struct lsdc_display_pipe *
connector_to_display_pipe(struct drm_connector *conn)
{
	return container_of(conn, struct lsdc_display_pipe, connector);
}

static inline struct lsdc_display_pipe *
encoder_to_display_pipe(struct drm_encoder *enc)
{
	return container_of(enc, struct lsdc_display_pipe, encoder);
}

struct lsdc_crtc_state {
	struct drm_crtc_state base;
	struct lsdc_pll_parms pparms;
};

struct lsdc_device {
	struct drm_device base;

	/* @reglock: protects concurrent register access */
	spinlock_t reglock;
	void __iomem *reg_base;
	void __iomem *vram;
	resource_size_t vram_base;
	resource_size_t vram_size;
	u64 mc_mask;

	struct lsdc_display_pipe dispipe[LSDC_NUM_CRTC];

	/* @num_output: count the number of active display pipe */
	unsigned int num_output;

	/* @descp: features description of the DC variant */
	const struct lsdc_desc *descp;

	u32 irq_status;
};

static inline struct lsdc_device *
to_lsdc(struct drm_device *ddev)
{
	return container_of(ddev, struct lsdc_device, base);
}

static inline struct lsdc_crtc_state *
to_lsdc_crtc_state(struct drm_crtc_state *base)
{
	return container_of(base, struct lsdc_crtc_state, base);
}

void lsdc_debugfs_init(struct drm_minor *minor);

int lsdc_crtc_init(struct drm_device *ddev,
		   struct drm_crtc *crtc,
		   unsigned int index,
		   struct drm_plane *primary,
		   struct drm_plane *cursor);

int lsdc_plane_init(struct lsdc_device *ldev,
		    struct drm_plane *plane,
		    enum drm_plane_type type,
		    unsigned int index);

int lsdc_create_output(struct lsdc_device *ldev, unsigned int index);

struct lsdc_i2c *lsdc_create_i2c_chan(struct drm_device *ddev,
				      void *base,
				      unsigned int index);

struct i2c_adapter *lsdc_get_i2c_adapter(struct lsdc_device *ldev, int index);

irqreturn_t lsdc_irq_thread_handler(int irq, void *arg);
irq_handler_t lsdc_get_irq_handler(struct lsdc_device *ldev);

u32 lsdc_rreg32(struct lsdc_device * const ldev, u32 offset);
void lsdc_wreg32(struct lsdc_device * const ldev, u32 offset, u32 val);

#endif

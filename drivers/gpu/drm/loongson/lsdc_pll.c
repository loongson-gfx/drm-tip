// SPDX-License-Identifier: GPL-2.0

#include <linux/delay.h>

#include "lsdc_drv.h"

/*
 * The structure of the pixel PLL register is evolved with times.
 * All loongson's cpu is little endian.
 */

/* u64 */
struct ls7a1000_pixpll_bitmap {
	/* Byte 0 ~ Byte 3 */
	unsigned div_out      : 7;   /*  0 : 6     output clock divider  */
	unsigned __1          : 14;  /*  7 : 20                          */
	unsigned loopc        : 9;   /* 21 : 29    clock multiplier      */
	unsigned __2          : 2;   /* 30 : 31                          */

	/* Byte 4 ~ Byte 7 */
	unsigned div_ref      : 7;   /*  0 : 6     input clock divider   */
	unsigned locked       : 1;   /*  7         PLL locked status     */
	unsigned sel_out      : 1;   /*  8         output clk selector   */
	unsigned __3          : 2;   /*  9 : 10                          */
	unsigned set_param    : 1;   /*  11        trigger the update    */
	unsigned bypass       : 1;   /*  12                              */
	unsigned powerdown    : 1;   /*  13                              */
	unsigned __4          : 18;  /*  14 : 31                         */
};

union lsdc_pixpll_bitmap {
	struct ls7a1000_pixpll_bitmap ls7a1000;
	u32 dword[4];
};

struct pixclk_to_pll_parm {
	/* kHz */
	unsigned int clock;

	unsigned short width;
	unsigned short height;
	unsigned short vrefresh;

	/* Stores parameters for programming the Hardware PLLs */
	unsigned short div_out;
	unsigned short loopc;
	unsigned short div_ref;
};

/*
 * Pixel clock to PLL parameters translation table.
 * Small static cached value to speed up PLL parameters calculation.
 */
static const struct pixclk_to_pll_parm pll_param_table[] = {
	{148500, 1920, 1080, 60,  11, 49,  3},   /* 1920x1080@60Hz */
						 /* 1920x1080@50Hz */
	{174500, 1920, 1080, 75,  17, 89,  3},   /* 1920x1080@75Hz */
	{181250, 2560, 1080, 75,  8, 58,  4},    /* 2560x1080@75Hz */
	{297000, 2560, 1080, 30,  8, 95,  4},    /* 3840x2160@30Hz */
	{301992, 1920, 1080, 100, 10, 151, 5},   /* 1920x1080@100Hz */
	{146250, 1680, 1050, 60,  16, 117, 5},   /* 1680x1050@60Hz */
	{135000, 1280, 1024, 75,  10, 54,  4},   /* 1280x1024@75Hz */
	{119000, 1680, 1050, 60,  20, 119, 5},   /* 1680x1050@60Hz */
	{108000, 1600, 900,  60,  15, 81,  5},   /* 1600x900@60Hz  */
						 /* 1280x1024@60Hz */
						 /* 1280x960@60Hz */
						 /* 1152x864@75Hz */

	{106500, 1440, 900,  60,  19, 81,  4},   /* 1440x900@60Hz */
	{88750,  1440, 900,  60,  16, 71,  5},   /* 1440x900@60Hz */
	{83500,  1280, 800,  60,  17, 71,  5},   /* 1280x800@60Hz */
	{71000,  1280, 800,  60,  20, 71,  5},   /* 1280x800@60Hz */

	{74250,  1280, 720,  60,  22, 49,  3},   /* 1280x720@60Hz */
						 /* 1280x720@50Hz */

	{78750,  1024, 768,  75,  16, 63,  5},   /* 1024x768@75Hz */
	{75000,  1024, 768,  70,  29, 87,  4},   /* 1024x768@70Hz */
	{65000,  1024, 768,  60,  20, 39,  3},   /* 1024x768@60Hz */

	{51200,  1024, 600,  60,  25, 64,  5},   /* 1024x600@60Hz */

	{57284,  832,  624,  75,  24, 55,  4},   /* 832x624@75Hz */
	{49500,  800,  600,  75,  40, 99,  5},   /* 800x600@75Hz */
	{50000,  800,  600,  72,  44, 88,  4},   /* 800x600@72Hz */
	{40000,  800,  600,  60,  30, 36,  3},   /* 800x600@60Hz */
	{36000,  800,  600,  56,  50, 72,  4},   /* 800x600@56Hz */
	{31500,  640,  480,  75,  40, 63,  5},   /* 640x480@75Hz */
						 /* 640x480@73Hz */

	{30240,  640,  480,  67,  62, 75,  4},   /* 640x480@67Hz */
	{27000,  720,  576,  50,  50, 54,  4},   /* 720x576@60Hz */
	{25175,  640,  480,  60,  85, 107, 5},   /* 640x480@60Hz */
	{25200,  640,  480,  60,  50, 63,  5},   /* 640x480@60Hz */
						 /* 720x480@60Hz */
};

/*
 * lsdc_pixpll_setup - ioremap the device dependent PLL registers
 *
 * @this: point to the object where this function is called from
 */
static int lsdc_pixpll_setup(struct lsdc_pll * const this)
{
	this->mmio = ioremap(this->reg_base, this->reg_size);

	return 0;
}

/*
 * Find a set of pll parameters from a static local table which avoid
 * computing the pll parameter eachtime a modeset is triggered.
 *
 * @this: point to the object where this function is called from
 * @clock: the desired output pixel clock, the unit is kHz
 * @pout: point to where the parameters to store if found
 *
 * Return 0 if success, return -1 if not found.
 */
static int lsdc_pixpll_find(struct lsdc_pll * const this,
			    unsigned int clock,
			    struct lsdc_pll_parms *pout)
{
	unsigned int num = ARRAY_SIZE(pll_param_table);
	unsigned int i;

	for (i = 0; i < num; ++i) {
		if (clock != pll_param_table[i].clock)
			continue;

		pout->div_ref = pll_param_table[i].div_ref;
		pout->loopc   = pll_param_table[i].loopc;
		pout->div_out = pll_param_table[i].div_out;

		return 0;
	}

	drm_dbg(this->ddev, "pixel clock %u: miss\n", clock);

	return -1;
}

/*
 * Find a set of pll parameters which have minimal difference with the
 * desired pixel clock frequency. It does that by computing all of the
 * possible combination. Compute the diff and find the combination with
 * minimal diff.
 *
 *  clock_out = refclk / div_ref * loopc / div_out
 *
 *  refclk is determined by the oscillator mounted on the motherboard(
 *  Here is 100MHz in almost all board)
 *
 * @this: point to the object from where this function is called
 * @clock_khz: the desired output pixel clock, the unit is kHz
 * @pout: point to the out struct of lsdc_pll_parms
 *
 *  Return 0 if a set of parameter is found, otherwise return the error
 *  between clock_kHz we wanted and the most closest candidate with it.
 */
static int lsdc_pixpll_compute(struct lsdc_pll * const this,
			       unsigned int clock_khz,
			       struct lsdc_pll_parms *pout)
{
	const unsigned int tolerance = 1000;
	unsigned int refclk = this->ref_clock;
	unsigned int min = tolerance;
	unsigned int div_out, loopc, div_ref;
	unsigned int computed;

	if (!lsdc_pixpll_find(this, clock_khz, pout))
		return 0;

	for (div_out = 6; div_out < 64; div_out++) {
		for (div_ref = 3; div_ref < 6; div_ref++) {
			for (loopc = 6; loopc < 161; loopc++) {
				unsigned int diff;

				if (loopc < 12 * div_ref)
					continue;
				if (loopc > 32 * div_ref)
					continue;

				computed = refclk / div_ref * loopc / div_out;

				if (clock_khz >= computed)
					diff = clock_khz - computed;
				else if (clock_khz < computed)
					diff = computed - clock_khz;

				if (diff < min) {
					min = diff;
					pout->div_ref = div_ref;
					pout->div_out = div_out;
					pout->loopc = loopc;

					if (diff == 0)
						return 0;
				}
			}
		}
	}

	if (min < tolerance)
		return 0;

	return min;
}

/*
 * Update the pll parameters to hardware, target to the pixpll in ls7a1000
 *
 * @this: point to the object from which this function is called
 * @pin: point to the struct of lsdc_pll_parms passed in
 *
 * return 0 if successful.
 */
static int ls7a1000_pixpll_param_update(struct lsdc_pll * const this,
					struct lsdc_pll_parms const *pin)
{
	void __iomem *reg = this->mmio;
	unsigned int counter = 0;
	bool locked;
	u32 val;

	/* Bypass the software configured PLL, using refclk directly */
	val = readl(reg + 0x4);
	val &= ~(1 << 8);
	writel(val, reg + 0x4);

	/* Powerdown the PLL */
	val = readl(reg + 0x4);
	val |= (1 << 13);
	writel(val, reg + 0x4);

	/* Clear the pll parameters */
	val = readl(reg + 0x4);
	val &= ~(1 << 11);
	writel(val, reg + 0x4);

	/* clear old value & config new value */
	val = readl(reg + 0x04);
	val &= ~0x7F;
	val |= pin->div_ref;        /* div_ref */
	writel(val, reg + 0x4);

	val = readl(reg);
	val &= ~0x7f;
	val |= pin->div_out;        /* div_out */

	val &= ~(0x1ff << 21);
	val |= pin->loopc << 21;    /* loopc */
	writel(val, reg);

	/* Set the pll the parameters */
	val = readl(reg + 0x4);
	val |= (1 << 11);
	writel(val, reg + 0x4);

	/* Powerup the PLL */
	val = readl(reg + 0x4);
	val &= ~(1 << 13);
	writel(val, reg + 0x4);

	udelay(1);

	/* Wait the PLL lock */
	do {
		val = readl(reg + 0x4);
		locked = val & 0x80;
		if (locked)
			break;
		++counter;
	} while (counter < 2000);

	drm_dbg(this->ddev, "%u loop waited\n", counter);

	/* Switch to the configured pll just now */
	val = readl(reg + 0x4);
	val |= (1UL << 8);
	writel(val, reg + 0x4);

	return 0;
}

static unsigned int ls7a1000_get_clock_rate(struct lsdc_pll * const this,
					    struct lsdc_pll_parms *pout)
{
	union lsdc_pixpll_bitmap parms;
	struct ls7a1000_pixpll_bitmap *obj = &parms.ls7a1000;
	unsigned int out;

	parms.dword[0] = readl(this->mmio);
	parms.dword[1] = readl(this->mmio + 4);
	out = this->ref_clock / obj->div_ref * obj->loopc / obj->div_out;
	if (pout) {
		pout->div_ref = obj->div_ref;
		pout->loopc = obj->loopc;
		pout->div_out = obj->div_out;
	}

	return out;
}

static const struct lsdc_pixpll_funcs ls7a1000_pixpll_funcs = {
	.setup = lsdc_pixpll_setup,
	.compute = lsdc_pixpll_compute,
	.update = ls7a1000_pixpll_param_update,
	.get_clock_rate = ls7a1000_get_clock_rate,
};

int lsdc_pixpll_init(struct lsdc_pll * const this,
		     struct drm_device *ddev,
		     unsigned int index)
{
	struct lsdc_device *ldev = to_lsdc(ddev);
	const struct lsdc_desc *descp = ldev->descp;

	this->ddev = ddev;
	this->index = index;
	this->ref_clock = LSDC_PLL_REF_CLK;

	/* LS7A1000, LS7A2000's setting registers is same */
	if (descp->chip == CHIP_LS7A2000 ||
	    descp->chip == CHIP_LS7A1000) {
		if (index == 0)
			this->reg_base = LS7A1000_CFG_REG_BASE + LS7A1000_PIX_PLL0_REG;
		else if (index == 1)
			this->reg_base = LS7A1000_CFG_REG_BASE + LS7A1000_PIX_PLL1_REG;
		this->reg_size = 8;
		this->funcs = &ls7a1000_pixpll_funcs;
	} else {
		drm_err(ddev, "unknown chip, the driver need update\n");
		return -ENOENT;
	}

	return this->funcs->setup(this);
}

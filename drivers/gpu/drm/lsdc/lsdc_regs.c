// SPDX-License-Identifier: GPL-2.0

#include "lsdc_drv.h"
#include "lsdc_regs.h"

u32 lsdc_rreg32(struct lsdc_device * const ldev, u32 offset)
{
	unsigned long flags;
	u32 ret;

	spin_lock_irqsave(&ldev->reglock, flags);

	ret = readl(ldev->reg_base + offset);

	spin_unlock_irqrestore(&ldev->reglock, flags);

	return ret;
}

void lsdc_wreg32(struct lsdc_device * const ldev, u32 offset, u32 val)
{
	unsigned long flags;

	spin_lock_irqsave(&ldev->reglock, flags);

	writel(val, ldev->reg_base + offset);

	spin_unlock_irqrestore(&ldev->reglock, flags);
}

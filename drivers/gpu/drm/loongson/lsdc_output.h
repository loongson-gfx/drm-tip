/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LSDC_OUTPUT_H__
#define __LSDC_OUTPUT_H__

#include "lsdc_drv.h"

int lsdc_create_output(struct lsdc_device *ldev, struct lsdc_display_pipe *p);

struct lsdc_i2c *lsdc_create_i2c_chan(struct drm_device *ddev,
				      void *base,
				      unsigned int index);

#endif

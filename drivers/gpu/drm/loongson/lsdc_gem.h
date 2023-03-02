/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LSDC_GEM_H__
#define __LSDC_GEM_H__

#include <drm/drm_device.h>
#include <drm/drm_gem.h>

struct drm_gem_object *
lsdc_prime_import_sg_table(struct drm_device *ddev,
			   struct dma_buf_attachment *attach,
			   struct sg_table *sg);

int lsdc_dumb_map_offset(struct drm_file *file,
			 struct drm_device *dev,
			 u32 handle,
			 uint64_t *offset);

int lsdc_dumb_create(struct drm_file *file,
		     struct drm_device *ddev,
		     struct drm_mode_create_dumb *args);

void lsdc_gem_init(struct drm_device *ddev);
void lsdc_gem_debugfs_init(struct drm_minor *primary);

#endif

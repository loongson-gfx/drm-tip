/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LSDC_TTM_H__
#define __LSDC_TTM_H__

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_modes.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_placement.h>

#include <linux/container_of.h>
#include <linux/iosys-map.h>

#define DRM_GEM_VRAM_PL_FLAG_SYSTEM	(1 << 0)
#define DRM_GEM_VRAM_PL_FLAG_VRAM	(1 << 1)
#define DRM_GEM_VRAM_PL_FLAG_TOPDOWN	(1 << 2)

/*
 * @bo:		TTM buffer object
 * @map:	Mapping information for @bo
 * @placement:	TTM placement information.
 * @placements:	TTM placement information.
 *
 * The type struct lsdc_bo represents a GEM object that is backed by VRAM.
 * It can be used for simple framebuffer devices with dedicated memory.
 * The buffer object can be evicted to system memory if video memory becomes
 * scarce.
 */
struct lsdc_bo {
	struct ttm_buffer_object bo;
	struct iosys_map map;

	/*
	 * @vmap_use_count:
	 *
	 * Reference count on the virtual address.
	 * The address are un-mapped when the count reaches zero.
	 */
	unsigned int vmap_use_count;

	/* Supported placements are TTM_PL_VRAM and TTM_PL_SYSTEM */
	struct ttm_placement placement;
	struct ttm_place placements[2];
};

static inline struct lsdc_bo *to_lsdc_bo(struct ttm_buffer_object *tbo)
{
	return container_of(tbo, struct lsdc_bo, bo);
}

static inline struct lsdc_bo *gem_to_lsdc_bo(struct drm_gem_object *gem)
{
	return container_of(gem, struct lsdc_bo, bo.base);
}

static inline struct ttm_buffer_object *to_ttm_bo(struct drm_gem_object *gem)
{
	return container_of(gem, struct ttm_buffer_object, base);
}

s64 lsdc_get_vram_bo_offset(struct drm_framebuffer *fb);

int lsdc_plane_prepare_fb(struct drm_plane *plane,
			  struct drm_plane_state *new_state);

void lsdc_plane_cleanup_fb(struct drm_plane *plane,
			   struct drm_plane_state *old_state);

enum drm_mode_status
lsdc_bo_mode_valid(struct drm_device *dev,
		   const struct drm_display_mode *mode);

/* lsdc_ttm */

int lsdc_dumb_map_offset(struct drm_file *file,
			 struct drm_device *dev,
			 uint32_t handle,
			 uint64_t *offset);

int lsdc_gem_dumb_create(struct drm_file *file,
			 struct drm_device *ddev,
			 struct drm_mode_create_dumb *args);

int lsdc_ttm_init(struct lsdc_device *ldev);

#endif

/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LSDC_TTM_H__
#define __LSDC_TTM_H__

#include <linux/container_of.h>
#include <linux/iosys-map.h>

#include <drm/drm_gem.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_range_manager.h>
#include <drm/ttm/ttm_tt.h>

#define LSDC_GEM_DOMAIN_SYSTEM          0x1
#define LSDC_GEM_DOMAIN_GTT             0x2
#define LSDC_GEM_DOMAIN_VRAM            0x4

struct lsdc_bo {
	struct ttm_buffer_object tbo;
	struct iosys_map map;

	unsigned int vmap_use_count;
	unsigned int prime_shared_count;

	struct ttm_placement placement;
	struct ttm_place placements[2];

	/* Protected by gem.mutex */
	struct list_head list;
};

static inline struct ttm_buffer_object *to_ttm_bo(struct drm_gem_object *gem)
{
	return container_of(gem, struct ttm_buffer_object, base);
}

static inline struct lsdc_bo *to_lsdc_bo(struct ttm_buffer_object *tbo)
{
	return container_of(tbo, struct lsdc_bo, tbo);
}

static inline struct lsdc_bo *gem_to_lsdc_bo(struct drm_gem_object *obj)
{
	return container_of(obj, struct lsdc_bo, tbo.base);
}

struct lsdc_bo *lsdc_bo_create(struct drm_device *ddev,
			       u32 domain,
			       u32 flags,
			       size_t size,
			       struct sg_table *sg,
			       struct dma_resv *resv);

bool lsdc_bo_is_ttm_bo(struct ttm_buffer_object *tbo);

int lsdc_bo_pin(struct drm_gem_object *gem);
void lsdc_bo_unpin(struct drm_gem_object *gem);

u64 lsdc_bo_gpu_offset(struct ttm_buffer_object *tbo);
unsigned long lsdc_bo_size(struct lsdc_bo *lbo);

void lsdc_bo_set_placement(struct lsdc_bo *lbo, u32 domain, u32 flags);

int lsdc_ttm_init(struct lsdc_device *ldev);

#endif

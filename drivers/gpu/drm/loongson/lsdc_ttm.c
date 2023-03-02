// SPDX-License-Identifier: GPL-2.0+

#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_managed.h>

#include "lsdc_drv.h"
#include "lsdc_ttm.h"

static void lsdc_ttm_tt_destroy(struct ttm_device *bdev, struct ttm_tt *tt)
{
	ttm_tt_fini(tt);
	kfree(tt);
}

static struct ttm_tt *
lsdc_ttm_tt_create(struct ttm_buffer_object *tbo, uint32_t page_flags)
{
	struct drm_device *ddev = tbo->base.dev;
	struct ttm_tt *tt;
	int ret;

	tt = kzalloc(sizeof(*tt), GFP_KERNEL);
	if (!tt)
		return NULL;

	ret = ttm_tt_init(tt, tbo, page_flags, ttm_cached, 0);
	if (ret < 0) {
		kfree(tt);
		return NULL;
	}

#if 1
	drm_info(ddev, "ttm_tt create\n");
#endif

	return tt;
}

static void lsdc_bo_evict_flags(struct ttm_buffer_object *tbo,
				struct ttm_placement *placement)
{
	struct lsdc_bo *lbo = to_lsdc_bo(tbo);
	struct drm_device *ddev = tbo->base.dev;

	if (!lsdc_bo_is_ttm_bo(tbo)) {
#if 1
		drm_info(ddev, "is not a ttm bo\n");
#endif
		return;
	}

	lsdc_bo_set_placement(lbo, LSDC_GEM_DOMAIN_SYSTEM, 0);

	*placement = lbo->placement;
}

static int lsdc_bo_move(struct ttm_buffer_object *tbo,
			bool evict,
			struct ttm_operation_ctx *ctx,
			struct ttm_resource *new_mem,
			struct ttm_place *hop)
{
	struct lsdc_bo *lbo = to_lsdc_bo(tbo);
	struct drm_device *ddev = tbo->base.dev;

	drm_info(ddev, "move to %u\n", new_mem->placement);

	if (drm_WARN_ON_ONCE(ddev, lbo->vmap_use_count))
		goto just_move_it;

	ttm_bo_vunmap(tbo, &lbo->map);
	/* explicitly clear mapping for next vmap call */
	iosys_map_clear(&lbo->map);

just_move_it:
	return ttm_bo_move_memcpy(tbo, ctx, new_mem);
}

static void lsdc_bo_delete_mem_notify(struct ttm_buffer_object *tbo)
{
	struct lsdc_bo *lbo = to_lsdc_bo(tbo);
	struct drm_device *ddev = tbo->base.dev;

	if (!lsdc_bo_is_ttm_bo(tbo)) {
#if 1
		drm_info(ddev, "is not a ttm bo\n");
#endif
		return;
	}

	if (drm_WARN_ON_ONCE(ddev, lbo->vmap_use_count))
		return;

	ttm_bo_vunmap(tbo, &lbo->map);
	iosys_map_clear(&lbo->map);
}

static int lsdc_bo_reserve_io_mem(struct ttm_device *bdev,
				  struct ttm_resource *mem)
{
	struct lsdc_device *ldev = tdev_to_ldev(bdev);

	switch (mem->mem_type) {
	case TTM_PL_SYSTEM:
		/* nothing to do */
		break;
	case TTM_PL_TT:
		break;
	case TTM_PL_VRAM:
		mem->bus.offset = (mem->start << PAGE_SHIFT) + ldev->vram_base;
		mem->bus.is_iomem = true;
		mem->bus.caching = ttm_write_combined;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static struct ttm_device_funcs lsdc_bo_driver = {
	.ttm_tt_create = lsdc_ttm_tt_create,
	.ttm_tt_destroy = lsdc_ttm_tt_destroy,
	.eviction_valuable = ttm_bo_eviction_valuable,
	.evict_flags = lsdc_bo_evict_flags,
	.move = lsdc_bo_move,
	.delete_mem_notify = lsdc_bo_delete_mem_notify,
	.io_mem_reserve = lsdc_bo_reserve_io_mem,
};

void lsdc_bo_set_placement(struct lsdc_bo *lbo, u32 domain, u32 flags)
{
	unsigned int i;
	unsigned int c = 0;

	lbo->placement.placement = lbo->placements;
	lbo->placement.busy_placement = lbo->placements;

	if (domain & LSDC_GEM_DOMAIN_VRAM) {
		lbo->placements[c].mem_type = TTM_PL_VRAM;
		lbo->placements[c++].flags = flags;
	}
#if 1
	if (domain & LSDC_GEM_DOMAIN_GTT) {
		lbo->placements[c].mem_type = TTM_PL_TT;
		lbo->placements[c++].flags = flags;
	}
#endif
	if (domain & LSDC_GEM_DOMAIN_SYSTEM || !c) {
		lbo->placements[c].mem_type = TTM_PL_SYSTEM;
		lbo->placements[c++].flags = flags;
	}

	lbo->placement.num_placement = c;
	lbo->placement.num_busy_placement = c;

	for (i = 0; i < c; ++i) {
		lbo->placements[i].fpfn = 0;
		lbo->placements[i].lpfn = 0;
	}
}

int lsdc_bo_pin(struct drm_gem_object *gem)
{
	struct ttm_buffer_object *tbo = to_ttm_bo(gem);
	struct lsdc_bo *lbo = to_lsdc_bo(tbo);
	int ret;

	ret = ttm_bo_reserve(tbo, true, false, NULL);
	if (ret)
		return ret;

	if (tbo->pin_count == 0) {
		struct ttm_operation_ctx ctx = { false, false };

		ret = ttm_bo_validate(tbo, &lbo->placement, &ctx);
		if (ret < 0) {
			ttm_bo_unreserve(tbo);
			return ret;
		}
	}

	ttm_bo_pin(tbo);

	ttm_bo_unreserve(tbo);

	return ret;
}

void lsdc_bo_unpin(struct drm_gem_object *gem)
{
	struct ttm_buffer_object *tbo = to_ttm_bo(gem);
	int ret;

	ret = ttm_bo_reserve(tbo, true, false, NULL);
	if (ret)
		return;

	ttm_bo_unpin(tbo);
	ttm_bo_unreserve(tbo);
}

u64 lsdc_bo_gpu_offset(struct ttm_buffer_object *tbo)
{
	struct ttm_resource *resource = tbo->resource;

	if (WARN_ON_ONCE(!tbo->pin_count))
		return -ENODEV;

	if (WARN_ON_ONCE(resource->mem_type == TTM_PL_SYSTEM))
		return 0;

	return resource->start << PAGE_SHIFT;
}

unsigned long lsdc_bo_size(struct lsdc_bo *lbo)
{
	return lbo->tbo.base.size;
}

static void lsdc_bo_destroy(struct ttm_buffer_object *tbo)
{
	struct lsdc_bo *lbo = to_lsdc_bo(tbo);

	WARN_ON(lbo->vmap_use_count);
	WARN_ON(iosys_map_is_set(&lbo->map));

	drm_gem_object_release(&tbo->base);

	kfree(lbo);
}

struct lsdc_bo *lsdc_bo_create(struct drm_device *ddev,
			       u32 domain,
			       u32 flags,
			       size_t size,
			       struct sg_table *sg,
			       struct dma_resv *resv)
{
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct ttm_device *bdev = &ldev->bdev;
	struct ttm_buffer_object *tbo;
	struct lsdc_bo *lbo;
	enum ttm_bo_type bo_type;
	int ret;

	lbo = kzalloc(sizeof(*lbo), GFP_KERNEL);
	if (!lbo)
		return ERR_PTR(-ENOMEM);

	lsdc_bo_set_placement(lbo, domain, flags);

	tbo = &lbo->tbo;

	ret = drm_gem_object_init(ddev, &tbo->base, size);
	if (ret) {
		kfree(lbo);
		return ERR_PTR(ret);
	}

	tbo->bdev = bdev;

	if (sg)
		bo_type = ttm_bo_type_sg;
	else
		bo_type = ttm_bo_type_device;

	ret = ttm_bo_init_validate(bdev,
				   tbo,
				   bo_type,
				   &lbo->placement,
				   0,
				   false,
				   sg,
				   resv,
				   lsdc_bo_destroy);
	if (ret) {
		kfree(lbo);
		return ERR_PTR(ret);
	}

	return lbo;
}

bool lsdc_bo_is_ttm_bo(struct ttm_buffer_object *tbo)
{
	return (tbo->destroy == &lsdc_bo_destroy);
}

static void lsdc_ttm_fini(struct drm_device *ddev, void *data)
{
	struct lsdc_device *ldev = (struct lsdc_device *)data;

	ttm_range_man_fini(&ldev->bdev, TTM_PL_VRAM);
	ttm_device_fini(&ldev->bdev);
}

int lsdc_ttm_init(struct lsdc_device *ldev)
{
	struct drm_device *ddev = &ldev->base;
	unsigned long num_pages;
	int ret;

	ret = ttm_device_init(&ldev->bdev,
			      &lsdc_bo_driver,
			      ddev->dev,
			      ddev->anon_inode->i_mapping,
			      ddev->vma_offset_manager,
			      false,
			      true);
	if (ret)
		return ret;

	num_pages = ldev->vram_size >> PAGE_SHIFT;

	ret = ttm_range_man_init(&ldev->bdev,
				 TTM_PL_VRAM,
				 false,
				 num_pages);
	if (ret)
		return ret;

	drm_dbg(ddev, "total number of pages: %lu\n", num_pages);

	return drmm_add_action_or_reset(ddev, lsdc_ttm_fini, ldev);
}

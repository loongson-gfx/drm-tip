// SPDX-License-Identifier: GPL-2.0+

#include <drm/drm_managed.h>
#include <drm/drm_gem.h>
#include <drm/ttm/ttm_tt.h>
#include <drm/ttm/ttm_range_manager.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include "lsdc_drv.h"
#include "lsdc_ttm.h"

static void lsdc_ttm_tt_destroy(struct ttm_device *bdev, struct ttm_tt *tt)
{
	ttm_tt_fini(tt);
	kfree(tt);
}

static struct ttm_tt *
lsdc_ttm_tt_create(struct ttm_buffer_object *bo, uint32_t page_flags)
{
	struct ttm_tt *tt;
	int ret;

	tt = kzalloc(sizeof(*tt), GFP_KERNEL);
	if (!tt)
		return NULL;

	ret = ttm_tt_init(tt, bo, page_flags, ttm_cached, 0);
	if (ret < 0)
		goto err_ttm_tt_init;

	return tt;

err_ttm_tt_init:
	kfree(tt);
	return NULL;
}

static void ttm_buffer_object_destroy(struct ttm_buffer_object *bo)
{
	struct lsdc_bo *lbo = to_lsdc_bo(bo);

	/* We got here via ttm_bo_put(), which means that the
	 * TTM buffer object in 'bo' has already been cleaned
	 * up; only release the GEM object.
	 */

	WARN_ON(lbo->vmap_use_count);
	WARN_ON(iosys_map_is_set(&lbo->map));

	drm_gem_object_release(&lbo->bo.base);

	kfree(lbo);
}

static bool lsdc_gem_is_vram(struct ttm_buffer_object *bo)
{
	return (bo->destroy == ttm_buffer_object_destroy);
}

static void lsdc_gem_vram_placement(struct lsdc_bo *lbo, unsigned long pl_flag)
{
	u32 invariant_flags = 0;
	unsigned int i;
	unsigned int c = 0;

	if (pl_flag & DRM_GEM_VRAM_PL_FLAG_TOPDOWN)
		invariant_flags = TTM_PL_FLAG_TOPDOWN;

	lbo->placement.placement = lbo->placements;
	lbo->placement.busy_placement = lbo->placements;

	if (pl_flag & DRM_GEM_VRAM_PL_FLAG_VRAM) {
		lbo->placements[c].mem_type = TTM_PL_VRAM;
		lbo->placements[c++].flags = invariant_flags;
	}

	if (pl_flag & DRM_GEM_VRAM_PL_FLAG_SYSTEM || !c) {
		lbo->placements[c].mem_type = TTM_PL_SYSTEM;
		lbo->placements[c++].flags = invariant_flags;
	}

	lbo->placement.num_placement = c;
	lbo->placement.num_busy_placement = c;

	for (i = 0; i < c; ++i) {
		lbo->placements[i].fpfn = 0;
		lbo->placements[i].lpfn = 0;
	}
}

static void drm_gem_vram_bo_driver_move_notify(struct lsdc_bo *gbo)
{
	struct ttm_buffer_object *bo = &gbo->bo;
	struct drm_device *ddev = bo->base.dev;

	if (drm_WARN_ON_ONCE(ddev, gbo->vmap_use_count))
		return;

	ttm_bo_vunmap(bo, &gbo->map);
	/* explicitly clear mapping for next vmap call */
	iosys_map_clear(&gbo->map);
}

static void lsdc_ttm_bo_evict_flags(struct ttm_buffer_object *tbo,
				    struct ttm_placement *placement)
{
	struct lsdc_bo *lbo;

	/* TTM may pass BOs that are not GEM VRAM BOs. */
	if (!lsdc_gem_is_vram(tbo))
		return;

	lbo = to_lsdc_bo(tbo);

	lsdc_gem_vram_placement(lbo, DRM_GEM_VRAM_PL_FLAG_SYSTEM);

	*placement = lbo->placement;
}

static int lsdc_ttm_bo_move(struct ttm_buffer_object *bo,
			    bool evict,
			    struct ttm_operation_ctx *ctx,
			    struct ttm_resource *new_mem,
			    struct ttm_place *hop)
{
	struct lsdc_bo *lbo = to_lsdc_bo(bo);

	drm_gem_vram_bo_driver_move_notify(lbo);

#if 1
	drm_info(bo->base.dev, "%s: \n", __func__);
#endif

	return ttm_bo_move_memcpy(bo, ctx, new_mem);
}

static void lsdc_ttm_delete_mem_notify(struct ttm_buffer_object *bo)
{
	struct lsdc_bo *lbo;

	/* TTM may pass BOs that are not GEM VRAM BOs. */
	if (!lsdc_gem_is_vram(bo))
		return;

	lbo = to_lsdc_bo(bo);

	drm_gem_vram_bo_driver_move_notify(lbo);
}

static int lsdc_ttm_io_mem_reserve(struct ttm_device *bdev,
				   struct ttm_resource *mem)
{
	struct lsdc_device *ldev = bdev_to_lsdc(bdev);

	switch (mem->mem_type) {
	case TTM_PL_SYSTEM:	/* nothing to do */
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
	.evict_flags = lsdc_ttm_bo_evict_flags,
	.move = lsdc_ttm_bo_move,
	.delete_mem_notify = lsdc_ttm_delete_mem_notify,
	.io_mem_reserve = lsdc_ttm_io_mem_reserve,
};


/*
 * @file:	DRM file pointer.
 * @ddev:	DRM device.
 * @handle:	GEM handle
 * @offset:	Returns the mapping's memory offset on success
 *
 * Provides an implementation of struct &drm_driver.dumb_map_offset for
 * TTM-based GEM drivers. TTM allocates the offset internally and
 * lsdc_dumb_map_offset() returns it for dumb-buffer implementations.
 *
 * See struct &drm_driver.dumb_map_offset.
 *
 * Returns:
 * 0 on success, or a negative errno code otherwise.
 */
int lsdc_dumb_map_offset(struct drm_file *file,
			 struct drm_device *ddev,
			 uint32_t handle,
			 uint64_t *offset)
{
	struct drm_gem_object *gem;

	gem = drm_gem_object_lookup(file, handle);
	if (!gem)
		return -ENOENT;

	*offset = drm_vma_node_offset_addr(&gem->vma_node);
#if 1
	drm_info(ddev, "%s: %llu\n", __func__, *offset);
#endif
	drm_gem_object_put(gem);

	return 0;
}

static void lsdc_bo_free(struct drm_gem_object *gem)
{
	struct ttm_buffer_object *tbo = to_ttm_bo(gem);

	ttm_bo_put(tbo);
}

static int lsdc_bo_validate(struct lsdc_bo *gbo, unsigned long pl_flag)
{
	struct ttm_operation_ctx ctx = { false, false };
	int ret;

	if (gbo->bo.pin_count)
		goto out;

	if (pl_flag)
		lsdc_gem_vram_placement(gbo, pl_flag);

	ret = ttm_bo_validate(&gbo->bo, &gbo->placement, &ctx);
	if (ret < 0)
		return ret;

out:
	ttm_bo_pin(&gbo->bo);

	return 0;
}

/*
 * Pins a GEM VRAM object in a region.
 * @gem: the GEM VRAM object
 *
 * Pinning a buffer object ensures that it is not evicted from a
 * memory region. A pinned buffer object has to be unpinned before
 * it can be pinned to another region. If the pl_flag argument is 0,
 * the buffer is pinned at its current location (video RAM or system
 * memory).
 *
 * Small buffer objects, such as cursor images, can lead to memory
 * fragmentation if they are pinned in the middle of video RAM. This
 * is especially a problem on devices with only a small amount of
 * video RAM. Fragmentation can prevent the primary framebuffer from
 * fitting in, even though there's enough memory overall. The modifier
 * DRM_GEM_VRAM_PL_FLAG_TOPDOWN marks the buffer object to be pinned
 * at the high end of the memory region to avoid fragmentation.
 *
 * Returns:
 * 0 on success, or
 * a negative error code otherwise.
 */
static int lsdc_bo_pin(struct drm_gem_object *gem)
{
	struct ttm_buffer_object *tbo = to_ttm_bo(gem);
	struct lsdc_bo *lbo = to_lsdc_bo(tbo);
	int ret;

	/* Fbdev console emulation is the use case of these PRIME
	 * helpers. This may involve updating a hardware buffer from
	 * a shadow FB. We pin the buffer to it's current location
	 * (either video RAM or system memory) to prevent it from
	 * being relocated during the update operation. If you require
	 * the buffer to be pinned to VRAM, implement a callback that
	 * sets the flags accordingly.
	 */

	ret = ttm_bo_reserve(tbo, true, false, NULL);
	if (ret) {
		drm_err(gem->dev, "%s: %d\n", __func__, ret);
		return ret;
	}

	drm_info(gem->dev, "%s: \n", __func__);

	ret = lsdc_bo_validate(lbo, 0);

	ttm_bo_unreserve(tbo);

	return ret;
}

static void lsdc_bo_unpin(struct drm_gem_object *gem)
{
	struct ttm_buffer_object *tbo = to_ttm_bo(gem);
	int ret;

	ret = ttm_bo_reserve(tbo, true, false, NULL);
	if (ret) {
		drm_err(gem->dev, "%s: bo reserve failed\n", __func__);
		return;
	}

	ttm_bo_unpin(tbo);
	ttm_bo_unreserve(tbo);
}

static int lsdc_bo_kmap_locked(struct lsdc_bo *gbo, struct iosys_map *map)
{
	int ret;

	if (gbo->vmap_use_count > 0)
		goto out;

	/*
	 * VRAM helpers unmap the BO only on demand. So the previous
	 * page mapping might still be around. Only vmap if the there's
	 * no mapping present.
	 */
	if (iosys_map_is_null(&gbo->map)) {
		ret = ttm_bo_vmap(&gbo->bo, &gbo->map);
		if (ret)
			return ret;
	}

out:
	++gbo->vmap_use_count;
	*map = gbo->map;

	return 0;
}

/*
 * Pins and maps a GEM VRAM object into kernel address space
 *
 * @gem: The GEM object to map
 * @map: Returns the kernel virtual address of the VRAM GEM object's backing
 *       store.
 *
 * The vmap function pins a GEM VRAM object to its current location, either
 * system or video memory, and maps its buffer into kernel address space.
 * As pinned object cannot be relocated, you should avoid pinning objects
 * permanently. Call drm_gem_vram_vunmap() with the returned address to
 * unmap and unpin the GEM VRAM object.
 *
 * Returns:
 * 0 on success, or a negative error code otherwise.
 */
static int lsdc_bo_vmap(struct drm_gem_object *gem, struct iosys_map *map)
{
	struct ttm_buffer_object *tbo = to_ttm_bo(gem);
	struct lsdc_bo *lbo = to_lsdc_bo(tbo);
	int ret;

	dma_resv_assert_held(gem->resv);

	ret = lsdc_bo_validate(lbo, 0);
	if (ret)
		return ret;

	ret = lsdc_bo_kmap_locked(lbo, map);
	if (ret)
		goto err_kmap;

	return 0;

err_kmap:
	ttm_bo_unpin(tbo);
	return ret;
}

/*
 * @gem: The GEM object to unmap
 * @map: Kernel virtual address where the VRAM GEM object was mapped
 */
static void lsdc_bo_vunmap(struct drm_gem_object *gem, struct iosys_map *map)
{
	struct drm_device *ddev = gem->dev;
	struct ttm_buffer_object *tbo = to_ttm_bo(gem);
	struct lsdc_bo *lbo = to_lsdc_bo(tbo);

	dma_resv_assert_held(gem->resv);

	if (drm_WARN_ON_ONCE(ddev, !lbo->vmap_use_count))
		return;

	if (drm_WARN_ON_ONCE(ddev, !iosys_map_is_equal(&lbo->map, map)))
		return; /* BUG: map not mapped from this BO */

	if (--lbo->vmap_use_count > 0)
		return;

	/*
	 * Permanently mapping and unmapping buffers adds overhead from
	 * updating the page tables and creates debugging output. Therefore,
	 * we delay the actual unmap operation until the BO gets evicted
	 * from memory. See drm_gem_vram_bo_driver_move_notify().
	 */

	ttm_bo_unpin(tbo);
}

/*
 * @gem: GEM object.
 * @vma: vm area.
 */
static int lsdc_ttm_mmap(struct drm_gem_object *gem,
			 struct vm_area_struct *vma)
{
	struct ttm_buffer_object *tbo = to_ttm_bo(gem);
	int ret;

	ret = ttm_bo_mmap_obj(vma, tbo);
	if (ret < 0)
		return ret;

	/*
	 * ttm has its own object refcounting, so drop gem reference
	 * to avoid double accounting counting.
	 */
	drm_gem_object_put(gem);

	return 0;
}

/*
 * @p: DRM printer
 * @indent: Tab indentation level
 * @gem: GEM object
 */
static void lsdc_ttm_print_info(struct drm_printer *p,
				unsigned int indent,
				const struct drm_gem_object *gem)
{
	static const char * const plname[] = {
		[ TTM_PL_SYSTEM ] = "system",
		[ TTM_PL_TT     ] = "tt",
		[ TTM_PL_VRAM   ] = "vram",
		[ TTM_PL_PRIV   ] = "priv",

		[ 16 ]            = "cached",
		[ 17 ]            = "uncached",
		[ 18 ]            = "wc",
		[ 19 ]            = "contig",

		[ 21 ]            = "pinned", /* NO_EVICT */
		[ 22 ]            = "topdown",
	};
	struct ttm_buffer_object *tbo = to_ttm_bo((struct drm_gem_object *)gem);
	struct ttm_resource *resource = tbo->resource;

	drm_printf_indent(p, indent, "placement=");
	drm_print_bits(p, resource->placement, plname, ARRAY_SIZE(plname));
	drm_printf(p, "\n");

	if (resource->bus.is_iomem)
		drm_printf_indent(p, indent, "bus.offset=%lx\n",
				  (unsigned long)resource->bus.offset);
}

static const struct drm_gem_object_funcs lsdc_gem_object_funcs = {
	.free   = lsdc_bo_free,
	.pin    = lsdc_bo_pin,
	.unpin  = lsdc_bo_unpin,
	.vmap   = lsdc_bo_vmap,
	.vunmap = lsdc_bo_vunmap,
	.mmap   = lsdc_ttm_mmap,
	.print_info = lsdc_ttm_print_info,
};

/*
 * Creates a VRAM-backed GEM object
 * @dev:		the DRM device
 * @size:		the buffer size in bytes
 * @pg_align:		the buffer's alignment in multiples of the page size
 *
 * GEM objects are allocated by calling struct drm_driver.gem_create_object,
 * if set. Otherwise kzalloc() will be used. Drivers can set their own GEM
 * object functions in struct drm_driver.gem_create_object. If no functions
 * are set, the new GEM object will use the default functions from GEM VRAM
 * helpers.
 *
 * Returns:
 * A new instance of &struct lsdc_bo on success, or
 * an ERR_PTR()-encoded error code otherwise.
 */
static struct lsdc_bo *
lsdc_gem_vram_create(struct drm_device *ddev,
		     size_t size,
		     unsigned long pg_align)
{
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct ttm_device *bdev = &ldev->bdev;
	struct lsdc_bo *lbo;
	struct ttm_buffer_object *tbo;
	struct drm_gem_object *gem;
	int ret;

	if (ddev->driver->gem_create_object) {
		gem = ddev->driver->gem_create_object(ddev, size);
		if (IS_ERR(gem))
			return ERR_CAST(gem);
		tbo = to_ttm_bo(gem);
		lbo = to_lsdc_bo(tbo);
	} else {
		lbo = kzalloc(sizeof(struct lsdc_bo), GFP_KERNEL);
		if (!lbo)
			return ERR_PTR(-ENOMEM);

		tbo = &lbo->bo;
		gem = &tbo->base;
	}

	if (!gem->funcs) {
		drm_info(ddev, "%s: hook gem_vram_object_funcs\n", __func__);
		gem->funcs = &lsdc_gem_object_funcs;
	}

	ret = drm_gem_object_init(ddev, gem, size);
	if (ret) {
		kfree(lbo);
		return ERR_PTR(ret);
	}

	tbo->bdev = bdev;
	lsdc_gem_vram_placement(lbo, DRM_GEM_VRAM_PL_FLAG_SYSTEM);

	/*
	 * A failing ttm_bo_init will call ttm_buffer_object_destroy
	 * to release gbo->bo.base and kfree gbo.
	 */
	ret = ttm_bo_init_validate(bdev,
				   tbo,
				   ttm_bo_type_device,
				   &lbo->placement,
				   pg_align,
				   false, NULL, NULL,
				   ttm_buffer_object_destroy);
	if (ret)
		return ERR_PTR(ret);

	return lbo;
}

/*
 * Helper for implementing dumb create
 * @file: the DRM file
 * @dev: the DRM device
 * @args: the arguments as provided to &struct drm_driver.dumb_create
 *
 * This helper function fills &struct drm_mode_create_dumb, which is used
 * by &struct drm_driver.dumb_create. Implementations of this interface
 * should forwards their arguments to this helper, plus the driver-specific
 * parameters.
 *
 * Returns:
 * 0 on success, or
 * a negative error code otherwise.
 */
int lsdc_gem_dumb_create(struct drm_file *file,
			 struct drm_device *ddev,
			 struct drm_mode_create_dumb *args)
{
	struct lsdc_device *ldev = to_lsdc(ddev);
	const struct lsdc_desc *descp = ldev->descp;
	/* the buffer's alignment in multiples of the page size */
	unsigned long pg_align = 0;
	/* the scanline's alignment in powers of 2 */
	unsigned long pitch_align = descp->pitch_align;
	size_t pitch, size;
	struct lsdc_bo *gbo;
	int ret;
	u32 handle;

	pitch = args->width * DIV_ROUND_UP(args->bpp, 8);
	if (pitch_align) {
		if (WARN_ON_ONCE(!is_power_of_2(pitch_align)))
			return -EINVAL;
		pitch = ALIGN(pitch, pitch_align);
	}
	size = pitch * args->height;

	size = roundup(size, PAGE_SIZE);
	if (!size)
		return -EINVAL;

	gbo = lsdc_gem_vram_create(ddev, size, pg_align);
	if (IS_ERR(gbo))
		return PTR_ERR(gbo);

	ret = drm_gem_handle_create(file, &gbo->bo.base, &handle);
	if (ret)
		goto err_drm_gem_object_put;

	drm_gem_object_put(&gbo->bo.base);

	drm_info(ddev, "stride: %lu, height: %u\n", pitch, args->height);

	args->pitch = pitch;
	args->size = size;
	args->handle = handle;

	return 0;

err_drm_gem_object_put:
	drm_gem_object_put(&gbo->bo.base);
	return ret;
}

static void lsdc_gem_vram_cleanup_fb(struct drm_plane *plane,
				     struct drm_plane_state *state,
				     unsigned int num_planes)
{
	struct drm_gem_object *obj;
	struct drm_framebuffer *fb = state->fb;

	while (num_planes) {
		--num_planes;
		obj = fb->obj[num_planes];
		if (!obj) {
			drm_err(plane->dev, "%s: don't have obj\n", __func__);
			continue;
		}
		lsdc_bo_unpin(obj);
	}
}

/*
 * @plane:	a DRM plane
 * @new_state:	the plane's new state
 *
 * During plane updates, this function sets the plane's fence and
 * pins the GEM VRAM objects of the plane's new framebuffer to VRAM.
 * Call drm_gem_vram_plane_helper_cleanup_fb() to unpin them.
 *
 * Returns:
 *	0 on success, or
 *	a negative errno code otherwise.
 */
int lsdc_plane_prepare_fb(struct drm_plane *plane,
			  struct drm_plane_state *new_state)
{
	struct drm_framebuffer *fb = new_state->fb;
	unsigned int i;
	int ret;

	if (!fb)
		return 0;

	for (i = 0; i < fb->format->num_planes; ++i) {
		struct ttm_buffer_object *tbo;
		struct lsdc_bo *lbo;
		struct drm_gem_object *obj;

		obj = fb->obj[i];
		if (!obj) {
			ret = -EINVAL;
			drm_err(plane->dev, "%s: don't have obj\n", __func__);
			goto err_ret;
		}
		tbo = to_ttm_bo(obj);
		lbo = to_lsdc_bo(tbo);

		ret = ttm_bo_reserve(tbo, true, false, NULL);
		if (ret)
			goto err_ret;

		ret = lsdc_bo_validate(lbo, DRM_GEM_VRAM_PL_FLAG_VRAM);
		ttm_bo_unreserve(&lbo->bo);

		if (ret)
			goto err_ret;
	}

	ret = drm_gem_plane_helper_prepare_fb(plane, new_state);
	if (ret)
		goto err_ret;

	return 0;

err_ret:
	drm_info(plane->dev, "%s: error: %d\n", __func__, ret);
	lsdc_gem_vram_cleanup_fb(plane, new_state, i);
	return ret;
}

/*
 * lsdc_gem_vram_plane_helper_cleanup_fb()
 * @plane:	a DRM plane
 * @old_state:	the plane's old state
 *
 * During plane updates, this function unpins the GEM VRAM
 * objects of the plane's old framebuffer from VRAM. Complements
 * drm_gem_vram_plane_helper_prepare_fb().
 */
void lsdc_plane_cleanup_fb(struct drm_plane *plane,
			   struct drm_plane_state *old_state)
{
	struct drm_framebuffer *fb = old_state->fb;

	if (!fb)
		return;

	lsdc_gem_vram_cleanup_fb(plane, old_state, fb->format->num_planes);
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

	drm_info(ddev, "number of pages: %lu\n", num_pages);

	return drmm_add_action_or_reset(ddev, lsdc_ttm_fini, ldev);
}


/*
 * Returns a gem buffer object's offset in video memory
 *
 * This function returns the buffer object's offset in the device's
 * video memory. The buffer object has to be pinned to %TTM_PL_VRAM.
 *
 * Returns:
 * The buffer object's offset in video memory on success, or
 * a negative errno code otherwise.
 */
s64 lsdc_get_vram_bo_offset(struct drm_framebuffer *fb)
{
	struct ttm_buffer_object *tbo = to_ttm_bo(fb->obj[0]);
	struct ttm_resource *resource = tbo->resource;

	if (WARN_ON_ONCE(!tbo->pin_count))
		return (s64)-ENODEV;

	/* Keep TTM behavior for now, remove when drivers are audited */
	if (WARN_ON_ONCE(!resource))
		return 0;

	if (WARN_ON_ONCE(resource->mem_type == TTM_PL_SYSTEM))
		return 0;

	return resource->start << PAGE_SHIFT;
}

/*
 * Tests if a display mode's framebuffer fits into the available video memory.
 * @ddev: the DRM device
 * @mode: the mode to test
 *
 * This function tests if enough video memory is available for using the
 * specified display mode. Atomic modesetting requires importing the
 * designated framebuffer into video memory before evicting the active
 * one. Hence, any framebuffer may consume at most half of the available
 * VRAM. Display modes that require a larger framebuffer can not be used,
 * even if the CRTC does support them. Each framebuffer is assumed to
 * have 32-bit color depth.
 *
 * Returns:
 * MODE_OK if the display mode is supported, or an error code of type
 * enum drm_mode_status otherwise.
 */
enum drm_mode_status
lsdc_bo_mode_valid(struct drm_device *ddev,
		   const struct drm_display_mode *mode)
{
	struct lsdc_device *ldev = to_lsdc(ddev);
	const unsigned long max_bpp = 4; /* DRM_FORMAT_XRGB8888 */
	unsigned long fbsize, fbpages, max_fbpages;

	max_fbpages = (ldev->vram_size / 2) >> PAGE_SHIFT;
	fbsize = mode->hdisplay * mode->vdisplay * max_bpp;
	fbpages = DIV_ROUND_UP(fbsize, PAGE_SIZE);

	if (fbpages > max_fbpages)
		return MODE_MEM;

	return MODE_OK;
}

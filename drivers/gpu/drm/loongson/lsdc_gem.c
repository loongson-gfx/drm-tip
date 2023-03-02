// SPDX-License-Identifier: GPL-2.0+

#include <linux/dma-buf.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>

#include "lsdc_drv.h"
#include "lsdc_gem.h"
#include "lsdc_ttm.h"

static int lsdc_gem_prime_pin(struct drm_gem_object *obj)
{
	struct lsdc_bo *lbo = gem_to_lsdc_bo(obj);
	int ret;

	/* Pin the buffer into GTT */
	lsdc_bo_set_placement(lbo, LSDC_GEM_DOMAIN_GTT, 0);

	ret = lsdc_bo_pin(obj);
	if (likely(ret == 0))
		lbo->prime_shared_count++;

#if 1
	drm_info(obj->dev, "prime pin: count: %u\n", lbo->prime_shared_count);
#endif

	return ret;
}

static void lsdc_gem_prime_unpin(struct drm_gem_object *obj)
{
	struct lsdc_bo *lbo = gem_to_lsdc_bo(obj);

	lsdc_bo_unpin(obj);
	if (lbo->prime_shared_count)
		lbo->prime_shared_count--;

#if 1
	drm_info(obj->dev, "prime unpin: count: %u\n", lbo->prime_shared_count);
#endif
}

static struct sg_table *
lsdc_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct drm_device *ddev = obj->dev;
	struct ttm_buffer_object *tbo = to_ttm_bo(obj);

#if 1
	drm_info(ddev, "get sg table\n");
#endif

	return drm_prime_pages_to_sg(ddev, tbo->ttm->pages, tbo->ttm->num_pages);
}

static void lsdc_gem_object_free(struct drm_gem_object *gem)
{
	struct ttm_buffer_object *tbo = to_ttm_bo(gem);

	ttm_bo_put(tbo);
}

static int lsdc_gem_object_vmap(struct drm_gem_object *gem, struct iosys_map *map)
{
	struct drm_device *ddev = gem->dev;
	struct ttm_buffer_object *tbo = to_ttm_bo(gem);
	struct lsdc_bo *lbo = to_lsdc_bo(tbo);
	int ret;

	dma_resv_assert_held(gem->resv);

	if (tbo->pin_count == 0) {
		struct ttm_operation_ctx ctx = { false, false };

		ret = ttm_bo_validate(tbo, &lbo->placement, &ctx);
		if (ret < 0)
			return ret;
	}

	ttm_bo_pin(tbo);

	if (lbo->vmap_use_count > 0) {
#if 1
		drm_info(ddev, "already mapped\n");
#endif
		goto finish;
	}

	/* Only vmap if the there's no mapping present */
	if (iosys_map_is_null(&lbo->map)) {
		ret = ttm_bo_vmap(tbo, &lbo->map);
		if (ret) {
			ttm_bo_unpin(tbo);
			return ret;
		}
	}

finish:
	++lbo->vmap_use_count;
	*map = lbo->map;

	return 0;
}

static void lsdc_gem_object_vunmap(struct drm_gem_object *gem, struct iosys_map *map)
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

	/* We delay the actual unmap operation until the BO gets evicted */
	ttm_bo_unpin(tbo);
}

static int lsdc_gem_object_mmap(struct drm_gem_object *gem,
				struct vm_area_struct *vma)
{
	struct ttm_buffer_object *tbo = to_ttm_bo(gem);
	int ret;

	ret = ttm_bo_mmap_obj(vma, tbo);
	if (ret < 0)
		return ret;

#if 1
	drm_info(gem->dev, "bo mmap\n");
#endif

	drm_gem_object_put(gem);

	return 0;
}

const struct drm_gem_object_funcs lsdc_gem_object_funcs = {
	.free = lsdc_gem_object_free,
	.export = drm_gem_prime_export,
	.pin = lsdc_gem_prime_pin,
	.unpin = lsdc_gem_prime_unpin,
	.get_sg_table = lsdc_gem_prime_get_sg_table,
	.vmap = lsdc_gem_object_vmap,
	.vunmap = lsdc_gem_object_vunmap,
	.mmap = lsdc_gem_object_mmap,
};

static struct drm_gem_object *
lsdc_gem_object_create(struct drm_device *ddev, u32 domain, u32 flags,
		       size_t size, struct sg_table *sg, struct dma_resv *resv)
{
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct drm_gem_object *gobj;
	struct lsdc_bo *lbo;
	int ret;

	lbo = lsdc_bo_create(ddev, domain, flags, size, NULL, NULL);
	if (IS_ERR(lbo)) {
		ret = PTR_ERR(lbo);
		return ERR_PTR(ret);
	}

	gobj = &lbo->tbo.base;
	gobj->funcs = &lsdc_gem_object_funcs;

	/* tracking the BOs we created */
	mutex_lock(&ldev->gem.mutex);
	list_add_tail(&lbo->list, &ldev->gem.objects);
	mutex_unlock(&ldev->gem.mutex);

	return gobj;
}

struct drm_gem_object *
lsdc_prime_import_sg_table(struct drm_device *ddev,
			   struct dma_buf_attachment *attach,
			   struct sg_table *sg)
{
	struct dma_resv *resv = attach->dmabuf->resv;
	u64 size = attach->dmabuf->size;
	struct drm_gem_object *gobj;
	struct lsdc_bo *lbo;

	dma_resv_lock(resv, NULL);

	gobj = lsdc_gem_object_create(ddev, LSDC_GEM_DOMAIN_GTT, 0, size, sg, resv);
	if (IS_ERR(gobj)) {
		dma_resv_unlock(resv);
		return gobj;
	}

	lbo = gem_to_lsdc_bo(gobj);
	lbo->prime_shared_count = 1;

#if 1
	drm_info(ddev, "dmabuf size: %llx\n", size);
#endif

	dma_resv_unlock(resv);

	return gobj;
}

int lsdc_dumb_create(struct drm_file *file,
		     struct drm_device *ddev,
		     struct drm_mode_create_dumb *args)
{
	struct lsdc_device *ldev = to_lsdc(ddev);
	const struct lsdc_desc *descp = ldev->descp;
	struct drm_gem_object *gobj;
	size_t pitch, size;
	u32 handle;
	int ret;

	pitch = args->width * DIV_ROUND_UP(args->bpp, 8);
	pitch = ALIGN(pitch, descp->pitch_align);
	size = pitch * args->height;
	size = roundup(size, PAGE_SIZE);
	if (!size)
		return -EINVAL;

	gobj = lsdc_gem_object_create(ddev,
				      LSDC_GEM_DOMAIN_VRAM,
				      TTM_PL_FLAG_CONTIGUOUS,
				      size, NULL, NULL);
	if (IS_ERR(gobj))
		return PTR_ERR(gobj);

	ret = drm_gem_handle_create(file, gobj, &handle);
	/* drop reference from allocate, handle holds it now */
	drm_gem_object_put(gobj);
	if (ret)
		return ret;

	args->pitch = pitch;
	args->size = size;
	args->handle = handle;

	return 0;
}

int lsdc_dumb_map_offset(struct drm_file *filp,
			 struct drm_device *ddev,
			 u32 handle,
			 uint64_t *offset)
{
	struct drm_gem_object *gobj;

	gobj = drm_gem_object_lookup(filp, handle);
	if (!gobj)
		return -ENOENT;

	*offset = drm_vma_node_offset_addr(&gobj->vma_node);

	drm_gem_object_put(gobj);

	return 0;
}


#if defined(CONFIG_DEBUG_FS)

static const char *lsdc_domain_to_str(uint32_t mem_type)
{
	switch (mem_type) {
	case TTM_PL_VRAM:
		return "VRAM";
	case TTM_PL_TT:
		return "GTT";
	case TTM_PL_SYSTEM:
		return "SYSTEM";
	default:
		break;
	}

	return "Unknown";
}

static int lsdc_debugfs_gem_info_show(struct seq_file *m, void *unused)
{
	struct lsdc_device *ldev = (struct lsdc_device *)m->private;
	struct lsdc_bo *lbo;
	unsigned int i = 0;

	if (ldev == NULL) {
		seq_printf(m, "ldev is null\n");
		return 0;
	}

	mutex_lock(&ldev->gem.mutex);

	list_for_each_entry(lbo, &ldev->gem.objects, list) {
		struct ttm_buffer_object *tbo = &lbo->tbo;
		struct ttm_resource *resource;
		uint32_t mem_type = 0;

		resource = tbo->resource;
		if (resource)
			mem_type = resource->mem_type;

		seq_printf(m, "bo[0x%08x] size: %8ldkB domain: %s\n",
			   i,
			   lsdc_bo_size(lbo) >> 10,
			   mem_type ? lsdc_domain_to_str(mem_type) : "NULL");
		i++;
	}

	mutex_unlock(&ldev->gem.mutex);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(lsdc_debugfs_gem_info);
#endif

void lsdc_gem_debugfs_init(struct drm_minor *primary)
{
#if defined(CONFIG_DEBUG_FS)
	struct drm_device *ddev = primary->dev;
	struct lsdc_device *ldev = to_lsdc(ddev);
	struct dentry *root = primary->debugfs_root;

	debugfs_create_file("lsdc_gem_info", 0444, root, ldev,
			    &lsdc_debugfs_gem_info_fops);
#endif
}

void lsdc_gem_init(struct drm_device *ddev)
{
	struct lsdc_device *ldev = to_lsdc(ddev);

	mutex_init(&ldev->gem.mutex);
	INIT_LIST_HEAD(&ldev->gem.objects);
}

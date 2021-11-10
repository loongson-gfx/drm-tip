// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2018 Etnaviv Project
 */

#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <drm/drm_drv.h>

#include "etnaviv_drv.h"
#include "etnaviv_gpu.h"
#include "etnaviv_pci_drv.h"

int etnaviv_cached_coherent = -1;
MODULE_PARM_DESC(cached_coherent, "using cached coherent(0 = disabled)");
module_param_named(cached_coherent, etnaviv_cached_coherent, int, 0644);

static int etnaviv_alloc_gpu(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct device *dev = &pdev->dev;
	struct etnaviv_gpu *gpu;
	int err;

	gpu = devm_kzalloc(dev, sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return -ENOMEM;

	gpu->dev = dev;
	mutex_init(&gpu->lock);
	mutex_init(&gpu->fence_lock);

	if (ent->driver_data == GC1000_IN_LS7A1000 ||
	    ent->driver_data == GC1000_IN_LS2K1000) {
		/* gpu bar 0 contain registers */
		gpu->mmio = devm_ioremap_resource(dev, &pdev->resource[0]);
		if (IS_ERR(gpu->mmio))
			return PTR_ERR(gpu->mmio);
	} else {
		dev_err(&pdev->dev, "unknown GPU model\n");
		return -ENOENT;
	}

	/* Get Interrupt: */
	err = etnaviv_gpu_register_irq(gpu, pdev->irq);
	if (err)
		return err;

	/* Get Clocks: */
	etnaviv_gpu_get_clock(gpu, dev);

	dev_set_drvdata(dev, gpu);

	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 200);
	pm_runtime_enable(dev);

	return 0;
}

static void etnaviv_free_gpu(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev;
	struct etnaviv_gpu *gpu = dev_get_drvdata(dev);

	pm_runtime_disable(dev);

	if (gpu)
		devm_kfree(dev, gpu);

	dev_set_drvdata(dev, NULL);
}

static int etnaviv_create_private(struct drm_device *ddev)
{
	struct etnaviv_drm_private *priv;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ddev->dev_private = priv;

	mutex_init(&priv->gem_lock);
	INIT_LIST_HEAD(&priv->gem_list);

	priv->num_gpus = 0;
	priv->shm_gfp_mask = GFP_HIGHUSER | __GFP_RETRY_MAYFAIL | __GFP_NOWARN;

	priv->cmdbuf_suballoc = etnaviv_cmdbuf_suballoc_new(ddev->dev);
	if (IS_ERR(priv->cmdbuf_suballoc)) {
		dev_err(ddev->dev, "Failed to create cmdbuf suballocator\n");
		ret = PTR_ERR(priv->cmdbuf_suballoc);
		goto out_free_priv;
	}

	dev_info(ddev->dev, "etnaviv drm private created\n");

	return 0;

out_free_priv:
	ddev->dev_private = NULL;
	kfree(priv);

	return ret;
}

static void etnaviv_destroy_private(struct drm_device *ddev)
{
	struct etnaviv_drm_private *priv = ddev->dev_private;

	etnaviv_cmdbuf_suballoc_destroy(priv->cmdbuf_suballoc);

	ddev->dev_private = NULL;
	kfree(priv);

	dev_info(ddev->dev, "etnaviv drm private freed\n");
}

static int etnaviv_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int ret;
	struct drm_device *ddev;
	struct etnaviv_drm_private *priv;

	ret = pcim_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable\n");
		return ret;
	}

	pci_set_master(pdev);

	/*
	 * Instantiate the DRM device.
	 */
	ddev = drm_dev_alloc(&etnaviv_drm_driver, &pdev->dev);
	if (IS_ERR(ddev))
		return PTR_ERR(ddev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto out_put;

	ret = dma_set_max_seg_size(&pdev->dev, SZ_2G);
	if (ret)
		goto out_put;

	ret = etnaviv_create_private(ddev);
	if (ret)
		goto out_put;

	priv = ddev->dev_private;

	if (etnaviv_cached_coherent) {
		/*
		 * Loongson Mips CPU's cache coherency is maintained by
		 * hardware, including ls3a4000, ls3a3000, ls2k1000 etc.
		 */
		if (IS_ENABLED(CONFIG_CPU_LOONGSON64))
			priv->has_cached_coherent = true;

		/*
		 * Loongson LoongArch CPU's cache coherency is also maintained
		 * by hardware, including ls3a5000, ls3c5000, ls2k1000la etc.
		 */
		if (IS_ENABLED(CONFIG_LOONGARCH))
			priv->has_cached_coherent = true;
	}

	dev_info(&pdev->dev, "cached coherent is %s\n",
		 priv->has_cached_coherent ? "enabled" : "disabled");

	ret = etnaviv_alloc_gpu(pdev, ent);
	if (ret)
		goto out_destroy_private;

	ret = etnaviv_gpu_bind(&pdev->dev, NULL, ddev);
	if (ret)
		goto out_free_gpu;

	ret = etnaviv_gpu_init(priv->gpu[0]);
	if (ret)
		goto out_unbind_gpu;

	dev_info(&pdev->dev, "GPU Initialized\n");

	ret = drm_dev_register(ddev, 0);
	if (ret)
		goto out_unbind_gpu;

	return 0;

out_unbind_gpu:
	etnaviv_gpu_unbind(&pdev->dev, NULL, ddev);
out_free_gpu:
	etnaviv_free_gpu(pdev);
out_destroy_private:
	etnaviv_destroy_private(ddev);
out_put:
	drm_dev_put(ddev);

	return ret;
}

static void etnaviv_pci_remove(struct pci_dev *pdev)
{
	struct etnaviv_gpu *gpu = dev_get_drvdata(&pdev->dev);
	struct drm_device *ddev = gpu->drm;

	drm_dev_unregister(ddev);

	etnaviv_gpu_unbind(&pdev->dev, NULL, ddev);

	etnaviv_free_gpu(pdev);

	etnaviv_destroy_private(ddev);

	drm_put_dev(ddev);
}

static const struct pci_device_id etnaviv_pci_id_lists[] = {
	{0x0014, 0x7a15, PCI_ANY_ID, PCI_ANY_ID, 0, 0, GC1000_IN_LS7A1000},
	{0x0014, 0x7a05, PCI_ANY_ID, PCI_ANY_ID, 0, 0, GC1000_IN_LS2K1000},
	{0, 0, 0, 0, 0, 0, 0}
};

struct pci_driver etnaviv_pci_driver = {
	.name = "etnaviv",
	.id_table = etnaviv_pci_id_lists,
	.probe = etnaviv_pci_probe,
	.remove = etnaviv_pci_remove,
	.driver = {
		.name   = "etnaviv",
	},
};

MODULE_DEVICE_TABLE(pci, etnaviv_pci_id_lists);

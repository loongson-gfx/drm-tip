/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ETNAVIV_PCI_DRV_H__
#define __ETNAVIV_PCI_DRV_H__

#include <linux/pci.h>

enum etnaviv_pci_gpu_family {
	GC1000_IN_LS7A1000 = 0,
	GC1000_IN_LS2K1000 = 1,
	CHIP_LAST,
};

extern struct pci_driver etnaviv_pci_driver;
#endif

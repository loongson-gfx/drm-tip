// SPDX-License-Identifier: GPL-2.0+

#include "lsdc_drv.h"

/*
 * Processor ID (implementation) values for bits 15:8 of the PRID register.
 */
#define LOONGSON_CPU_IMP_MASK           0xff00
#define LOONGSON_CPU_IMP_SHIFT          8

#define LOONGARCH_CPU_IMP_LS2K1000      0xa0
#define LOONGARCH_CPU_IMP_LS2K2000      0xb0
#define LOONGARCH_CPU_IMP_LS3A5000      0xc0

#define LOONGSON_CPU_MIPS_IMP_LS2K      0x61 /* Loongson 2K Mips series SoC */

/*
 * Particular Revision values for bits 7:0 of the PRID register.
 */
#define LOONGSON_CPU_REV_MASK           0x00ff

#define LOONGARCH_CPUCFG_PRID_REG       0x0

unsigned int loongson_cpu_get_prid(u8 *imp, u8 *rev)
{
	unsigned int prid = 0;

#if defined(__loongarch__)
	__asm__ volatile("cpucfg %0, %1\n\t"
			: "=&r"(prid)
			: "r"(LOONGARCH_CPUCFG_PRID_REG)
			);
#endif

#if defined(__mips__)
	__asm__ volatile("mfc0\t%0, $15\n\t"
			: "=r" (prid)
			);
#endif

	if (imp)
		*imp = (prid & LOONGSON_CPU_IMP_MASK) >> LOONGSON_CPU_IMP_SHIFT;

	if (rev)
		*rev = prid & LOONGSON_CPU_REV_MASK;

	return prid;
}

/* LS2K2000 has only LoongArch edition (LA364) */
bool lsdc_is_ls2k2000(void)
{
	u8 imp;

	loongson_cpu_get_prid(&imp, NULL);

	if (imp == LOONGARCH_CPU_IMP_LS2K2000)
		return true;

	return false;
}

bool lsdc_is_ls2k1000(void)
{
	u8 imp;

	loongson_cpu_get_prid(&imp, NULL);

#if defined(__mips__)
	/* LS2K1000 has Mips edition(mips64r2) */
	if (imp == LOONGSON_CPU_MIPS_IMP_LS2K)
		return true;
#endif

#if defined(__loongarch__)
	/* LS2K1000 has loongarch edition(LA264)
	 * CPU core and instruction set changed, remains are basically same
	 */
	if (imp == LOONGARCH_CPU_IMP_LS2K1000)
		return true;
#endif

	return false;
}

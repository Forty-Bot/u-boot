// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018, Bin Meng <bmeng.cn@gmail.com>
 */

#include <common.h>
#include <dm.h>
#include <fdtdec.h>
#include <init.h>
#include <log.h>
#include <ram.h>
#include <linux/sizes.h>

DECLARE_GLOBAL_DATA_PTR;

int dram_init(void)
{
#if CONFIG_IS_ENABLED(RAM)
	int ret;
	struct ram_info info;
	struct udevice *dev;

	ret = uclass_get_device(UCLASS_RAM, 0, &dev);
	if (ret) {
		debug("DRAM init failed: %d\n", ret);
		return ret;
	}

	ret = ram_get_info(dev, &info);
	if (ret) {
		debug("Cannot get DRAM size: %d\n", ret);
		return ret;
	}

	gd->ram_base = info.base;
	gd->ram_size = info.size;

	return 0;
#else
	return fdtdec_setup_mem_size_base();
#endif
}

int dram_init_banksize(void)
{
	return fdtdec_setup_memory_banksize();
}

ulong board_get_usable_ram_top(ulong total_size)
{
#ifdef CONFIG_64BIT
	/*
	 * Ensure that we run from first 4GB so that all
	 * addresses used by U-Boot are 32bit addresses.
	 *
	 * This in-turn ensures that 32bit DMA capable
	 * devices work fine because DMA mapping APIs will
	 * provide 32bit DMA addresses only.
	 */
	if (gd->ram_top > SZ_4G)
		return SZ_4G;
#endif
	return gd->ram_top;
}

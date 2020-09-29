// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2020 Sean Anderson <seanga2@gmail.com>
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <ram.h>

static int k210_sram_probe(struct udevice *dev)
{
	int ret;
	struct clk_bulk clocks;

	/* Relocate as high as possible to leave more space to load payloads */
	ret = fdtdec_setup_mem_size_base_highest();
	if (ret)
		return ret;

	/* Enable ram bank clocks */
	ret = clk_get_bulk(dev, &clocks);
	if (ret)
		return ret;

	ret = clk_enable_bulk(&clocks);
	if (ret)
		return ret;

	return 0;
}

static int k210_sram_get_info(struct udevice *dev, struct ram_info *info)
{
	info->base = gd->ram_base;
	info->size = gd->ram_size;

	return 0;
}

static struct ram_ops k210_sram_ops = {
	.get_info = k210_sram_get_info,
};

static const struct udevice_id k210_sram_ids[] = {
	{ .compatible = "kendryte,k210-sram" },
	{ }
};

U_BOOT_DRIVER(fu540_ddr) = {
	.name = "k210_sram",
	.id = UCLASS_RAM,
	.of_match = k210_sram_ids,
	.ops = &k210_sram_ops,
	.probe = k210_sram_probe,
};

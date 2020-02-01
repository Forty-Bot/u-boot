// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2014 Google, Inc
 */

#include <common.h>
#include <dm.h>
#include <clk.h>

#define SIMPLE_BUS 0
#define SIMPLE_MFD 1
#define SIMPLE_PM_BUS 2

struct simple_bus_plat {
	u32 base;
	u32 size;
	u32 target;
};

fdt_addr_t simple_bus_translate(struct udevice *dev, fdt_addr_t addr)
{
	struct simple_bus_plat *plat = dev_get_uclass_platdata(dev);

	if (addr >= plat->base && addr < plat->base + plat->size)
		addr = (addr - plat->base) + plat->target;

	return addr;
}

static int simple_bus_post_bind(struct udevice *dev)
{
#if CONFIG_IS_ENABLED(OF_PLATDATA)
	return 0;
#else
	u32 cell[3];
	int ret;

	ret = dev_read_u32_array(dev, "ranges", cell, ARRAY_SIZE(cell));
	if (!ret) {
		struct simple_bus_plat *plat = dev_get_uclass_platdata(dev);

		plat->base = cell[0];
		plat->target = cell[1];
		plat->size = cell[2];
	}

	return dm_scan_fdt_dev(dev);
#endif
}

UCLASS_DRIVER(simple_bus) = {
	.id		= UCLASS_SIMPLE_BUS,
	.name		= "simple_bus",
	.post_bind	= simple_bus_post_bind,
	.per_device_platdata_auto_alloc_size = sizeof(struct simple_bus_plat),
};

static const int generic_simple_bus_probe(struct udevice *dev)
{
#if CONFIG_IS_ENABLED(CLK)
	int ret;
	struct clk_bulk *bulk;
	ulong type = dev_get_driver_data(dev);

	if (type == SIMPLE_PM_BUS) {
		bulk = kzalloc(sizeof(*bulk), GFP_KERNEL);
		if (!bulk)
			return -ENOMEM;

		ret = clk_get_bulk(dev, bulk);
		if (ret)
			return ret;
		
		ret = clk_enable_bulk(bulk);
		if (ret && ret != -ENOSYS && ret != -ENOTSUPP) {
			clk_release_bulk(bulk);
			return ret;
		}
		dev->priv = bulk;
	}
#endif
	return 0;
}

static const int generic_simple_bus_remove(struct udevice *dev)
{
	int ret = 0;
#if CONFIG_IS_ENABLED(CLK)
	struct clk_bulk *bulk;
	ulong type = dev_get_driver_data(dev);

	if (type == SIMPLE_PM_BUS) {
		bulk = dev_get_priv(dev);
		ret = clk_release_bulk(bulk);
		kfree(bulk);
		if (ret == -ENOSYS)
			ret = 0;
	}
#endif
	return ret;
}

static const struct udevice_id generic_simple_bus_ids[] = {
	{ .compatible = "simple-bus", .data = SIMPLE_BUS },
	{ .compatible = "simple-mfd", .data = SIMPLE_MFD },
	{ .compatible = "simple-pm-bus", .data = SIMPLE_PM_BUS },
	{ }
};

U_BOOT_DRIVER(simple_bus_drv) = {
	.name	= "generic_simple_bus",
	.id	= UCLASS_SIMPLE_BUS,
	.of_match = generic_simple_bus_ids,
	.probe = generic_simple_bus_probe,
	.remove = generic_simple_bus_remove,
	.flags	= DM_FLAG_PRE_RELOC,
};

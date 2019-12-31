// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Sean Anderson <seanga2@gmail.com>
 */
#include <asm/k210_sysctl.h>

#include <dm.h>

static const struct udevice_id k210_sysctl_ids[] = {
	{ .compatible = "kendryte,k210-sysctl", },
	{ }
};

U_BOOT_DRIVER(k210_sysctl) = {
	.name = "k210_sysctl",
	.id = UCLASS_SYSCON,
#if !CONFIG_IS_ENABLED(OF_PLATDATA)
	.bind = dm_scan_fdt_dev,
#endif
	.of_match = k210_sysctl_ids,
	.flags = DM_FLAG_PRE_RELOC,
};

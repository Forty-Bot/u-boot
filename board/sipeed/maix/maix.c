// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Sean Anderson <seanga2@gmail.com>
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <fdt_support.h>

phys_size_t get_effective_memsize(void)
{
	return CONFIG_SYS_SDRAM_SIZE;
}

int board_init(void)
{
	int ret;
	ofnode bank = ofnode_null();

	/* Enable RAM clocks */
	while (true) {
		struct clk clk;

		bank = ofnode_by_prop_value(bank, "device_type", "memory",
					    sizeof("memory"));
		if (ofnode_equal(bank, ofnode_null()))
			break;

		ret = clk_get_by_index_nodev(bank, 0, &clk);
		if (ret)
			continue;

		ret = clk_enable(&clk);
		clk_free(&clk);
		if (ret)
			return ret;
	}
	return 0;
}

int ft_board_setup(void *blob, bd_t *bd)
{
	int i;
	u64 base[CONFIG_NR_DRAM_BANKS];
	u64 size[CONFIG_NR_DRAM_BANKS];

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		base[i] = bd->bi_dram[i].start;
		size[i] = bd->bi_dram[i].size;
	}

	return fdt_fixup_memory_banks(blob, base, size, CONFIG_NR_DRAM_BANKS);
}

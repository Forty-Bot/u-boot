/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2019 Sean Anderson <seanga2@gmail.com>
 */

#ifndef K210_SYSCTL_H
#define K210_SYSCTL_H

#include <linux/compiler.h>

/*
 * sysctl registers
 * Taken from kendryte-standalone-sdk/lib/drivers/include/sysctl.h
 */
struct k210_sysctl {
	u32 git_id;
	u32 clk_freq;
	u32 pll0;
	u32 pll1;
	u32 pll2;
	u32 resv5;
	u32 pll_lock;
	u32 rom_error;
	u32 clk_sel[2];
	u32 clk_en_cent;
	u32 clk_en_peri;
	u32 soft_reset;
	u32 peri_reset;
	u32 clk_thr[7];
	u32 misc;
	u32 peri;
	u32 spi_sleep;
	u32 reset_status;
	u32 dma_sel0;
	u32 dma_sel1;
	u32 power_sel;
	u32 resv28;
	u32 resv29;
	u32 resv30;
	u32 resv31;
} __packed;

#endif /* K210_SYSCTL_H */

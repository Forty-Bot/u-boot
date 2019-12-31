/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2019 Sean Anderson <seanga2@gmail.com>
 */

#ifndef CONFIGS_SIPEED_MAIX_H
#define CONFIGS_SIPEED_MAIX_H

#include <linux/sizes.h>

#define CONFIG_SYS_LOAD_ADDR 0x80000000
#define CONFIG_SYS_SDRAM_BASE CONFIG_SYS_LOAD_ADDR
#define CONFIG_SYS_SDRAM_SIZE SZ_8M
/* Start just below AI memory */
#define CONFIG_SYS_INIT_SP_ADDR 0x805FFFFF
#define CONFIG_SYS_MALLOC_LEN SZ_8K
#define CONFIG_SYS_CACHELINE_SIZE 64

#endif /* CONFIGS_SIPEED_MAIX_H */

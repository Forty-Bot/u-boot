/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2019-20 Sean Anderson <seanga2@gmail.com>
 */

#ifndef CONFIGS_SIPEED_MAIX_H
#define CONFIGS_SIPEED_MAIX_H

#include <linux/sizes.h>

#define CONFIG_SYS_LOAD_ADDR 0x80000000
/* Start just below the second bank */
#define CONFIG_SYS_INIT_SP_ADDR 0x803FFFFF
#define CONFIG_SYS_MALLOC_LEN SZ_8K
#define CONFIG_SYS_CACHELINE_SIZE 64

#endif /* CONFIGS_SIPEED_MAIX_H */

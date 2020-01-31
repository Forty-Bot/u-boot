// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2013 Altera Corporation <www.altera.com>
 */

#include <common.h>
#include <asm/io.h>
#include <clk.h>
#include <log2.h>
#include <watchdog.h>
#include <wdt.h>

#define DW_WDT_CR	0x00
#define DW_WDT_TORR	0x04
#define DW_WDT_CRR	0x0C

#define DW_WDT_CR_EN_OFFSET	0x00
#define DW_WDT_CR_RMOD_OFFSET	0x01
#define DW_WDT_CR_RMOD_VAL	0x00
#define DW_WDT_CRR_RESTART_VAL	0x76

struct designware_wdt_priv {
	void __iomem *base;
	ulong clock_khz;
};

/*
 * Set the watchdog time interval.
 * Counter is 32 bit.
 */
static int designware_wdt_settimeout(struct designware_wdt_priv *priv,
				     u64 timeout)
{
	signed int i;

	/* calculate the timeout range value */
	i = log_2_n_round_up(timeout * priv->clock_khz) - 16;
	if (i > 15)
		i = 15;
	if (i < 0)
		i = 0;

	writel(i | (i << 4), priv->base + DW_WDT_TORR);
	return 0;
}

static void designware_wdt_enable(struct designware_wdt_priv *priv)
{
	writel((DW_WDT_CR_RMOD_VAL << DW_WDT_CR_RMOD_OFFSET) |
	       (0x1 << DW_WDT_CR_EN_OFFSET), priv->base + DW_WDT_CR);
}

static unsigned int designware_wdt_is_enabled(struct designware_wdt_priv *priv)
{
	unsigned long val;
	val = readl(priv->base + DW_WDT_CR);
	return val & 1;
}

static void designware_wdt_reset(struct designware_wdt_priv *priv)
{
	if (designware_wdt_is_enabled(priv))
		writel(DW_WDT_CRR_RESTART_VAL, priv->base + DW_WDT_CRR);
}

static void designware_wdt_init(struct designware_wdt_priv *priv, u64 timeout)
{
	/* reset to disable the watchdog */
	designware_wdt_reset(priv);
	/* set timer in miliseconds */
	designware_wdt_settimeout(priv, timeout);
	designware_wdt_enable(priv);
	designware_wdt_reset(priv);
}

#ifdef CONFIG_HW_WATCHDOG
static struct designware_wdt_priv wdt_priv = {
	.base = CONFIG_DW_WDT_BASE,
};

void hw_watchdog_reset(void)
{
	/* XXX: may contain a function call; must be done at runtime */
	wdt_priv.clock_khz = CONFIG_DW_WDT_CLOCK_KHZ;
	designware_wdt_reset(&wdt_priv);
}

void hw_watchdog_init(void)
{
	/* XXX: see above */
	wdt_priv.clock_khz = CONFIG_DW_WDT_CLOCK_KHZ;
	designware_wdt_init(&wdt_priv, CONFIG_WATCHDOG_TIMEOUT_MSECS);
}
#endif

#ifdef CONFIG_WDT
static int dw_wdt_reset(struct udevice *dev)
{
	struct designware_wdt_priv *priv = dev_get_priv(dev);

	designware_wdt_reset(priv);

	return 0;
}

static int dw_wdt_start(struct udevice *dev, u64 timeout, ulong flags)
{
	struct designware_wdt_priv *priv = dev_get_priv(dev);

	designware_wdt_init(priv, timeout);

	return 0;
}

static int dw_wdt_probe(struct udevice *dev)
{
	int ret;
	ulong rate;
	struct clk clk;
	struct designware_wdt_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -ENOENT;

	ret = clk_get_by_index(dev, 0, &clk);
	if (ret)
		return ret;

	rate = clk_get_rate(&clk);
	if (IS_ERR_VALUE(rate)) {
		clk_free(&clk);
		return rate;
	}
	priv->clock_khz = rate / 1000;

	return 0;
}

static const struct wdt_ops dw_wdt_ops = {
	.start		= dw_wdt_start,
	.reset		= dw_wdt_reset,
};

static const struct udevice_id dw_wdt_ids[] = {
	{ .compatible = "snps,dw-wdt" },
	{ },
};

U_BOOT_DRIVER(designware_wdt) = {
	.name		= "designware_wdt",
	.id		= UCLASS_WDT,
	.of_match	= dw_wdt_ids,
	.probe		= dw_wdt_probe,
	.ops		= &dw_wdt_ops,
	.priv_auto_alloc_size = sizeof(struct designware_wdt_priv),
	.flags		= DM_FLAG_PRE_RELOC,
};
#endif /* WDT */

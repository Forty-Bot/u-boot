// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019-20 Sean Anderson <seanga2@gmail.com>
 */
#include <kendryte/clk.h>

#include <asm/io.h>
#include <dt-bindings/clock/k210-sysctl.h>
#include <dt-bindings/mfd/k210-sysctl.h>
#include <dm.h>
#include <log.h>
#include <mapmem.h>

#include <kendryte/bypass.h>
#include <kendryte/pll.h>

static ulong k210_clk_get_rate(struct clk *clk)
{
	struct clk *c;
	int err = clk_get_by_id(clk->id, &c);

	if (err)
		return err;
	return clk_get_rate(c);
}

static ulong k210_clk_set_rate(struct clk *clk, unsigned long rate)
{
	struct clk *c;
	int err = clk_get_by_id(clk->id, &c);

	if (err)
		return err;
	return clk_set_rate(c, rate);
}

static int k210_clk_set_parent(struct clk *clk, struct clk *parent)
{
	struct clk *c, *p;
	int err = clk_get_by_id(clk->id, &c);

	if (err)
		return err;

	err = clk_get_by_id(parent->id, &p);
	if (err)
		return err;

	return clk_set_parent(c, p);
}

static int k210_clk_endisable(struct clk *clk, bool enable)
{
	struct clk *c;
	int err = clk_get_by_id(clk->id, &c);

	if (err)
		return err;
	return enable ? clk_enable(c) : clk_disable(c);
}

static int k210_clk_enable(struct clk *clk)
{
	return k210_clk_endisable(clk, true);
}

static int k210_clk_disable(struct clk *clk)
{
	return k210_clk_endisable(clk, false);
}

static const struct clk_ops k210_clk_ops = {
	.set_rate = k210_clk_set_rate,
	.get_rate = k210_clk_get_rate,
	.set_parent = k210_clk_set_parent,
	.enable = k210_clk_enable,
	.disable = k210_clk_disable,
};

/* The first clock is in0, which is filled in by k210_clk_probe */
static const char * const generic_sels[] = { "in0_half", "pll0_half" };
static const char *pll2_sels[] = { NULL, "pll0", "pll1" };

static struct clk_divider *k210_clk_comp_div_flags(void __iomem *reg, u8 shift,
						   u8 width, u8 flags)
{
	struct clk_divider *div;

	div = kzalloc(sizeof(*div), GFP_KERNEL);
	if (!div)
		return div;
	div->reg = reg;
	div->shift = shift;
	div->width = width;
	div->flags = flags;
	return div;
}

static inline struct clk_divider *k210_clk_comp_div(void __iomem *reg, u8 shift,
						    u8 width)
{
	return k210_clk_comp_div_flags(reg, shift, width, 0);
}

static struct clk_gate *k210_clk_comp_gate(void __iomem *reg, u8 bit_idx)
{
	struct clk_gate *gate;

	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return gate;
	gate->reg = reg;
	gate->bit_idx = bit_idx;
	return gate;
}

static struct clk_mux *k210_clk_comp_mux(const char * const parent_names[],
					 u8 num_parents, void __iomem *reg,
					 u8 shift, u8 width)
{
	struct clk_mux *mux;

	mux = kzalloc(sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return mux;
	mux->reg = reg;
	mux->mask = BIT(width) - 1;
	mux->shift = shift;
	mux->parent_names = parent_names;
	mux->num_parents = num_parents;
	return mux;
}

static struct clk *k210_clk_comp_nomux(const char *name, const char *parent,
				       struct clk_divider *div,
				       struct clk_gate *gate)
{
	if (!div || !gate) {
		kfree(div);
		kfree(gate);
		return ERR_PTR(-ENOMEM);
	}
	return clk_register_composite(NULL, name, &parent, 1,
				      NULL, NULL,
				      &div->clk, &clk_divider_ops,
				      &gate->clk, &clk_gate_ops, 0);
}

static struct clk *k210_clk_comp(const char *name, struct clk_divider *div,
				 struct clk_gate *gate, struct clk_mux *mux)
{
	if (!div || !gate || !mux) {
		kfree(div);
		kfree(gate);
		kfree(mux);
		return ERR_PTR(-ENOMEM);
	}
	return clk_register_composite(NULL, name, generic_sels,
				      ARRAY_SIZE(generic_sels),
				      &mux->clk, &clk_mux_ops,
				      &div->clk, &clk_divider_ops,
				      &gate->clk, &clk_gate_ops, 0);
}

static int k210_clk_probe(struct udevice *dev)
{
	int err;
	const char *in0;
	struct clk **children;
	struct clk *aclk;
	struct clk *bypass;
	struct clk in0_clk;
	struct clk *in0_half;
	struct clk_divider *div;
	struct clk_gate *gate;
	struct clk_mux *mux;
	struct k210_pll *pll;
	void *base;

	base = dev_read_addr_ptr(dev_get_parent(dev));
	if (!base)
		return -EINVAL;

	err = clk_get_by_index(dev, 0, &in0_clk);
	if (err)
		goto cleanup_base;
	in0 = in0_clk.dev->name;
	pll2_sels[0] = in0;

	in0_half = k210_clk_half("in0_half", in0);

	/* PLLs */
	pll = k210_clk_comp_pll(base + K210_SYSCTL_PLL0,
				base + K210_SYSCTL_PLL_LOCK, 0, 2);
	/*
	 * All PLLs have a broken bypass, but pll0 has the CPU downstream, so we
	 * need to manually reparent it whenever we configure pll0
	 */
	bypass = k210_clk_bypass("pll0", in0, &pll->clk, &k210_pll_ops,
				 in0_half);
	clk_dm(K210_CLK_PLL0, bypass);
	clk_dm(K210_CLK_PLL1, k210_clk_pll("pll1", in0, base + K210_SYSCTL_PLL1,
					   base + K210_SYSCTL_PLL_LOCK, 8, 1));
	/* PLL2 is muxed, so set up a composite clock */
	mux = k210_clk_comp_mux(pll2_sels, ARRAY_SIZE(pll2_sels),
				base + K210_SYSCTL_PLL2, 26, 2);
	pll = k210_clk_comp_pll(base + K210_SYSCTL_PLL2,
				base + K210_SYSCTL_PLL_LOCK, 16, 1);
	if (!mux || !pll) {
		kfree(mux);
		kfree(pll);
	} else {
		clk_dm(K210_CLK_PLL2,
		       clk_register_composite(NULL, "pll2", pll2_sels,
					      ARRAY_SIZE(pll2_sels),
					      &mux->clk, &clk_mux_ops,
					      &pll->clk, &k210_pll_ops,
					      &pll->clk, &k210_pll_ops, 0));
	}

	/* Half-frequency clocks for "even" dividers */
	k210_clk_half("pll0_half", "pll0");
	k210_clk_half("pll2_half", "pll2");

	/* Muxed clocks */
	div = k210_clk_comp_div_flags(base + K210_SYSCTL_SEL0, 1, 2,
				      CLK_DIVIDER_POWER_OF_TWO);
	mux = k210_clk_comp_mux(generic_sels, ARRAY_SIZE(generic_sels),
				base + K210_SYSCTL_SEL0, 0, 1);
	/*
	 * aclk is the direct parent of the cpu clock, and needs to be
	 * reparented when pll0 is configured.
	 */
	children = kzalloc(sizeof(*children), GFP_KERNEL);
	if (!div || !mux || !children) {
		kfree(div);
		kfree(mux);
		kfree(children);
	} else {
		aclk = clk_register_composite(NULL, "aclk", generic_sels,
					      ARRAY_SIZE(generic_sels),
					      &mux->clk, &clk_mux_ops,
					      &div->clk, &clk_divider_ops,
					      NULL, NULL, 0);
		children[0] = aclk;
		err = k210_bypass_set_children(bypass, children, 1);
		if (!err)
			clk_dm(K210_CLK_ACLK, aclk);
	}

	div = k210_clk_comp_div(base + K210_SYSCTL_SEL0, 1, 2);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 9);
	mux = k210_clk_comp_mux(generic_sels, ARRAY_SIZE(generic_sels),
				base + K210_SYSCTL_SEL0, 12, 1);
	clk_dm(K210_CLK_SPI3, k210_clk_comp("spi3", div, gate, mux));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR2, 8, 0);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 21);
	mux = k210_clk_comp_mux(generic_sels, ARRAY_SIZE(generic_sels),
				base + K210_SYSCTL_SEL0, 13, 1);
	clk_dm(K210_CLK_TIMER0, k210_clk_comp("timer0", div, gate, mux));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR2, 8, 8);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 22);
	mux = k210_clk_comp_mux(generic_sels, ARRAY_SIZE(generic_sels),
				base + K210_SYSCTL_SEL0, 14, 1);
	clk_dm(K210_CLK_TIMER1, k210_clk_comp("timer1", div, gate, mux));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR2, 8, 16);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 23);
	mux = k210_clk_comp_mux(generic_sels, ARRAY_SIZE(generic_sels),
				base + K210_SYSCTL_SEL0, 15, 1);
	clk_dm(K210_CLK_TIMER2, k210_clk_comp("timer2", div, gate, mux));

	/* Dividing clocks, no mux */
	div = k210_clk_comp_div(base + K210_SYSCTL_THR0, 0, 4);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_CENT, 1);
	clk_dm(K210_CLK_SRAM0, k210_clk_comp_nomux("sram0", "aclk", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR0, 4, 4);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_CENT, 2);
	clk_dm(K210_CLK_SRAM1, k210_clk_comp_nomux("sram1", "aclk", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR0, 16, 4);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 0);
	clk_dm(K210_CLK_ROM, k210_clk_comp_nomux("rom", "aclk", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR0, 12, 4);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 3);
	clk_dm(K210_CLK_DVP, k210_clk_comp_nomux("dvp", "aclk", div, gate));

	/*
	 * XXX: the next three clocks may be using an even divider
	 * c.f. <https://github.com/kendryte/kendryte-standalone-sdk/issues/99>
	 */
	div = k210_clk_comp_div(base + K210_SYSCTL_SEL0, 3, 3);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_CENT, 3);
	clk_dm(K210_CLK_APB0, k210_clk_comp_nomux("apb0", "aclk", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_SEL0, 6, 3);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_CENT, 4);
	clk_dm(K210_CLK_APB1, k210_clk_comp_nomux("apb1", "aclk", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_SEL0, 9, 3);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_CENT, 5);
	clk_dm(K210_CLK_APB2, k210_clk_comp_nomux("apb2", "aclk", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR0, 8, 4);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 2);
	clk_dm(K210_CLK_AI, k210_clk_comp_nomux("ai", "pll1", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR3, 0, 16);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 10);
	clk_dm(K210_CLK_I2S0,
	       k210_clk_comp_nomux("i2s0", "pll2_half", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR3, 16, 16);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 11);
	clk_dm(K210_CLK_I2S1,
	       k210_clk_comp_nomux("i2s1", "pll2_half", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR4, 0, 16);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 12);
	clk_dm(K210_CLK_I2S2,
	       k210_clk_comp_nomux("i2s2", "pll2_half", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR6, 0, 8);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 24);
	clk_dm(K210_CLK_WDT0,
	       k210_clk_comp_nomux("wdt0", "in0_half", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR6, 8, 8);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 25);
	clk_dm(K210_CLK_WDT1,
	       k210_clk_comp_nomux("wdt1", "in0_half", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR1, 0, 8);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 6);
	clk_dm(K210_CLK_SPI0,
	       k210_clk_comp_nomux("spi0", "pll0_half", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR1, 8, 8);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 7);
	clk_dm(K210_CLK_SPI1,
	       k210_clk_comp_nomux("spi1", "pll0_half", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR1, 16, 8);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 8);
	clk_dm(K210_CLK_SPI2,
	       k210_clk_comp_nomux("spi2", "pll0_half", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR5, 8, 8);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 13);
	clk_dm(K210_CLK_I2C0,
	       k210_clk_comp_nomux("i2c0", "pll0_half", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR5, 16, 8);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 14);
	clk_dm(K210_CLK_I2C1,
	       k210_clk_comp_nomux("i2c1", "pll0_half", div, gate));

	div = k210_clk_comp_div(base + K210_SYSCTL_THR5, 24, 8);
	gate = k210_clk_comp_gate(base + K210_SYSCTL_EN_PERI, 15);
	clk_dm(K210_CLK_I2C2,
	       k210_clk_comp_nomux("i2c2", "pll0_half", div, gate));

	/* Gated clocks */
	clk_dm(K210_CLK_CPU,
	       k210_clk_gate("cpu", "aclk", base + K210_SYSCTL_EN_CENT, 0));
	clk_dm(K210_CLK_DMA,
	       k210_clk_gate("dma", "aclk", base + K210_SYSCTL_EN_PERI, 1));
	clk_dm(K210_CLK_FFT,
	       k210_clk_gate("fft", "aclk", base + K210_SYSCTL_EN_PERI, 4));
	clk_dm(K210_CLK_GPIO,
	       k210_clk_gate("gpio", "apb0", base + K210_SYSCTL_EN_PERI, 5));
	clk_dm(K210_CLK_UART1,
	       k210_clk_gate("uart1", "apb0", base + K210_SYSCTL_EN_PERI, 16));
	clk_dm(K210_CLK_UART2,
	       k210_clk_gate("uart2", "apb0", base + K210_SYSCTL_EN_PERI, 17));
	clk_dm(K210_CLK_UART3,
	       k210_clk_gate("uart3", "apb0", base + K210_SYSCTL_EN_PERI, 18));
	clk_dm(K210_CLK_FPIOA,
	       k210_clk_gate("fpioa", "apb0", base + K210_SYSCTL_EN_PERI, 20));
	clk_dm(K210_CLK_SHA,
	       k210_clk_gate("sha", "apb0", base + K210_SYSCTL_EN_PERI, 26));
	clk_dm(K210_CLK_AES,
	       k210_clk_gate("aes", "apb1", base + K210_SYSCTL_EN_PERI, 19));
	clk_dm(K210_CLK_OTP,
	       k210_clk_gate("otp", "apb1", base + K210_SYSCTL_EN_PERI, 27));
	clk_dm(K210_CLK_RTC,
	       k210_clk_gate("rtc", in0, base + K210_SYSCTL_EN_PERI, 29));

cleanup_base:
	unmap_sysmem(base);
	return err;
}

static const struct udevice_id k210_clk_ids[] = {
	{ .compatible = "kendryte,k210-clk" },
	{ },
};

U_BOOT_DRIVER(k210_clk) = {
	.name = "k210_clk",
	.id = UCLASS_CLK,
	.of_match = k210_clk_ids,
	.ops = &k210_clk_ops,
	.probe = k210_clk_probe,
};

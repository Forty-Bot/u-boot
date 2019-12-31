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

static const char * const generic_sels[] = { "in0_half", "pll0_half" };
/* The first clock is in0, which is filled in by k210_clk_probe */
static const char *aclk_sels[] = { NULL, "pll0_half" };
static const char *pll2_sels[] = { NULL, "pll0", "pll1" };

#define DIV(id, reg, shift, width) DIV_FLAGS(id, reg, shift, width, 0)
#define DIV_LIST \
	DIV_FLAGS(K210_CLK_ACLK, K210_SYSCTL_SEL0, 1, 2, \
		  CLK_DIVIDER_POWER_OF_TWO) \
	DIV(K210_CLK_APB0,   K210_SYSCTL_SEL0,  3,  3) \
	DIV(K210_CLK_APB1,   K210_SYSCTL_SEL0,  6,  3) \
	DIV(K210_CLK_APB2,   K210_SYSCTL_SEL0,  9,  3) \
	DIV(K210_CLK_SRAM0,  K210_SYSCTL_THR0,  0,  4) \
	DIV(K210_CLK_SRAM1,  K210_SYSCTL_THR0,  4,  4) \
	DIV(K210_CLK_AI,     K210_SYSCTL_THR0,  8,  4) \
	DIV(K210_CLK_DVP,    K210_SYSCTL_THR0, 12,  4) \
	DIV(K210_CLK_ROM,    K210_SYSCTL_THR0, 16,  4) \
	DIV(K210_CLK_SPI0,   K210_SYSCTL_THR1,  0,  8) \
	DIV(K210_CLK_SPI1,   K210_SYSCTL_THR1,  8,  8) \
	DIV(K210_CLK_SPI2,   K210_SYSCTL_THR1, 16,  8) \
	DIV(K210_CLK_SPI3,   K210_SYSCTL_THR1, 24,  8) \
	DIV(K210_CLK_TIMER0, K210_SYSCTL_THR2,  0,  8) \
	DIV(K210_CLK_TIMER1, K210_SYSCTL_THR2,  8,  8) \
	DIV(K210_CLK_TIMER2, K210_SYSCTL_THR2, 16,  8) \
	DIV(K210_CLK_I2S0,   K210_SYSCTL_THR3,  0, 16) \
	DIV(K210_CLK_I2S1,   K210_SYSCTL_THR3, 16, 16) \
	DIV(K210_CLK_I2S2,   K210_SYSCTL_THR4,  0, 16) \
	DIV(K210_CLK_I2S0_M, K210_SYSCTL_THR4, 16,  8) \
	DIV(K210_CLK_I2S1_M, K210_SYSCTL_THR4, 24,  8) \
	DIV(K210_CLK_I2S2_M, K210_SYSCTL_THR4,  0,  8) \
	DIV(K210_CLK_I2C0,   K210_SYSCTL_THR5,  8,  8) \
	DIV(K210_CLK_I2C1,   K210_SYSCTL_THR5, 16,  8) \
	DIV(K210_CLK_I2C2,   K210_SYSCTL_THR5, 24,  8) \
	DIV(K210_CLK_WDT0,   K210_SYSCTL_THR6,  0,  8) \
	DIV(K210_CLK_WDT1,   K210_SYSCTL_THR6,  8,  8)

#define _DIVIFY(id) K210_CLK_DIV_##id
#define DIVIFY(id) _DIVIFY(id)

enum k210_clk_div_ids {
#define DIV_FLAGS(id, ...) DIVIFY(id),
	DIV_LIST
#undef DIV_FLAGS
};

static struct clk_divider k210_clk_dividers[]  = {
#define DIV_FLAGS(id, _reg, _shift, _width, _flags) \
	[DIVIFY(id)] = { \
		.reg = (void *)(_reg), \
		.shift = (_shift), \
		.width = (_width), \
		.flags = (_flags), \
	},
	DIV_LIST
#undef DIV_FLAGS
};

#undef DIV
#undef DIV_LIST

#define GATE_LIST \
	GATE(K210_CLK_CPU,    K210_SYSCTL_EN_CENT,  0) \
	GATE(K210_CLK_SRAM0,  K210_SYSCTL_EN_CENT,  1) \
	GATE(K210_CLK_SRAM1,  K210_SYSCTL_EN_CENT,  2) \
	GATE(K210_CLK_APB0,   K210_SYSCTL_EN_CENT,  3) \
	GATE(K210_CLK_APB1,   K210_SYSCTL_EN_CENT,  4) \
	GATE(K210_CLK_APB2,   K210_SYSCTL_EN_CENT,  5) \
	GATE(K210_CLK_ROM,    K210_SYSCTL_EN_PERI,  0) \
	GATE(K210_CLK_DMA,    K210_SYSCTL_EN_PERI,  1) \
	GATE(K210_CLK_AI,     K210_SYSCTL_EN_PERI,  2) \
	GATE(K210_CLK_DVP,    K210_SYSCTL_EN_PERI,  3) \
	GATE(K210_CLK_FFT,    K210_SYSCTL_EN_PERI,  4) \
	GATE(K210_CLK_GPIO,   K210_SYSCTL_EN_PERI,  5) \
	GATE(K210_CLK_SPI0,   K210_SYSCTL_EN_PERI,  6) \
	GATE(K210_CLK_SPI1,   K210_SYSCTL_EN_PERI,  7) \
	GATE(K210_CLK_SPI2,   K210_SYSCTL_EN_PERI,  8) \
	GATE(K210_CLK_SPI3,   K210_SYSCTL_EN_PERI,  9) \
	GATE(K210_CLK_I2S0,   K210_SYSCTL_EN_PERI, 10) \
	GATE(K210_CLK_I2S1,   K210_SYSCTL_EN_PERI, 11) \
	GATE(K210_CLK_I2S2,   K210_SYSCTL_EN_PERI, 12) \
	GATE(K210_CLK_I2C0,   K210_SYSCTL_EN_PERI, 13) \
	GATE(K210_CLK_I2C1,   K210_SYSCTL_EN_PERI, 14) \
	GATE(K210_CLK_I2C2,   K210_SYSCTL_EN_PERI, 15) \
	GATE(K210_CLK_UART1,  K210_SYSCTL_EN_PERI, 16) \
	GATE(K210_CLK_UART2,  K210_SYSCTL_EN_PERI, 17) \
	GATE(K210_CLK_UART3,  K210_SYSCTL_EN_PERI, 18) \
	GATE(K210_CLK_AES,    K210_SYSCTL_EN_PERI, 19) \
	GATE(K210_CLK_FPIOA,  K210_SYSCTL_EN_PERI, 20) \
	GATE(K210_CLK_TIMER0, K210_SYSCTL_EN_PERI, 21) \
	GATE(K210_CLK_TIMER1, K210_SYSCTL_EN_PERI, 22) \
	GATE(K210_CLK_TIMER2, K210_SYSCTL_EN_PERI, 23) \
	GATE(K210_CLK_WDT0,   K210_SYSCTL_EN_PERI, 24) \
	GATE(K210_CLK_WDT1,   K210_SYSCTL_EN_PERI, 25) \
	GATE(K210_CLK_SHA,    K210_SYSCTL_EN_PERI, 26) \
	GATE(K210_CLK_OTP,    K210_SYSCTL_EN_PERI, 27) \
	GATE(K210_CLK_RTC,    K210_SYSCTL_EN_PERI, 29)

#define _GATEIFY(id) K210_CLK_GATE_##id
#define GATEIFY(id) _GATEIFY(id)

enum k210_clk_gate_ids {
#define GATE(id, ...) GATEIFY(id),
	GATE_LIST
#undef GATE
};

static struct clk_gate k210_clk_gates[] = {
#define GATE(id, _reg, _idx) \
	[GATEIFY(id)] = { \
		.reg = (void *)(_reg), \
		.bit_idx = (_idx), \
	},
	GATE_LIST
#undef GATE
};

#undef GATE_LIST

#define MUX(id, reg, shift, width) \
	MUX_PARENTS(id, generic_sels, reg, shift, width)
#define MUX_LIST \
	MUX_PARENTS(K210_CLK_PLL2, pll2_sels, K210_SYSCTL_PLL2, 26, 2) \
	MUX_PARENTS(K210_CLK_ACLK, aclk_sels, K210_SYSCTL_SEL0,  0, 1) \
	MUX(K210_CLK_SPI3,   K210_SYSCTL_SEL0, 12, 1) \
	MUX(K210_CLK_TIMER0, K210_SYSCTL_SEL0, 13, 1) \
	MUX(K210_CLK_TIMER1, K210_SYSCTL_SEL0, 14, 1) \
	MUX(K210_CLK_TIMER2, K210_SYSCTL_SEL0, 15, 1)

#define _MUXIFY(id) K210_CLK_MUX_##id
#define MUXIFY(id) _MUXIFY(id)

enum k210_clk_mux_ids {
#define MUX_PARENTS(id, ...) MUXIFY(id),
	MUX_LIST
#undef MUX_PARENTS
};

static struct clk_mux k210_clk_muxes[] = {
#define MUX_PARENTS(id, parents, _reg, _shift, _width) \
	[MUXIFY(id)] = { \
		.parent_names = (const char * const *)(parents), \
		.num_parents = ARRAY_SIZE(parents), \
		.reg = (void *)(_reg), \
		.shift = (_shift), \
		.mask = BIT(_width) - 1, \
	},
	MUX_LIST
#undef MUX_PARENTS
};

#undef MUX
#undef MUX_LIST

static struct k210_pll k210_clk_plls[] = {
#define PLL(_reg, _shift, _width) { \
	.reg = (void *)(_reg), \
	.lock = (void *)K210_SYSCTL_PLL_LOCK, \
	.shift = (_shift), \
	.width = (_width), \
}
	[0] = PLL(K210_SYSCTL_PLL0,  0, 2),
	[1] = PLL(K210_SYSCTL_PLL1,  8, 1),
	[2] = PLL(K210_SYSCTL_PLL2, 16, 1),
#undef PLL
};

#define COMP(id, mux, div, gate) \
	COMP_FULL(id, &(mux)->clk, &clk_mux_ops, \
		  &(div)->clk, &clk_divider_ops, \
		  &(gate)->clk, &clk_gate_ops)
#define COMP_ID(id) \
	COMP(id, &k210_clk_muxes[MUXIFY(id)], \
	     &k210_clk_dividers[DIVIFY(id)], \
	     &k210_clk_gates[GATEIFY(id)])
#define COMP_NOMUX(id, div, gate) \
	COMP_FULL(id, NULL, NULL, \
		  &(div)->clk, &clk_divider_ops, \
		  &(gate)->clk, &clk_gate_ops)
#define COMP_NOMUX_ID(id) \
	COMP_NOMUX(id, &k210_clk_dividers[DIVIFY(id)], \
		   &k210_clk_gates[GATEIFY(id)])
#define COMP_LIST \
	COMP_FULL(K210_CLK_PLL2, \
		  &k210_clk_muxes[MUXIFY(K210_CLK_PLL2)].clk, &clk_mux_ops, \
		  &k210_clk_plls[2].clk, &k210_pll_ops, \
		  &k210_clk_plls[2].clk, &k210_pll_ops) \
	COMP_FULL(K210_CLK_ACLK, \
		  &k210_clk_muxes[MUXIFY(K210_CLK_ACLK)].clk, &clk_mux_ops, \
		  &k210_clk_dividers[DIVIFY(K210_CLK_ACLK)].clk, \
		  &clk_divider_ops, \
		  NULL, NULL) \
	COMP_ID(K210_CLK_SPI3) \
	COMP_ID(K210_CLK_TIMER0) \
	COMP_ID(K210_CLK_TIMER1) \
	COMP_ID(K210_CLK_TIMER2) \
	COMP_NOMUX_ID(K210_CLK_SRAM0) \
	COMP_NOMUX_ID(K210_CLK_SRAM1) \
	COMP_NOMUX_ID(K210_CLK_ROM) \
	COMP_NOMUX_ID(K210_CLK_DVP) \
	COMP_NOMUX_ID(K210_CLK_APB0) \
	COMP_NOMUX_ID(K210_CLK_APB1) \
	COMP_NOMUX_ID(K210_CLK_APB2) \
	COMP_NOMUX_ID(K210_CLK_AI) \
	COMP_NOMUX_ID(K210_CLK_I2S0) \
	COMP_NOMUX_ID(K210_CLK_I2S1) \
	COMP_NOMUX_ID(K210_CLK_I2S2) \
	COMP_NOMUX_ID(K210_CLK_WDT0) \
	COMP_NOMUX_ID(K210_CLK_WDT1) \
	COMP_NOMUX_ID(K210_CLK_SPI0) \
	COMP_NOMUX_ID(K210_CLK_SPI1) \
	COMP_NOMUX_ID(K210_CLK_SPI2) \
	COMP_NOMUX_ID(K210_CLK_I2C0) \
	COMP_NOMUX_ID(K210_CLK_I2C1) \
	COMP_NOMUX_ID(K210_CLK_I2C2)

#define _COMPIFY(id) K210_CLK_COMP_##id
#define COMPIFY(id) _COMPIFY(id)

enum k210_clk_comp_ids {
#define COMP_FULL(id, ...) COMPIFY(id),
	COMP_LIST
#undef COMP_FULL
};

static struct clk_composite k210_clk_comps[] = {
#define COMP_FULL(id, _mux, _mux_ops, _div, _div_ops, _gate, _gate_ops) \
	[COMPIFY(id)] = { \
		.mux = (_mux), \
		.mux_ops = (_mux_ops), \
		.rate = (_div), \
		.rate_ops = (_div_ops), \
		.gate = (_gate), \
		.gate_ops = (_gate_ops), \
	},
	COMP_LIST
#undef COMP_FULL
};

#undef COMP
#undef COMP_ID
#undef COMP_NOMUX
#undef COMP_NOMUX_ID
#undef COMP_LIST

static struct clk *k210_clk_bypass_children = {
	&k210_clk_comps[COMPIFY(K210_CLK_ACLK)].clk,
};

static struct clk *k210_clk_bypass_saved_parents = {
	NULL,
};

static struct k210_bypass k210_clk_bypass = {
	.bypassee = &k210_clk_plls[0].clk,
	.bypassee_ops = &k210_pll_ops,
	.children = &k210_clk_bypass_children,
	.child_count = 1,
	.saved_parents = &k210_clk_bypass_saved_parents,
};

static bool probed;

static int k210_clk_probe(struct udevice *dev)
{
	int ret, i;
	const char *in0;
	struct clk *in0_clk;
	void *base;

	/* Only one instance of this driver allowed */
	if (READ_ONCE(probed))
		return -ENOTSUPP;

	base = dev_read_addr_ptr(dev_get_parent(dev));
	if (!base)
		return -EINVAL;

	in0_clk = kzalloc(sizeof(*in0_clk), GFP_KERNEL);
	if (!in0_clk)
		return -ENOMEM;

	ret = clk_get_by_index(dev, 0, in0_clk);
	if (ret)
		return ret;
	in0 = in0_clk->dev->name;

	WRITE_ONCE(probed, true);

	aclk_sels[0] = in0;
	pll2_sels[0] = in0;

	/* Fixup registers to be absolute, rather than relative */
#define FIXUP_REGS(clocks) \
	for (i = 0; i < ARRAY_SIZE(clocks); i++) \
		clocks[i].reg += (ulong)base
	FIXUP_REGS(k210_clk_dividers);
	FIXUP_REGS(k210_clk_gates);
	FIXUP_REGS(k210_clk_muxes);
#undef FIXUP_REGS
	for (i = 0; i < ARRAY_SIZE(k210_clk_plls); i++) {
		k210_clk_plls[i].reg += (ulong)base;
		k210_clk_plls[i].lock += (ulong)base;
	}

	/*
	 * All PLLs have a broken bypass, but pll0 has the CPU downstream, so we
	 * need to manually reparent it whenever we configure pll0
	 */
	k210_clk_bypass.alt = in0_clk;
	clk_dm(K210_CLK_PLL0,
	       k210_register_bypass_struct("pll0", in0, &k210_clk_bypass));
	clk_dm(K210_CLK_PLL1,
	       k210_register_pll_struct("pll1", in0, &k210_clk_plls[1]));
	/* PLL2 is muxed, so set up a composite clock */
	clk_dm(K210_CLK_PLL2,
	       clk_register_composite_struct("pll2", pll2_sels,
					     ARRAY_SIZE(pll2_sels),
					     &k210_clk_comps[COMPIFY(K210_CLK_PLL2)]));

	/* Half-frequency clocks for "even" dividers */
	clk_dm(K210_CLK_IN0_H, k210_clk_half("in0_half", in0));
	clk_dm(K210_CLK_PLL0_H, k210_clk_half("pll0_half", "pll0"));
	clk_dm(K210_CLK_PLL2_H, k210_clk_half("pll2_half", "pll2"));

	/* ACLK has no gate */
	clk_dm(K210_CLK_ACLK,
	       clk_register_composite_struct("aclk", aclk_sels,
					     ARRAY_SIZE(aclk_sels),
					     &k210_clk_comps[COMPIFY(K210_CLK_ACLK)]));

#define REGISTER_COMP(id, name) \
	clk_dm(id, clk_register_composite_struct(name, generic_sels, \
						 ARRAY_SIZE(generic_sels), \
						 &k210_clk_comps[COMPIFY(id)]))
	REGISTER_COMP(K210_CLK_SPI3,   "spi3");
	REGISTER_COMP(K210_CLK_TIMER0, "timer0");
	REGISTER_COMP(K210_CLK_TIMER1, "timer1");
	REGISTER_COMP(K210_CLK_TIMER2, "timer2");
#undef COMP

	/* Dividing clocks, no mux */
#define REGISTER_COMP_NOMUX(id, name, _parent) do { \
	const char *parent = _parent; \
	clk_dm(id, \
	       clk_register_composite_struct(name, &parent, 1, \
					     &k210_clk_comps[COMPIFY(id)])); \
} while (false)
	REGISTER_COMP_NOMUX(K210_CLK_SRAM0, "sram0",  "aclk");
	REGISTER_COMP_NOMUX(K210_CLK_SRAM1, "sram1",  "aclk");
	REGISTER_COMP_NOMUX(K210_CLK_ROM,   "rom",    "aclk");
	REGISTER_COMP_NOMUX(K210_CLK_DVP,   "dvp",    "aclk");
	REGISTER_COMP_NOMUX(K210_CLK_APB0,  "apb0",   "aclk");
	REGISTER_COMP_NOMUX(K210_CLK_APB1,  "apb1",   "aclk");
	REGISTER_COMP_NOMUX(K210_CLK_APB2,  "apb2",   "aclk");
	REGISTER_COMP_NOMUX(K210_CLK_AI,    "ai",     "pll1");
	REGISTER_COMP_NOMUX(K210_CLK_I2S0,  "i2s0",   "pll2_half");
	REGISTER_COMP_NOMUX(K210_CLK_I2S1,  "i2s1",   "pll2_half");
	REGISTER_COMP_NOMUX(K210_CLK_I2S2,  "i2s2",   "pll2_half");
	REGISTER_COMP_NOMUX(K210_CLK_WDT0,  "wdt0",   "in0_half");
	REGISTER_COMP_NOMUX(K210_CLK_WDT1,  "wdt1",   "in0_half");
	REGISTER_COMP_NOMUX(K210_CLK_SPI0,  "spi0",   "pll0_half");
	REGISTER_COMP_NOMUX(K210_CLK_SPI1,  "spi1",   "pll0_half");
	REGISTER_COMP_NOMUX(K210_CLK_SPI2,  "spi2",   "pll0_half");
	REGISTER_COMP_NOMUX(K210_CLK_I2C0,  "i2c0",   "pll0_half");
	REGISTER_COMP_NOMUX(K210_CLK_I2C1,  "i2c1",   "pll0_half");
	REGISTER_COMP_NOMUX(K210_CLK_I2C2,  "i2c2",   "pll0_half");
#undef REGISTER_COMP_NOMUX

	/* Dividing clocks */
#define REGISTER_DIV(id, name, parent) clk_dm(id, \
	clk_register_divider_struct(name, parent, \
				    &k210_clk_dividers[DIVIFY(id)]))
	REGISTER_DIV(K210_CLK_I2S0_M, "i2s0_m", "pll2_half");
	REGISTER_DIV(K210_CLK_I2S1_M, "i2s1_m", "pll2_half");
	REGISTER_DIV(K210_CLK_I2S2_M, "i2s2_m", "pll2_half");
#undef REGISTER_DIV

	/* Gated clocks */
#define REGISTER_GATE(id, name, parent) \
	clk_dm(id, clk_register_gate_struct(name, parent, \
					    &k210_clk_gates[GATEIFY(id)]))
	REGISTER_GATE(K210_CLK_CPU,   "cpu",    "aclk");
	REGISTER_GATE(K210_CLK_DMA,   "dma",    "aclk");
	REGISTER_GATE(K210_CLK_FFT,   "fft",    "aclk");
	REGISTER_GATE(K210_CLK_GPIO,  "gpio",   "apb0");
	REGISTER_GATE(K210_CLK_UART1, "uart1",  "apb0");
	REGISTER_GATE(K210_CLK_UART2, "uart2",  "apb0");
	REGISTER_GATE(K210_CLK_UART3, "uart3",  "apb0");
	REGISTER_GATE(K210_CLK_FPIOA, "fpioa",  "apb0");
	REGISTER_GATE(K210_CLK_SHA,   "sha",    "apb0");
	REGISTER_GATE(K210_CLK_AES,   "aes",    "apb1");
	REGISTER_GATE(K210_CLK_OTP,   "otp",    "apb1");
	REGISTER_GATE(K210_CLK_RTC,   "rtc",    in0);
#undef REGISTER_GATE

	return 0;
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

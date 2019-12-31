// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Sean Anderson <seanga2@gmail.com>
 */
#include "pll.h"

#define LOG_CATEGORY UCLASS_CLK
#include <asm/io.h>
/* For DIV_ROUND_DOWN_ULL, defined in linux/kernel.h */
#include <div64.h>
#include <dt-bindings/clock/k210-sysctl.h>
#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <log.h>

#define CLK_K210_PLL "clk_k210_pll"
#define to_k210_pll(_clk) container_of(_clk, struct k210_pll, clk)

static int k210_pll_enable(struct clk *clk);
static int k210_pll_disable(struct clk *clk);

/*
 * The k210 PLLs have three factors: r, f, and od. The equation for the output
 * rate is
 * 	rate = (rate_in * f) / (r * od).
 * Moving knowns to one side of the equation, we get
 *	rate / rate_in = f / (r * od)
 * Rearranging slightly,
 *	abs_error = abs((rate / rate_in) - (f / (r * od))).
 * To get relative, error, we divide by the expected ratio
 *	error = abs((rate / rate_in) - (f / (r * od))) / (rate / rate_in).
 * Simplifying,
 *	error = abs(1 - f / (r * od)) / (rate / rate_in)
 *	error = abs(1 - (f * rate_in) / (r * od * rate))
 * Using the constants ratio = rate / rate_in and inv_ratio = rate_in / rate,
 *	error = abs((f * inv_ratio) / (r * od) - 1)
 * This is the error used in evaluating parameters.
 *
 * r and od are four bits each, while f is six bits. Because r and od are
 * multiplied together, instead of the full 256 values possible if both bits
 * were used fully, there are only 97 distinct products. Combined with f, there
 * are 6208 possible settings for the PLL. However, most of these settings can
 * be ruled out immediately because they do not have the correct ratio. Of these
 * remaining options, there are at most 97, due to the limited range of f, r,
 * and od.
 *
 * Throughout the calculation function, fixed point arithmetic is used. Because
 * the range of rate and rate_in may be up to 1.8 GHz, or around 2^30, 64-bit
 * 32.32 fixed-point numbers are used to represent ratios. In general, to
 * implement division, the numerator is first multiplied by 2^32. This gives a
 * result where the whole number part is in the upper 32 bits, and the fraction
 * is in the lower 32 bits.
 *
 * The r and od factors are stored in a table. This is to make it easy to find
 * the next-largest product.
 *
 * In general, rounding is done to the closest integer. This helps find the best
 * approximation for the ratio. Rounding in one direction (e.g down) could cause
 * the function to miss a better ratio with one of the parameters increased by
 * one.
 */

/*
 * The factors table was generated with the following python code:
 * factors = {}
 * for i in range(1, 17):
 *  for j in range(1, 17):
 *   (x, y) = factors.get(i*j) or (17, 1)
 *   # Pick "balanced" factors
 *   if abs(i - j) < abs(x - y):
 *    factors[i*j] = (i, j)
 * for k, v in sorted(factors.items()):
 *  print("PACK(%s, %s)," % v)
 */
#define PACK(r, od) ((((r - 1) & 0xF) << 4) | ((od - 1) & 0xF))
#define UNPACK_R(val) (((val >> 4) & 0xF) + 1)
#define UNPACK_OD(val) ((val & 0xF) + 1)
static const u8 factors[] = {
	PACK(1, 1),
	PACK(1, 2),
	PACK(1, 3),
	PACK(2, 2),
	PACK(1, 5),
	PACK(2, 3),
	PACK(1, 7),
	PACK(2, 4),
	PACK(3, 3),
	PACK(2, 5),
	PACK(1, 11),
	PACK(3, 4),
	PACK(1, 13),
	PACK(2, 7),
	PACK(3, 5),
	PACK(4, 4),
	PACK(3, 6),
	PACK(4, 5),
	PACK(3, 7),
	PACK(2, 11),
	PACK(4, 6),
	PACK(5, 5),
	PACK(2, 13),
	PACK(3, 9),
	PACK(4, 7),
	PACK(5, 6),
	PACK(4, 8),
	PACK(3, 11),
	PACK(5, 7),
	PACK(6, 6),
	PACK(3, 13),
	PACK(5, 8),
	PACK(6, 7),
	PACK(4, 11),
	PACK(5, 9),
	PACK(6, 8),
	PACK(7, 7),
	PACK(5, 10),
	PACK(4, 13),
	PACK(6, 9),
	PACK(5, 11),
	PACK(7, 8),
	PACK(6, 10),
	PACK(7, 9),
	PACK(8, 8),
	PACK(5, 13),
	PACK(6, 11),
	PACK(7, 10),
	PACK(8, 9),
	PACK(5, 15),
	PACK(7, 11),
	PACK(6, 13),
	PACK(8, 10),
	PACK(9, 9),
	PACK(7, 12),
	PACK(8, 11),
	PACK(9, 10),
	PACK(7, 13),
	PACK(8, 12),
	PACK(7, 14),
	PACK(9, 11),
	PACK(10, 10),
	PACK(8, 13),
	PACK(7, 15),
	PACK(9, 12),
	PACK(10, 11),
	PACK(8, 14),
	PACK(9, 13),
	PACK(10, 12),
	PACK(11, 11),
	PACK(9, 14),
	PACK(8, 16),
	PACK(10, 13),
	PACK(11, 12),
	PACK(9, 15),
	PACK(10, 14),
	PACK(11, 13),
	PACK(12, 12),
	PACK(10, 15),
	PACK(11, 14),
	PACK(12, 13),
	PACK(10, 16),
	PACK(11, 15),
	PACK(12, 14),
	PACK(13, 13),
	PACK(11, 16),
	PACK(12, 15),
	PACK(13, 14),
	PACK(12, 16),
	PACK(13, 15),
	PACK(14, 14),
	PACK(13, 16),
	PACK(14, 15),
	PACK(14, 16),
	PACK(15, 15),
	PACK(15, 16),
	PACK(16, 16),
};

struct k210_pll_params {
	u8 r;
	u8 f;
	u8 od;
};

static int k210_pll_calc_params(u32 rate, u32 rate_in,
				struct k210_pll_params *best)
{
	int i;
	s64 error, best_error;
	u64 ratio, inv_ratio; /* fixed point 32.32 ratio of the rates */
	u64 r, f, od;

	/* Can't go over 1.8 GHz */
	if (rate > 1800000000)
		return -EINVAL;

	ratio = DIV_ROUND_CLOSEST_ULL((u64)rate << 32, rate_in);
	inv_ratio = DIV_ROUND_CLOSEST_ULL((u64)rate_in << 32, rate);
	/* Can't increase by more than 64 or reduce by more than 256 */
	if (rate > rate_in && ratio > (64ULL << 32))
		return -EINVAL;
	else if (rate <= rate_in && inv_ratio > (256ULL << 32))
		return -EINVAL;

	/* Variables get immediately incremented, so start at -1th iteration */ 
	i = -1;
	f = 0;
	r = 0;
	od = 0;
	best_error = S64_MAX;
	/* do-while here so we always try at least one ratio */
	do {
		/*
		 * Try the next largest value for f (or r and od) and
		 * recalculate the other parameters based on that
		 */
		if (rate > rate_in) {
			i++;
			r = UNPACK_R(factors[i]);
			od = UNPACK_OD(factors[i]);
			
			/* Round close */
			f = (r * od * ratio + BIT(31)) >> 32;
			if (f > 64)
				f = 64;

		} else {
			u64 last_od = od;
			u64 last_r = r;
			u64 tmp = ++f * inv_ratio;
			bool round_up = !!(tmp & BIT(31));
			u32 goal = (tmp >> 32) + round_up;
			u32 err, last_err;

			/* Get the next r/od pair in factors */
			while (r * od < goal && i + 1 < ARRAY_SIZE(factors)) {
				i++;
				r = UNPACK_R(factors[i]);
				od = UNPACK_OD(factors[i]);
			}

			/*
			 * This is a case of double rounding. If we rounded up
			 * above, we need to round down (in cases of ties) here.
			 * This prevents off-by-one errors resulting from
			 * choosing X+2 over X when X.Y rounds up to X+1 and
			 * there is no r * od = X+1. For the converse, when X.Y
			 * is rounded down to X, we should choose X+1 over X-1.
			 */
			err = abs(r * od - goal);
			last_err = abs(last_r * last_od - goal);
			if (last_err < err || (round_up && (last_err = err))) {
				i--;
				r = last_r;
				od = last_od;
			}
		}
		/* 32.0 * 32.32 = 64.32 */
		error = DIV_ROUND_CLOSEST_ULL(f * inv_ratio, r * od);
		/* The lower 16 bits are spurious */
		error = abs((error - BIT(32))) >> 16;

		if (error < best_error) {
			best->r = r;
			best->f = f;
			best->od = od;
			best_error = error;
		}
	} while (f < 64 && i + 1 < ARRAY_SIZE(factors) && error != 0);

	log_debug("best error %lld\n", best_error);
	return 0;
}

static ulong k210_pll_set_rate(struct clk *clk, ulong rate)
{
	int err;
	long long rate_in = clk_get_parent_rate(clk);
	struct k210_pll_params params = {};
	struct k210_pll *pll = to_k210_pll(clk);
	u32 reg;
	
	if (rate_in < 0)
		return rate_in;

	log_debug("Calculating parameters with rate=%lu and rate_in=%lld\n",
		  rate, rate_in);
	err = k210_pll_calc_params(rate, rate_in, &params);
	if (err)
		return err;
	log_debug("Got r=%u f=%u od=%u\n", params.r, params.f, params.od);

	/*
	 * Don't use clk_disable as it might not actually disable the pll due to
	 * refcounting
	 */
	err = k210_pll_disable(clk);
	if (err)
		return err;

	reg = readl(pll->reg);
	reg &= ~K210_PLL_CLKR
	    & ~K210_PLL_CLKF
	    & ~K210_PLL_CLKOD
	    & ~K210_PLL_BWADJ;
	reg |= FIELD_PREP(K210_PLL_CLKR, params.r - 1)
	    | FIELD_PREP(K210_PLL_CLKF, params.f - 1)
	    | FIELD_PREP(K210_PLL_CLKOD, params.od - 1)
	    | FIELD_PREP(K210_PLL_BWADJ, params.f - 1);
	writel(reg, pll->reg);

	err = k210_pll_enable(clk);
	if (err)
		return err;

	return clk_get_rate(clk);
}

static ulong k210_pll_get_rate(struct clk *clk)
{

	long long rate_in = clk_get_parent_rate(clk);
	struct k210_pll *pll = to_k210_pll(clk);
	u64 r, f, od;
	u32 reg = readl(pll->reg);

	if (rate_in < 0)
		return rate_in;

	if (reg & K210_PLL_BYPASS)
		return rate_in;
	
	r = FIELD_GET(K210_PLL_CLKR, reg) + 1;
	f = FIELD_GET(K210_PLL_CLKF, reg) + 1;
	od = FIELD_GET(K210_PLL_CLKOD, reg) + 1;

	return DIV_ROUND_DOWN_ULL(((u64)rate_in) * f, r * od);
}

/* Check if the PLL is locked */
static int k210_pll_locked(struct k210_pll *pll)
{
	u32 reg = readl(pll->lock);

	return (reg & pll->lock_mask) == pll->lock_mask;
}

/*
 * Wait for the PLL to be locked. If the PLL is not locked, try clearing the
 * slip before retrying
 */
static void k210_pll_waitfor_lock(struct k210_pll *pll)
{
	while (!k210_pll_locked(pll)) {
		u32 reg = readl(pll->lock);

		reg |= BIT(pll->shift + K210_PLL_CLEAR_SLIP);
		writel(reg, pll->lock);
		udelay(1);
	}
}

/* Adapted from sysctl_pll_enable */
static int k210_pll_enable(struct clk *clk)
{
	struct k210_pll *pll = to_k210_pll(clk);
	u32 reg = readl(pll->reg);

	reg &= ~K210_PLL_BYPASS;
	writel(reg, pll->reg);

	reg |= K210_PLL_PWRD;
	writel(reg, pll->reg);

	/* Ensure reset is low before asserting it */
	reg &= ~K210_PLL_RESET;
	writel(reg, pll->reg);
	reg |= K210_PLL_RESET;
	writel(reg, pll->reg);
	/* FIXME: this doesn't really have to be a whole microsecond */
	udelay(1);
	reg &= ~K210_PLL_RESET;
	writel(reg, pll->reg);

	k210_pll_waitfor_lock(pll);
	return 0;
}

static int k210_pll_disable(struct clk *clk)
{
	struct k210_pll *pll = to_k210_pll(clk);
	u32 reg = readl(pll->reg);

	/*
	 * Bypassing before powering off is important so child clocks don't stop
	 * working. This is especially important for pll0, the indirect parent
	 * of the cpu clock.
	 */
	reg |= K210_PLL_BYPASS;
	writel(reg, pll->reg);

	reg &= ~K210_PLL_PWRD;
	writel(reg, pll->reg);
	return 0;
}

const struct clk_ops k210_pll_ops = {
	.get_rate = k210_pll_get_rate,
	.set_rate = k210_pll_set_rate,
	.enable = k210_pll_enable,
	.disable = k210_pll_disable,
};

struct k210_pll *k210_clk_comp_pll(void __iomem *reg, void __iomem *lock,
				       u8 shift, u8 width)
{
	struct k210_pll *pll;

	
	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return pll;
	pll->reg = reg;
	pll->lock = lock;
	pll->shift = shift;
	pll->lock_mask = GENMASK(shift + width, shift);
	return pll;
}

struct clk *k210_clk_pll(const char *name, const char *parent_name,
			 void __iomem *reg, void __iomem *lock, u8 shift,
			 u8 width)
{
	int err;
	struct k210_pll *pll;

	pll = k210_clk_comp_pll(reg, lock, shift, width);
	if (!pll)
		return ERR_PTR(-ENOMEM);

	err = clk_register(&pll->clk, CLK_K210_PLL, name, parent_name);
	if (err) {
		kfree(pll);
		return ERR_PTR(err);
	}
	return &pll->clk;
}

U_BOOT_DRIVER(k210_pll) = {
	.name	= CLK_K210_PLL,
	.id	= UCLASS_CLK,
	.ops	= &k210_pll_ops,
	.flags = DM_FLAG_PRE_RELOC,
};

#ifndef K210_PLL_H
#define K210_PLL_H

#include <clk.h>

#define K210_PLL_CLKR GENMASK(3, 0)
#define K210_PLL_CLKF GENMASK(9, 4)
#define K210_PLL_CLKOD GENMASK(13, 10)
#define K210_PLL_BWADJ GENMASK(19, 14)
#define K210_PLL_RESET BIT(20)
#define K210_PLL_PWRD BIT(21)
#define K210_PLL_INTFB BIT(22)
#define K210_PLL_BYPASS BIT(23)
#define K210_PLL_TEST BIT(24)
#define K210_PLL_EN BIT(25)
#define K210_PLL_TEST_EN BIT(26)

#define K210_PLL_LOCK 0
#define K210_PLL_CLEAR_SLIP 2
#define K210_PLL_TEST_OUT 3

struct k210_pll {
	struct clk clk;
	void __iomem *reg; /* Base PLL register */
	void __iomem *lock; /* Common PLL lock register */
	u8 shift; /* Offset of bits in lock register */
	u8 lock_mask; /* Mask of lock bits to test against, pre-shifted */
};

extern const struct clk_ops k210_pll_ops;

struct k210_pll *k210_clk_comp_pll(void __iomem *reg, void __iomem *lock,
				   u8 shift, u8 width);
struct clk *k210_clk_pll(const char *name, const char *parent_name,
			 void __iomem *reg, void __iomem *lock, u8 shift,
			 u8 width);

#endif /* K210_PLL_H */

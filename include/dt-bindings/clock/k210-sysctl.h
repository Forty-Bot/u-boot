/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2019-20 Sean Anderson <seanga2@gmail.com>
 */

#ifndef CLOCK_K210_SYSCTL_H
#define CLOCK_K210_SYSCTL_H

/*
 * Arbitrary identifiers for clocks.
 */
#define K210_CLK_NONE   0
#define K210_CLK_PLL0   1
#define K210_CLK_PLL1   2
#define K210_CLK_PLL2   3
#define K210_CLK_CPU    4
#define K210_CLK_SRAM0  5
#define K210_CLK_SRAM1  6
#define K210_CLK_APB0   7
#define K210_CLK_APB1   8
#define K210_CLK_APB2   9
#define K210_CLK_ROM    10
#define K210_CLK_DMA    11
#define K210_CLK_AI     12
#define K210_CLK_DVP    13
#define K210_CLK_FFT    14
#define K210_CLK_GPIO   15
#define K210_CLK_SPI0   16
#define K210_CLK_SPI1   17
#define K210_CLK_SPI2   18
#define K210_CLK_SPI3   19
#define K210_CLK_I2S0   20
#define K210_CLK_I2S1   21
#define K210_CLK_I2S2   22
#define K210_CLK_I2S0_M 23
#define K210_CLK_I2S1_M 24
#define K210_CLK_I2S2_M 25
#define K210_CLK_I2C0   26
#define K210_CLK_I2C1   27
#define K210_CLK_I2C2   28
#define K210_CLK_UART1  29
#define K210_CLK_UART2  30
#define K210_CLK_UART3  31
#define K210_CLK_AES    32
#define K210_CLK_FPIOA  33
#define K210_CLK_TIMER0 34
#define K210_CLK_TIMER1 35
#define K210_CLK_TIMER2 36
#define K210_CLK_WDT0   37
#define K210_CLK_WDT1   38
#define K210_CLK_SHA    39
#define K210_CLK_OTP    40
#define K210_CLK_RTC    41
#define K210_CLK_ACLK   42

#endif /* CLOCK_K210_SYSCTL_H */

/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2019 Sean Anderson <seanga2@gmail.com>
 */

#ifndef CLOCK_K210_SYSCTL_H
#define CLOCK_K210_SYSCTL_H

/* Clocks as defined by kendryte-standalone-sdk/lib/drivers/include/sysctl.h */
#define K210_CLK_PLL0 0
#define K210_CLK_PLL1 1
#define K210_CLK_PLL2 2
#define K210_CLK_CPU 3
#define K210_CLK_SRAM0 4
#define K210_CLK_SRAM1 5
#define K210_CLK_APB0 6
#define K210_CLK_APB1 7
#define K210_CLK_APB2 8
#define K210_CLK_ROM 9
#define K210_CLK_DMA 10
#define K210_CLK_AI 11
#define K210_CLK_DVP 12
#define K210_CLK_FFT 13
#define K210_CLK_GPIO 14
#define K210_CLK_SPI0 15
#define K210_CLK_SPI1 16
#define K210_CLK_SPI2 17
#define K210_CLK_SPI3 18
#define K210_CLK_I2S0 19
#define K210_CLK_I2S1 20
#define K210_CLK_I2S2 21
#define K210_CLK_I2C0 22
#define K210_CLK_I2C1 23
#define K210_CLK_I2C2 24
#define K210_CLK_UART1 25
#define K210_CLK_UART2 26
#define K210_CLK_UART3 27
#define K210_CLK_AES 28
#define K210_CLK_FPIOA 29
#define K210_CLK_TIMER0 30
#define K210_CLK_TIMER1 31
#define K210_CLK_TIMER2 32
#define K210_CLK_WDT0 33
#define K210_CLK_WDT1 34
#define K210_CLK_SHA 35
#define K210_CLK_OTP 36
#define K210_CLK_RTC 37
#define K210_CLK_ACLK 40
#define K210_CLK_MAX 40

#define K210_CLK_HCLK 41 /* Unused */
#define K210_CLK_IN0 42 /* Defined elsewhere in the device tree */

#endif /* CLOCK_K210_SYSCTL_H */

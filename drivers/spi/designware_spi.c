// SPDX-License-Identifier: GPL-2.0
/*
 * Designware master SPI core controller driver
 *
 * Copyright (C) 2014 Stefan Roese <sr@denx.de>
 * Copyright (C) 2020 Sean Anderson <seanga2@gmail.com>
 *
 * Very loosely based on the Linux driver:
 * drivers/spi/spi-dw.c, which is:
 * Copyright (c) 2009, Intel Corporation.
 */

#define LOG_CATEGORY UCLASS_SPI
#include <common.h>
#include <log.h>
#include <asm-generic/gpio.h>
#include <clk.h>
#include <dm.h>
#include <errno.h>
#include <malloc.h>
#include <spi.h>
#include <fdtdec.h>
#include <reset.h>
#include <dm/device_compat.h>
#include <linux/bitops.h>
#include <linux/bitfield.h>
#include <linux/compat.h>
#include <linux/iopoll.h>
#include <asm/io.h>

/* Register offsets */
#define DW_SPI_CTRL0			0x00
#define DW_SPI_CTRL1			0x04
#define DW_SPI_SSIENR			0x08
#define DW_SPI_MWCR			0x0c
#define DW_SPI_SER			0x10
#define DW_SPI_BAUDR			0x14
#define DW_SPI_TXFLTR			0x18
#define DW_SPI_RXFLTR			0x1c
#define DW_SPI_TXFLR			0x20
#define DW_SPI_RXFLR			0x24
#define DW_SPI_SR			0x28
#define DW_SPI_IMR			0x2c
#define DW_SPI_ISR			0x30
#define DW_SPI_RISR			0x34
#define DW_SPI_TXOICR			0x38
#define DW_SPI_RXOICR			0x3c
#define DW_SPI_RXUICR			0x40
#define DW_SPI_MSTICR			0x44
#define DW_SPI_ICR			0x48
#define DW_SPI_DMACR			0x4c
#define DW_SPI_DMATDLR			0x50
#define DW_SPI_DMARDLR			0x54
#define DW_SPI_IDR			0x58
#define DW_SPI_VERSION			0x5c
#define DW_SPI_DR			0x60

/* Bit fields in CTRLR0 */
#define CTRLR0_DFS_MASK			GENMASK(3, 0)

#define CTRLR0_FRF_MASK			GENMASK(5, 4)
#define CTRLR0_FRF_SPI			0x0
#define CTRLR0_FRF_SSP			0x1
#define CTRLR0_FRF_MICROWIRE		0x2
#define CTRLR0_FRF_RESV			0x3

#define CTRLR0_MODE_MASK		GENMASK(7, 6)
#define CTRLR0_MODE_SCPH		0x1
#define CTRLR0_MODE_SCPOL		0x2

#define CTRLR0_TMOD_MASK		GENMASK(9, 8)
#define	CTRLR0_TMOD_TR			0x0		/* xmit & recv */
#define CTRLR0_TMOD_TO			0x1		/* xmit only */
#define CTRLR0_TMOD_RO			0x2		/* recv only */
#define CTRLR0_TMOD_EPROMREAD		0x3		/* eeprom read mode */

#define CTRLR0_SLVOE_OFFSET		10
#define CTRLR0_SRL_OFFSET		11
#define CTRLR0_CFS_MASK			GENMASK(15, 12)

/* The next two fields are only present on version 4.* */
#define CTRLR0_DFS_32_MASK		GENMASK(20, 16)

#define CTRLR0_SPI_FRF_MASK		GENMASK(22, 21)
#define CTRLR0_SPI_FRF_BYTE		0x0
#define	CTRLR0_SPI_FRF_DUAL		0x1
#define	CTRLR0_SPI_FRF_QUAD		0x2

/* Bit fields in CTRLR0 based on DWC_ssi_databook.pdf v1.01a */
#define DWC_SSI_CTRLR0_DFS_MASK		GENMASK(4, 0)
#define DWC_SSI_CTRLR0_FRF_MASK		GENMASK(7, 6)
#define DWC_SSI_CTRLR0_MODE_MASK	GENMASK(9, 8)
#define DWC_SSI_CTRLR0_TMOD_MASK	GENMASK(11, 10)
#define DWC_SSI_CTRLR0_SRL_OFFSET	13
#define DWC_SSI_CTRLR0_SPI_FRF_MASK	GENMASK(23, 22)

/* Bit fields in SR, 7 bits */
#define SR_MASK				GENMASK(6, 0)	/* cover 7 bits */
#define SR_BUSY				BIT(0)
#define SR_TF_NOT_FULL			BIT(1)
#define SR_TF_EMPT			BIT(2)
#define SR_RF_NOT_EMPT			BIT(3)
#define SR_RF_FULL			BIT(4)
#define SR_TX_ERR			BIT(5)
#define SR_DCOL				BIT(6)

/* Bit fields in VERSION */
#define VERSION_MAJOR_MASK		GENMASK(31, 24)
#define VERSION_MINOR_HIGH_MASK		GENMASK(23, 16)
#define VERSION_MINOR_LOW_MASK		GENMASK(15, 8)

#define RX_TIMEOUT			1000		/* timeout in ms */

struct dw_spi_platdata {
	s32 frequency;		/* Default clock frequency, -1 for none */
	void __iomem *regs;
};

struct dw_spi_priv {
	struct clk clk;
	struct reset_ctl_bulk resets;
	struct gpio_desc cs_gpio;	/* External chip-select gpio */

	u32 (*update_cr0)(struct dw_spi_priv *priv);

	void __iomem *regs;
	unsigned long bus_clk_rate;
	unsigned int freq;		/* Default frequency */
	unsigned int mode;

	const void *tx;
	const void *tx_end;
	void *rx;
	void *rx_end;
	u32 fifo_len;			/* depth of the FIFO buffer */

	int bits_per_word;
	int len;
	u8 cs;				/* chip select pin */
	u8 tmode;			/* TR/TO/RO/EEPROM */
	u8 type;			/* SPI/SSP/MicroWire */
};

static inline u32 dw_read(struct dw_spi_priv *priv, u32 offset)
{
	return __raw_readl(priv->regs + offset);
}

static inline void dw_write(struct dw_spi_priv *priv, u32 offset, u32 val)
{
	__raw_writel(val, priv->regs + offset);
}

static u32 dw_spi_dw16_update_cr0(struct dw_spi_priv *priv)
{
	return FIELD_PREP(CTRLR0_DFS_MASK, priv->bits_per_word - 1)
	     | FIELD_PREP(CTRLR0_FRF_MASK, priv->type)
	     | FIELD_PREP(CTRLR0_MODE_MASK, priv->mode)
	     | FIELD_PREP(CTRLR0_TMOD_MASK, priv->tmode);
}

static u32 dw_spi_dw32_update_cr0(struct dw_spi_priv *priv)
{
	return FIELD_PREP(CTRLR0_DFS_32_MASK, priv->bits_per_word - 1)
	     | FIELD_PREP(CTRLR0_FRF_MASK, priv->type)
	     | FIELD_PREP(CTRLR0_MODE_MASK, priv->mode)
	     | FIELD_PREP(CTRLR0_TMOD_MASK, priv->tmode);
}

static u32 dw_spi_dwc_update_cr0(struct dw_spi_priv *priv)
{
	return FIELD_PREP(DWC_SSI_CTRLR0_DFS_MASK, priv->bits_per_word - 1)
	     | FIELD_PREP(DWC_SSI_CTRLR0_FRF_MASK, priv->type)
	     | FIELD_PREP(DWC_SSI_CTRLR0_MODE_MASK, priv->mode)
	     | FIELD_PREP(DWC_SSI_CTRLR0_TMOD_MASK, priv->tmode);
}

static int dw_spi_dw16_init(struct udevice *bus, struct dw_spi_priv *priv)
{
	priv->update_cr0 = dw_spi_dw16_update_cr0;
	return 0;
}

static int dw_spi_dw32_init(struct udevice *bus, struct dw_spi_priv *priv)
{
	priv->update_cr0 = dw_spi_dw32_update_cr0;
	return 0;
}

static void dw_spi_print_version(struct dw_spi_priv *priv, u32 version,
				 char *driver)
{
	char *fmt;

	if (driver)
		fmt = "SPI@%p: Device version %c.%c%c%c matches driver version %s\n";
	else
		fmt = "SPI@%p: Device version %c.%c%c%c does not match any driver%.0s\n";

	log_info(fmt, priv->regs, version >> 24, version >> 16, version >> 8,
		 version, driver);
}

static int dw_spi_dw_generic_init(struct udevice *bus, struct dw_spi_priv *priv)
{
	u32 version;

	log_warning("SPI@%p: Compatible string did not specify device version.\n",
		    priv->regs);

	version = dw_read(priv, DW_SPI_VERSION);
	switch (FIELD_GET(VERSION_MAJOR_MASK, version)) {
	case '3':
		if (FIELD_GET(VERSION_MINOR_HIGH_MASK, version) < '2' ||
		    FIELD_GET(VERSION_MINOR_LOW_MASK, version) < '3') {
			dw_spi_print_version(priv, version, "<3.23");
			return dw_spi_dw16_init(bus, priv);
		}
	case '4':
		dw_spi_print_version(priv, version, ">=3.23");
		return dw_spi_dw32_init(bus, priv);
	default:
		dw_spi_print_version(priv, version, NULL);
		return -ENOTSUPP;
	}
}

static int dw_spi_dwc_init(struct udevice *bus, struct dw_spi_priv *priv)
{
	priv->update_cr0 = dw_spi_dwc_update_cr0;
	return 0;
}

static int request_gpio_cs(struct udevice *bus)
{
#if CONFIG_IS_ENABLED(DM_GPIO) && !defined(CONFIG_SPL_BUILD)
	struct dw_spi_priv *priv = dev_get_priv(bus);
	int ret;

	/* External chip select gpio line is optional */
	ret = gpio_request_by_name(bus, "cs-gpios", 0, &priv->cs_gpio,
				   GPIOD_IS_OUT | GPIOD_IS_OUT_ACTIVE);
	if (ret == -ENOENT)
		return 0;

	if (ret < 0) {
		log_err("SPI@%p: Couldn't request gpio! (error %d)\n",
			priv->regs, ret);
		return ret;
	}

	log_debug("Using external gpio for CS management\n");
#endif
	return 0;
}

static int dw_spi_ofdata_to_platdata(struct udevice *bus)
{
	struct dw_spi_platdata *plat = bus->platdata;

	plat->regs = dev_read_addr_ptr(bus);
	if (!plat->regs)
		return -EINVAL;

	/* Use 500KHz as a suitable default */
	plat->frequency = dev_read_u32_default(bus, "spi-max-frequency",
					       500000);

	if (dev_read_bool(bus, "spi-slave"))
		return -EINVAL;

	log_info("SPI@%p max-frequency=%d\n", plat->regs, plat->frequency);

	return request_gpio_cs(bus);
}

static inline void spi_enable_chip(struct dw_spi_priv *priv, int enable)
{
	dw_write(priv, DW_SPI_SSIENR, (enable ? 1 : 0));
}

/* Restart the controller, disable all interrupts, clean rx fifo */
static void spi_hw_init(struct dw_spi_priv *priv)
{
	spi_enable_chip(priv, 0);
	dw_write(priv, DW_SPI_IMR, 0xff);
	spi_enable_chip(priv, 1);

	/*
	 * Try to detect the FIFO depth if not set by interface driver,
	 * the depth could be from 2 to 256 from HW spec
	 */
	if (!priv->fifo_len) {
		u32 fifo;

		for (fifo = 1; fifo < 256; fifo++) {
			dw_write(priv, DW_SPI_TXFLTR, fifo);
			if (fifo != dw_read(priv, DW_SPI_TXFLTR))
				break;
		}

		priv->fifo_len = (fifo == 1) ? 0 : fifo;
		dw_write(priv, DW_SPI_TXFLTR, 0);
	}
	log_debug("fifo_len=%d\n", priv->fifo_len);
}

/*
 * We define dw_spi_get_clk function as 'weak' as some targets
 * (like SOCFPGA_GEN5 and SOCFPGA_ARRIA10) don't use standard clock API
 * and implement dw_spi_get_clk their own way in their clock manager.
 */
__weak int dw_spi_get_clk(struct udevice *bus, ulong *rate)
{
	struct dw_spi_priv *priv = dev_get_priv(bus);
	int ret;

	ret = clk_get_by_index(bus, 0, &priv->clk);
	if (ret)
		return ret;

	ret = clk_enable(&priv->clk);
	if (ret && ret != -ENOSYS && ret != -ENOTSUPP)
		return ret;

	*rate = clk_get_rate(&priv->clk);
	if (!*rate)
		goto err_rate;

	log_debug("Got clock via device tree: %lu Hz\n", *rate);

	return 0;

err_rate:
	clk_disable(&priv->clk);
	clk_free(&priv->clk);

	return -EINVAL;
}

static int dw_spi_reset(struct udevice *bus)
{
	int ret;
	struct dw_spi_priv *priv = dev_get_priv(bus);

	ret = reset_get_bulk(bus, &priv->resets);
	if (ret) {
		/*
		 * Return 0 if error due to !CONFIG_DM_RESET and reset
		 * DT property is not present.
		 */
		if (ret == -ENOENT || ret == -ENOTSUPP)
			return 0;

		log_warning("SPI@%p: Couldn't find/assert reset device (error %d)\n",
			    priv->regs, ret);
		return ret;
	}

	ret = reset_deassert_bulk(&priv->resets);
	if (ret) {
		reset_release_bulk(&priv->resets);
		log_err("SPI@%p: Failed to de-assert reset for SPI (error %d)\n",
			priv->regs, ret);
		return ret;
	}

	return 0;
}

typedef int (*dw_spi_init_t)(struct udevice *bus, struct dw_spi_priv *priv);

static int dw_spi_probe(struct udevice *bus)
{
	dw_spi_init_t init = (dw_spi_init_t)dev_get_driver_data(bus);
	struct dw_spi_platdata *plat = dev_get_platdata(bus);
	struct dw_spi_priv *priv = dev_get_priv(bus);
	int ret;

	priv->regs = plat->regs;
	priv->freq = plat->frequency;

	ret = dw_spi_get_clk(bus, &priv->bus_clk_rate);
	if (ret)
		return ret;

	ret = dw_spi_reset(bus);
	if (ret)
		return ret;

	if (!init)
		return -EINVAL;
	ret = init(bus, priv);
	if (ret)
		return ret;

	/* Currently only bits_per_word == 8 supported */
	priv->bits_per_word = 8;

	priv->tmode = 0; /* Tx & Rx */

	/* Basic HW init */
	spi_hw_init(priv);

	return 0;
}

/* Return the max entries we can fill into tx fifo */
static inline u32 tx_max(struct dw_spi_priv *priv)
{
	u32 tx_left, tx_room, rxtx_gap;

	tx_left = (priv->tx_end - priv->tx) / (priv->bits_per_word >> 3);
	tx_room = priv->fifo_len - dw_read(priv, DW_SPI_TXFLR);

	/*
	 * Another concern is about the tx/rx mismatch, we
	 * thought about using (priv->fifo_len - rxflr - txflr) as
	 * one maximum value for tx, but it doesn't cover the
	 * data which is out of tx/rx fifo and inside the
	 * shift registers. So a control from sw point of
	 * view is taken.
	 */
	rxtx_gap = ((priv->rx_end - priv->rx) - (priv->tx_end - priv->tx)) /
		(priv->bits_per_word >> 3);

	return min3(tx_left, tx_room, (u32)(priv->fifo_len - rxtx_gap));
}

/* Return the max entries we should read out of rx fifo */
static inline u32 rx_max(struct dw_spi_priv *priv)
{
	u32 rx_left = (priv->rx_end - priv->rx) / (priv->bits_per_word >> 3);

	return min_t(u32, rx_left, dw_read(priv, DW_SPI_RXFLR));
}

static void dw_writer(struct dw_spi_priv *priv)
{
	u32 max = tx_max(priv);
	u16 txw = 0;

	while (max--) {
		/* Set the tx word if the transfer's original "tx" is not null */
		if (priv->tx_end - priv->len) {
			if (priv->bits_per_word == 8)
				txw = *(u8 *)(priv->tx);
			else
				txw = *(u16 *)(priv->tx);
		}
		dw_write(priv, DW_SPI_DR, txw);
		log_content("tx=0x%02x\n", txw);
		priv->tx += priv->bits_per_word >> 3;
	}
}

static void dw_reader(struct dw_spi_priv *priv)
{
	u32 max = rx_max(priv);
	u16 rxw;

	while (max--) {
		rxw = dw_read(priv, DW_SPI_DR);
		log_content("rx=0x%02x\n", rxw);

		/* Care about rx if the transfer's original "rx" is not null */
		if (priv->rx_end - priv->len) {
			if (priv->bits_per_word == 8)
				*(u8 *)(priv->rx) = rxw;
			else
				*(u16 *)(priv->rx) = rxw;
		}
		priv->rx += priv->bits_per_word >> 3;
	}
}

static int poll_transfer(struct dw_spi_priv *priv)
{
	do {
		dw_writer(priv);
		dw_reader(priv);
	} while (priv->rx_end > priv->rx);

	return 0;
}

/*
 * We define external_cs_manage function as 'weak' as some targets
 * (like MSCC Ocelot) don't control the external CS pin using a GPIO
 * controller. These SoCs use specific registers to control by
 * software the SPI pins (and especially the CS).
 */
__weak void external_cs_manage(struct udevice *dev, bool on)
{
#if CONFIG_IS_ENABLED(DM_GPIO) && !defined(CONFIG_SPL_BUILD)
	struct dw_spi_priv *priv = dev_get_priv(dev->parent);

	if (!dm_gpio_is_valid(&priv->cs_gpio))
		return;

	dm_gpio_set_value(&priv->cs_gpio, on ? 1 : 0);
#endif
}

static int dw_spi_xfer(struct udevice *dev, unsigned int bitlen,
		       const void *dout, void *din, unsigned long flags)
{
	struct udevice *bus = dev->parent;
	struct dw_spi_priv *priv = dev_get_priv(bus);
	const u8 *tx = dout;
	u8 *rx = din;
	int ret = 0;
	u32 cr0 = 0;
	u32 val;
	u32 cs;

	/* spi core configured to do 8 bit transfers */
	if (bitlen % 8) {
		log_err("Non byte aligned SPI transfer.\n");
		return -1;
	}

	/* Start the transaction if necessary. */
	if (flags & SPI_XFER_BEGIN)
		external_cs_manage(dev, false);

	if (rx && tx)
		priv->tmode = CTRLR0_TMOD_TR;
	else if (rx)
		priv->tmode = CTRLR0_TMOD_RO;
	else
		/*
		 * In transmit only mode (CTRL0_TMOD_TO) input FIFO never gets
		 * any data which breaks our logic in poll_transfer() above.
		 */
		priv->tmode = CTRLR0_TMOD_TR;

	cr0 = priv->update_cr0(priv);

	priv->len = bitlen >> 3;
	log_debug("rx=%p tx=%p len=%d [bytes]\n", rx, tx, priv->len);

	priv->tx = (void *)tx;
	priv->tx_end = priv->tx + priv->len;
	priv->rx = rx;
	priv->rx_end = priv->rx + priv->len;

	/* Disable controller before writing control registers */
	spi_enable_chip(priv, 0);

	log_debug("cr0=%08x\n", cr0);
	/* Reprogram cr0 only if changed */
	if (dw_read(priv, DW_SPI_CTRL0) != cr0)
		dw_write(priv, DW_SPI_CTRL0, cr0);

	/*
	 * Configure the desired SS (slave select 0...3) in the controller
	 * The DW SPI controller will activate and deactivate this CS
	 * automatically. So no cs_activate() etc is needed in this driver.
	 */
	cs = spi_chip_select(dev);
	dw_write(priv, DW_SPI_SER, 1 << cs);

	/* Enable controller after writing control registers */
	spi_enable_chip(priv, 1);

	/* Start transfer in a polling loop */
	ret = poll_transfer(priv);

	/*
	 * Wait for current transmit operation to complete.
	 * Otherwise if some data still exists in Tx FIFO it can be
	 * silently flushed, i.e. dropped on disabling of the controller,
	 * which happens when writing 0 to DW_SPI_SSIENR which happens
	 * in the beginning of new transfer.
	 */
	if (readl_poll_timeout(priv->regs + DW_SPI_SR, val,
			       (val & SR_TF_EMPT) && !(val & SR_BUSY),
			       RX_TIMEOUT * 1000)) {
		ret = -ETIMEDOUT;
	}

	/* Stop the transaction if necessary */
	if (flags & SPI_XFER_END)
		external_cs_manage(dev, true);

	return ret;
}

static int dw_spi_set_speed(struct udevice *bus, uint speed)
{
	struct dw_spi_platdata *plat = dev_get_platdata(bus);
	struct dw_spi_priv *priv = dev_get_priv(bus);
	u16 clk_div;

	if (speed > plat->frequency)
		speed = plat->frequency;

	/* Disable controller before writing control registers */
	spi_enable_chip(priv, 0);

	/* clk_div doesn't support odd number */
	clk_div = priv->bus_clk_rate / speed;
	clk_div = (clk_div + 1) & 0xfffe;
	dw_write(priv, DW_SPI_BAUDR, clk_div);

	/* Enable controller after writing control registers */
	spi_enable_chip(priv, 1);

	priv->freq = speed;
	log_debug("SPI@%p speed=%d clk_div=%d\n", priv->regs, priv->freq,
		  clk_div);

	return 0;
}

static int dw_spi_set_mode(struct udevice *bus, uint mode)
{
	struct dw_spi_priv *priv = dev_get_priv(bus);

	/*
	 * Can't set mode yet. Since this depends on if rx, tx, or
	 * rx & tx is requested. So we have to defer this to the
	 * real transfer function.
	 */
	priv->mode = mode;
	log_debug("SPI@%p mode=%d\n", priv->regs, priv->mode);

	return 0;
}

static int dw_spi_remove(struct udevice *bus)
{
	struct dw_spi_priv *priv = dev_get_priv(bus);
	int ret;

	ret = reset_release_bulk(&priv->resets);
	if (ret)
		return ret;

#if CONFIG_IS_ENABLED(CLK)
	ret = clk_disable(&priv->clk);
	if (ret)
		return ret;

	ret = clk_free(&priv->clk);
	if (ret)
		return ret;
#endif
	return 0;
}

static const struct dm_spi_ops dw_spi_ops = {
	.xfer		= dw_spi_xfer,
	.set_speed	= dw_spi_set_speed,
	.set_mode	= dw_spi_set_mode,
	/*
	 * cs_info is not needed, since we require all chip selects to be
	 * in the device tree explicitly
	 */
};

static const struct udevice_id dw_spi_ids[] = {
	/* Generic compatible strings */

	{ .compatible = "snps,dw-apb-ssi", .data = (ulong)dw_spi_dw_generic_init },
	{ .compatible = "snps,dw-apb-ssi-3.20a", .data = (ulong)dw_spi_dw16_init },
	{ .compatible = "snps,dw-apb-ssi-3.22a", .data = (ulong)dw_spi_dw16_init },
	/*
	 * The exact version of the Intel Quark D20000 SPI device is documented
	 * as 3.23*, so a "patch" version is not included.
	 */
	{ .compatible = "snps,dw-apb-ssi-3.23", .data = (ulong)dw_spi_dw32_init },
	{ .compatible = "snps,dw-apb-ssi-4.00a", .data = (ulong)dw_spi_dw32_init },
	/*
	 * Similarly, the version of spi0-2 on the Canaan Kendryte K210 is
	 * undocumented, and was determined empherically to be 4.01*.
	 */
	{ .compatible = "snps,dw-apb-ssi-4.01", .data = (ulong)dw_spi_dw32_init },
	{ .compatible = "snps,dwc-ssi-1.01a", .data = (ulong)dw_spi_dwc_init },

	/* Compatible strings for specific SoCs */

	/*
	 * Both the Cyclone V and Arria V share a device tree and have the same
	 * version of this device. This compatible string is used for those
	 * devices, and is not used for sofpgas in general.
	 */
	{ .compatible = "altr,socfpga-spi", .data = (ulong)dw_spi_dw16_init },
	{ .compatible = "altr,socfpga-arria10-spi", .data = (ulong)dw_spi_dw16_init },
	{ .compatible = "canaan,kendryte-k210-spi", .data = (ulong)dw_spi_dw32_init },
	{ .compatible = "canaan,kendryte-k210-ssi", .data = (ulong)dw_spi_dwc_init },
	/* FIXME: next two should be dw_spi_dw32_init; but they need testing */
	{ .compatible = "intel,stratix10-spi", .data = (ulong)dw_spi_dw16_init },
	{ .compatible = "intel,agilex-spi", .data = (ulong)dw_spi_dw16_init },
	/* FIXME: is this the correct version for the next four? */
	{ .compatible = "mscc,ocelot-spi", .data = (ulong)dw_spi_dw16_init },
	{ .compatible = "mscc,jaguar2-spi", .data = (ulong)dw_spi_dw16_init },
	{ .compatible = "snps,axs10x-spi", .data = (ulong)dw_spi_dw16_init },
	{ .compatible = "snps,hsdk-spi", .data = (ulong)dw_spi_dw16_init },
	{ }
};

U_BOOT_DRIVER(dw_spi) = {
	.name = "dw_spi",
	.id = UCLASS_SPI,
	.of_match = dw_spi_ids,
	.ops = &dw_spi_ops,
	.ofdata_to_platdata = dw_spi_ofdata_to_platdata,
	.platdata_auto_alloc_size = sizeof(struct dw_spi_platdata),
	.priv_auto_alloc_size = sizeof(struct dw_spi_priv),
	.probe = dw_spi_probe,
	.remove = dw_spi_remove,
};

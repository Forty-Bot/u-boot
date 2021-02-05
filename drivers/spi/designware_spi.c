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
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>
#include <fdtdec.h>
#include <log.h>
#include <malloc.h>
#include <mux.h>
#include <reset.h>
#include <spi.h>
#include <spi-mem.h>
#include <asm/io.h>
#include <asm-generic/gpio.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/compat.h>
#include <linux/iopoll.h>
#include <linux/sizes.h>

/* Register offsets */
#define DW_SPI_CTRLR0			0x00
#define DW_SPI_CTRLR1			0x04
#define DW_SPI_SSIENR			0x08
#define DW_SPI_MWCR			0x0c
#define DW_SPI_SER			0x10
#define DW_SPI_BAUDR			0x14
#define DW_SPI_TXFTLR			0x18
#define DW_SPI_RXFTLR			0x1c
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
#define DW_SPI_RX_SAMPLE_DLY		0xf0
#define DW_SPI_SPI_CTRL0		0xf4
#define DW_SPI_XIP_MODE_BITS		0xfc
#define DW_SPI_XIP_INCR_INST		0x100
#define DW_SPI_XIP_WRAP_INST		0x104
#define DW_SPI_XIP_CTRL			0x108
#define DW_SPI_XIP_SER			0x10c
#define DW_SPI_XRXOICR			0x110
#define DW_SPI_XIP_XNT_TIME_OUT		0x114

/* Bit fields in CTRLR0 */
/*
 * Only present when SSI_MAX_XFER_SIZE=16. This is the default, and the only
 * option before version 3.23a.
 */
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

#define CTRLR0_SLVOE_OFFSET		BIT(10)
#define CTRLR0_SRL			BIT(11)
#define CTRLR0_CFS_MASK			GENMASK(15, 12)

/* Only present when SSI_MAX_XFER_SIZE=32 */
#define CTRLR0_DFS_32_MASK		GENMASK(20, 16)

/* The next field is only present on versions after 4.00a */
#define CTRLR0_SPI_FRF_MASK		GENMASK(22, 21)
#define CTRLR0_SPI_FRF_BYTE		0x0
#define	CTRLR0_SPI_FRF_DUAL		0x1
#define	CTRLR0_SPI_FRF_QUAD		0x2
#define	CTRLR0_SPI_FRF_OCTAL		0x3

/* Bit fields in CTRLR0 based on DWC_ssi_databook.pdf v1.01a */
#define DWC_SSI_CTRLR0_DFS_MASK		GENMASK(4, 0)
#define DWC_SSI_CTRLR0_FRF_MASK		GENMASK(7, 6)
#define DWC_SSI_CTRLR0_MODE_MASK	GENMASK(9, 8)
#define DWC_SSI_CTRLR0_TMOD_MASK	GENMASK(11, 10)
#define DWC_SSI_CTRLR0_SRL		BIT(13)
#define DWC_SSI_CTRLR0_SSTE		BIT(14)
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

/* Bit fields in (R)ISR */

/* TX FIFO Empty */
#define ISR_TXEI			BIT(0)
/* TX FIFO Overflow */
#define ISR_TXOI			BIT(1)
/* RX FIFO Underflow */
#define ISR_RXUI			BIT(2)
/* RX FIFO Overflow */
#define ISR_RXOI			BIT(3)
/* RX FIFO Full */
#define ISR_RXFI			BIT(4)
/* Multi-master contention */
#define ISR_MSTI			BIT(5)
/* XIP Receive FIFO Overflow */
#define ISR_XRXOI			BIT(6)
/* TX FIFO Underflow */
#define ISR_TXUI			BIT(7)
/* AXI Error */
#define ISR_AXIE			BIT(8)
/* SPI TX Error */
#define ISR_SPITE			BIT(10)
/* SSI Done */
#define ISR_DONE			BIT(11)

/* Bit fields in SPI_CTRLR0 */

/*
 * Whether the instruction or address use the value of SPI_FRF or use
 * FRF_BYTE
 */
#define SPI_CTRLR0_TRANS_TYPE_MASK	GENMASK(1, 0)
#define TRANS_TYPE_1_1_X		0x0
#define TRANS_TYPE_1_X_X		0x1
#define TRANS_TYPE_X_X_X		0x2
/* Address length in 4-bit units */
#define SPI_CTRLR0_ADDR_L_MASK		GENMASK(5, 2)
/* Enable mode bits after address in XIP mode */
#define SPI_CTRLR0_XIP_MD_BIT_EN	BIT(7)
/* Instruction length */
#define SPI_CTRLR0_INST_L_MASK		GENMASK(9, 8)
#define INST_L_0			0x0
#define INST_L_4			0x1
#define INST_L_8			0x2
#define INST_L_16			0x3
/* Number of "dummy" cycles */
#define SPI_CTRLR0_WAIT_CYCLES_MASK	GENMASK(15, 11)
/* Stretch the clock if the FIFO over/underflows */
#define SPI_CTRLR0_CLK_STRETCH_EN	BIT(30)

/* Bit fields in XIP_CTRL */

/* XIP SPI frame format */
#define XIP_CTRL_FRF			GENMASK(1, 0)
/* Same as SPI_CTRLR0_TRANS_TYPE */
#define XIP_CTRL_TRANS_TYPE_MASK	GENMASK(3, 2)
/* Address length in 4-bit increments */
#define XIP_CTRL_ADDR_L_MASK		GENMASK(7, 4)
/* Same as SPI_CTRLR0_INST_L */
#define XIP_CTRL_INST_L_MASK		GENMASK(10, 9)
/* Enable mode bits */
#define XIP_CTRL_MD_BITS_EN		BIT(12)
/* Wait cycles */
#define XIP_CTRL_WAIT_CYCLES_MASK	GENMASK(17, 13)
/* Use fixed-size DFS in XIP mode, ignoring AHB request width */
#define XIP_CTRL_DFS_HC			BIT(18)
/* Enable instruction phase */
#define XIP_CTRL_INST_EN		BIT(22)
/* Continuous transfer: don't deselect slave after one transfer */
#define XIP_CTRL_CONT_XFER_EN		BIT(23)
/* Mode bits length; length = 1 << (MBL + 1) */
#define XIP_CTRL_XIP_MBL_MASK		GENMASK(27, 26)
/* Prefetch contiguous data frames */
#define XIP_CTRL_PREFETCH_EN		BIT(29)

#define RX_TIMEOUT			1000		/* timeout in ms */

struct dw_spi_plat {
	s32 frequency;		/* Default clock frequency, -1 for none */
	void __iomem *regs;
	fdt_size_t regs_size;
};

struct dw_spi_priv {
	struct clk clk;
	struct reset_ctl_bulk resets;
	struct gpio_desc cs_gpio;	/* External chip-select gpio */
	struct mux_control *mux;	/* XIP mode mux */

	void __iomem *regs;
	fdt_size_t regs_size;
/* DW SPI capabilities */
#define DW_SPI_CAP_CS_OVERRIDE		BIT(0) /* Unimplemented */
#define DW_SPI_CAP_KEEMBAY_MST		BIT(1) /* Unimplemented */
#define DW_SPI_CAP_DWC_SSI		BIT(2)
#define DW_SPI_CAP_DFS32		BIT(3)
#define DW_SPI_CAP_ENHANCED		BIT(4)
#define DW_SPI_CAP_XIP			BIT(5)
#define DW_SPI_CAP_XIP_CONCURRENT	BIT(6)
	unsigned long caps;
	unsigned long bus_clk_rate;
	unsigned int freq;		/* Default frequency */
	unsigned int mode;

	u32 mux_xip_state;		/* Mux state to enable XIP mode */
	u32 fifo_len;			/* depth of the FIFO buffer */

	int bits_per_word;
	int frames;			/* Number of frames in the transfer */
	u8 cs;				/* chip select pin */
	u8 tmode;			/* TR/TO/RO/EEPROM */
	u8 type;			/* SPI/SSP/MicroWire */
	u8 spi_frf;			/* BYTE/DUAL/QUAD/OCTAL */
};

static inline u32 dw_read(struct dw_spi_priv *priv, u32 offset)
{
	return __raw_readl(priv->regs + offset);
}

static inline void dw_write(struct dw_spi_priv *priv, u32 offset, u32 val)
{
	__raw_writel(val, priv->regs + offset);
}

static u32 dw_spi_update_cr0(struct dw_spi_priv *priv)
{
	u32 cr0;

	if (priv->caps & DW_SPI_CAP_DWC_SSI) {
		cr0 = FIELD_PREP(DWC_SSI_CTRLR0_DFS_MASK,
				 priv->bits_per_word - 1)
		    | FIELD_PREP(DWC_SSI_CTRLR0_FRF_MASK, priv->type)
		    | FIELD_PREP(DWC_SSI_CTRLR0_MODE_MASK, priv->mode)
		    | FIELD_PREP(DWC_SSI_CTRLR0_TMOD_MASK, priv->tmode)
		    | FIELD_PREP(DWC_SSI_CTRLR0_SPI_FRF_MASK, priv->spi_frf);
	} else {
		if (priv->caps & DW_SPI_CAP_DFS32)
			cr0 = FIELD_PREP(CTRLR0_DFS_32_MASK,
					 priv->bits_per_word - 1);
		else
			cr0 = FIELD_PREP(CTRLR0_DFS_MASK,
					 priv->bits_per_word - 1);

		cr0 |= FIELD_PREP(CTRLR0_FRF_MASK, priv->type)
		    |  FIELD_PREP(CTRLR0_MODE_MASK, priv->mode)
		    |  FIELD_PREP(CTRLR0_TMOD_MASK, priv->tmode)
		    |  FIELD_PREP(CTRLR0_SPI_FRF_MASK, priv->spi_frf);
	}

	return cr0;
}

static u32 dw_spi_update_spi_cr0(const struct spi_mem_op *op)
{
	uint trans_type, wait_cycles;

	/* This assumes support_op has filtered invalid types */
	if (op->addr.buswidth == 1)
		trans_type = TRANS_TYPE_1_1_X;
	else if (op->cmd.buswidth == 1)
		trans_type = TRANS_TYPE_1_X_X;
	else
		trans_type = TRANS_TYPE_X_X_X;

	if (op->dummy.buswidth)
		wait_cycles = op->dummy.nbytes * 8 / op->dummy.buswidth;
	else
		wait_cycles = 0;

	return FIELD_PREP(SPI_CTRLR0_TRANS_TYPE_MASK, trans_type)
	       | FIELD_PREP(SPI_CTRLR0_ADDR_L_MASK, op->addr.nbytes * 2)
	       | FIELD_PREP(SPI_CTRLR0_INST_L_MASK, INST_L_8)
	       | FIELD_PREP(SPI_CTRLR0_WAIT_CYCLES_MASK, wait_cycles)
	       | SPI_CTRLR0_CLK_STRETCH_EN;
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
		dev_err(bus, "Couldn't request gpio! (error %d)\n", ret);
		return ret;
	}

	if (dm_gpio_is_valid(&priv->cs_gpio)) {
		dm_gpio_set_dir_flags(&priv->cs_gpio,
				      GPIOD_IS_OUT | GPIOD_IS_OUT_ACTIVE);
	}

	dev_dbg(bus, "Using external gpio for CS management\n");
#endif
	return 0;
}

static int dw_spi_of_to_plat(struct udevice *bus)
{
	fdt_addr_t regs;
	struct dw_spi_plat *plat = dev_get_plat(bus);

	regs = dev_read_addr_size_index(bus, 0, &plat->regs_size);
	if (regs == FDT_ADDR_T_NONE)
		return -EINVAL;
	plat->regs = (void *)regs;

	/* Use 500KHz as a suitable default */
	plat->frequency = dev_read_u32_default(bus, "spi-max-frequency",
					       500000);

	if (dev_read_bool(bus, "spi-slave"))
		return -EINVAL;

	dev_info(bus, "max-frequency=%d\n", plat->frequency);

	return request_gpio_cs(bus);
}

static int dw_spi_mux(struct udevice *dev, bool xip)
{
	struct dw_spi_priv *priv = dev_get_priv(dev);

	if (!priv->mux)
		return 0;

	if (xip && priv->mux_xip_state)
		return mux_control_select(priv->mux, priv->mux_xip_state);
	else
		return mux_control_select(priv->mux, 0);
}

/*
 * dw_spi_mux_regs() - Select the control registers using the XIP mux
 * @dev: The device to mux
 *
 * This selects the control registers using the XIP mux, driving the xip_en
 * signal low. This function must be called before any accesses to control
 * registers.
 *
 * Return: 0 on success or negative error value
 */
static inline int dw_spi_mux_regs(struct udevice *dev)
{
	return dw_spi_mux(dev, false);
}

/*
 * dw_spi_mux_xip() - Select the control registers using the XIP mux
 * @dev: The device to mux
 *
 * This selects XIP mode using the XIP mux, driving the xip_en signal high. This
 * function must be called before any XIP accesses.
 *
 * Return: 0 on success or negative error value
 */
static inline int dw_spi_mux_xip(struct udevice *dev)
{
	return dw_spi_mux(dev, true);
}

/*
 * dw_spi_mux_deselect()
 * @dev: The device to mux
 *
 * This deselects the XIP mux, returning it to its default state. This must be
 * called after control register or XIP accesses are finished, before other
 * calls to @dw_spi_mux_regs or @dw_spi_mux_xip.
 */
static void dw_spi_mux_deselect(struct udevice *dev)
{
	int err;
	struct dw_spi_priv *priv = dev_get_priv(dev);

	if (!priv->mux)
		return;

	err = mux_control_deselect(priv->mux);
	if (err)
		dev_warn(dev, "could not deselect mux (err %d)\n", err);
}

/* Restart the controller, disable all interrupts, clean rx fifo */
static int spi_hw_init(struct udevice *bus, struct dw_spi_priv *priv)
{
	int ret;
	u32 cr0;

	ret = dw_spi_mux_regs(bus);
	if (ret)
		return ret;

	dw_write(priv, DW_SPI_SSIENR, 0);
	dw_write(priv, DW_SPI_IMR, 0);

	/*
	 * Detect features by writing CTRLR0 and seeing which fields remain
	 * zeroed.
	 */
	dw_write(priv, DW_SPI_SSIENR, 0);
	dw_write(priv, DW_SPI_CTRLR0, 0xffffffff);
	cr0 = dw_read(priv, DW_SPI_CTRLR0);

	/*
	 * DWC_SPI always has DFS_32. If we read zeros from DFS, then we need to
	 * use DFS_32 instead
	 */
	if (priv->caps & DW_SPI_CAP_DWC_SSI || !FIELD_GET(CTRLR0_DFS_MASK, cr0))
		priv->caps |= DW_SPI_CAP_DFS32;

	/*
	 * If SPI_FRF exists that means we have DUAL, QUAD, or OCTAL. Since we
	 * can't differentiate, just set a general ENHANCED cap and let the
	 * slave decide what to use.
	 */
	if (priv->caps & DW_SPI_CAP_DWC_SSI) {
		if (FIELD_GET(DWC_SSI_CTRLR0_SPI_FRF_MASK, cr0))
			priv->caps |= DW_SPI_CAP_ENHANCED;
	} else if (FIELD_GET(CTRLR0_SPI_FRF_MASK, cr0)) {
		priv->caps |= DW_SPI_CAP_ENHANCED;
	}

	/*
	 * DWC_SPI always has this register with SSIC_XIP_EN. There is no way
	 * to detect XIP for DW APB SSI
	 */
	dw_write(priv, DW_SPI_XIP_INCR_INST, 0xffffffff);
	if (dw_read(priv, DW_SPI_XIP_INCR_INST))
		priv->caps |= DW_SPI_CAP_XIP;

	/* Exists with SSIC_CONCURRENT_XIP_EN */
	dw_write(priv, DW_SPI_XIP_CTRL, 0xffffffff);
	if (dw_read(priv, DW_SPI_XIP_CTRL))
		priv->caps |= DW_SPI_CAP_XIP_CONCURRENT;

	dw_write(priv, DW_SPI_SSIENR, 1);

	/*
	 * Try to detect the FIFO depth if not set by interface driver,
	 * the depth could be from 2 to 256 from HW spec
	 */
	if (!priv->fifo_len) {
		u32 fifo;

		for (fifo = 1; fifo < 256; fifo++) {
			dw_write(priv, DW_SPI_TXFTLR, fifo);
			if (fifo != dw_read(priv, DW_SPI_TXFTLR))
				break;
		}

		priv->fifo_len = (fifo == 1) ? 0 : fifo;
		dw_write(priv, DW_SPI_TXFTLR, 0);
	}

	/* Set receive fifo interrupt level register for clock stretching */
	dw_write(priv, DW_SPI_RXFTLR, priv->fifo_len - 1);

	dw_spi_mux_deselect(bus);
	return 0;
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

	dev_dbg(bus, "Got clock via device tree: %lu Hz\n", *rate);

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

		dev_warn(bus, "Couldn't find/assert reset device (error %d)\n",
			 ret);
		return ret;
	}

	ret = reset_deassert_bulk(&priv->resets);
	if (ret) {
		reset_release_bulk(&priv->resets);
		dev_err(bus, "Failed to de-assert reset for SPI (error %d)\n",
			ret);
		return ret;
	}

	return 0;
}

int dw_spi_get_mux(struct udevice *bus)
{
	int ret;
	struct dw_spi_priv *priv = dev_get_priv(bus);

	ret = mux_get_by_index(bus, 0, &priv->mux);
	if (ret) {
		/*
		 * Return 0 if error due to !CONFIG_MUX or mux
		 * DT property is not present.
		 */
		if (ret == -ENOENT || ret == -ENOTSUPP)
			return 0;

		dev_warn(bus, "Couldn't get xip mux (error %d)\n", ret);
		return ret;
	}

	ret = dev_read_u32(bus, "mux-xip-state", &priv->mux_xip_state);
	if (ret || priv->mux_xip_state > 1) {
		dev_warn(bus, "Invalid/missing mux-xip-state property\n");
		return -EINVAL;
	}

	return 0;
}

static int dw_spi_probe(struct udevice *bus)
{
	struct dw_spi_plat *plat = dev_get_plat(bus);
	struct dw_spi_priv *priv = dev_get_priv(bus);
	int ret;
	u32 version;

	priv->regs = plat->regs;
	priv->regs_size = plat->regs_size;
	priv->freq = plat->frequency;

	ret = dw_spi_get_clk(bus, &priv->bus_clk_rate);
	if (ret)
		return ret;

	ret = dw_spi_reset(bus);
	if (ret)
		return ret;

	ret = dw_spi_get_mux(bus);
	if (ret)
		return ret;

	/* Currently only bits_per_word == 8 supported */
	priv->bits_per_word = 8;

	priv->tmode = 0; /* Tx & Rx */

	/* Basic HW init */
	priv->caps = dev_get_driver_data(bus);
	ret = spi_hw_init(bus, priv);
	if (ret)
		return ret;

	if (!priv->mux)
		priv->caps &= DW_SPI_CAP_XIP;

	version = dw_read(priv, DW_SPI_VERSION);
	dev_dbg(bus,
		"ssi_version_id=%c.%c%c%c ssi_rx_fifo_depth=%u ssi_max_xfer_size=%u\n",
		version >> 24, version >> 16, version >> 8, version,
		priv->fifo_len, priv->caps & DW_SPI_CAP_DFS32 ? 32 : 16);

	return 0;
}

/**
 * dw_writer() - Write data frames to the tx fifo
 * @priv: Driver private info
 * @tx: The tx buffer
 * @idx: The number of data frames already transmitted
 * @tx_frames: The number of data frames left to transmit
 * @rx_frames: The number of data frames left to receive (0 if only
 *             transmitting)
 * @frame_bytes: The number of bytes taken up by one data frame
 *
 * This function writes up to @tx_frames data frames using data from @tx[@idx].
 *
 * Return: The number of frames read
 */
static uint dw_writer(struct dw_spi_priv *priv, const void *tx, uint idx,
		      uint tx_frames, uint rx_frames, uint frame_bytes)
{
	u32 tx_room = priv->fifo_len - dw_read(priv, DW_SPI_TXFLR);
	/*
	 * Another concern is about the tx/rx mismatch, we
	 * thought about using (priv->fifo_len - rxflr - txflr) as
	 * one maximum value for tx, but it doesn't cover the
	 * data which is out of tx/rx fifo and inside the
	 * shift registers. So a control from sw point of
	 * view is taken.
	 */
	u32 rxtx_gap = rx_frames - tx_frames;
	u32 count = min3(tx_frames, tx_room, (u32)(priv->fifo_len - rxtx_gap));
	u32 *dr = priv->regs + DW_SPI_DR;

	if (!count)
		return 0;

#define do_write(type) do { \
	type *start = ((type *)tx) + idx; \
	type *end = start + count; \
	do { \
		__raw_writel(*start++, dr); \
	} while (start < end); \
} while (0)
	switch (frame_bytes) {
	case 1:
		do_write(u8);
		break;
	case 2:
		do_write(u16);
		break;
	case 3:
	case 4:
	default:
		do_write(u32);
		break;
	}
#undef do_write

	return count;
}

/**
 * dw_reader() - Read data frames from the rx fifo
 * @priv: Driver private data
 * @rx: The rx buffer
 * @idx: The number of data frames already received
 * @frames: The number of data frames left to receive
 * @frame_bytes: The size of a data frame in bytes
 *
 * This function reads up to @frames data frames from @rx[@idx].
 *
 * Return: The number of frames read
 */
static uint dw_reader(struct dw_spi_priv *priv, void *rx, uint idx, uint frames,
		      uint frame_bytes)
{
	u32 rx_lvl = dw_read(priv, DW_SPI_RXFLR);
	u32 count = min(frames, rx_lvl);
	u32 *dr = priv->regs + DW_SPI_DR;

	if (!count)
		return 0;

#define do_read(type) do { \
	type *start = ((type *)rx) + idx; \
	type *end = start + count; \
	do { \
		*start++ = __raw_readl(dr); \
	} while (start < end); \
} while (0)
	switch (frame_bytes) {
	case 1:
		do_read(u8);
		break;
	case 2:
		do_read(u16);
		break;
	case 3:
	case 4:
	default:
		do_read(u32);
		break;
	}
#undef do_read

	return count;
}

/**
 * poll_transfer() - Transmit and receive data frames
 * @priv: Driver private data
 * @tx: The tx buffer. May be %NULL to only receive.
 * @rx: The rx buffer. May be %NULL to discard read data.
 * @frames: The number of data frames to transfer
 *
 * Transmit @tx, while recieving @rx.
 *
 * Return: The lesser of the number of frames transmitted or received.
 */
static uint poll_transfer(struct dw_spi_priv *priv, const void *tx, void *rx,
			  uint frames)
{
	uint frame_bytes = priv->bits_per_word >> 3;
	uint tx_idx = 0;
	uint rx_idx = 0;
	uint tx_frames = tx ? frames : 0;
	uint rx_frames = rx ? frames : 0;

	while (tx_frames || rx_frames) {
		if (tx_frames) {
			uint tx_diff = dw_writer(priv, tx, tx_idx, tx_frames,
						 rx_frames, frame_bytes);

			tx_idx += tx_diff;
			tx_frames -= tx_diff;
		}

		if (rx_frames) {
			uint rx_diff = dw_reader(priv, rx, rx_idx, rx_frames,
						 frame_bytes);

			rx_idx += rx_diff;
			rx_frames -= rx_diff;
		}

		/*
		 * If we don't read/write fast enough, the transfer stops.
		 * Don't bother reading out what's left in the FIFO; it's
		 * garbage.
		 */
		if (dw_read(priv, DW_SPI_RISR) & (ISR_RXOI | ISR_TXUI))
			break;
	}
	return min(tx ? tx_idx : rx_idx, rx ? rx_idx : tx_idx);
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
	const void *tx = dout;
	u8 *rx = din;
	int ret = 0;
	u32 cr0 = 0;
	u32 val, cs;
	uint frames;

	/* DUAL/QUAD/OCTAL only supported by exec_op for now */
	if (priv->mode & (SPI_TX_DUAL | SPI_TX_QUAD | SPI_TX_OCTAL |
			  SPI_RX_DUAL | SPI_RX_QUAD | SPI_RX_OCTAL))
		return -1;

	priv->spi_frf = CTRLR0_SPI_FRF_BYTE;

	/* spi core configured to do 8 bit transfers */
	if (bitlen % priv->bits_per_word) {
		dev_err(dev, "Non byte aligned SPI transfer.\n");
		return -1;
	}

	ret = dw_spi_mux_regs(bus);
	if (ret)
		return ret;

	frames = bitlen / priv->bits_per_word;

	/* Start the transaction if necessary. */
	if (flags & SPI_XFER_BEGIN)
		external_cs_manage(dev, false);

	if (rx && tx)
		priv->tmode = CTRLR0_TMOD_TR;
	else if (rx)
		priv->tmode = CTRLR0_TMOD_RO;
	else
		priv->tmode = CTRLR0_TMOD_TO;

	cr0 = dw_spi_update_cr0(priv);

	/* Disable controller before writing control registers */
	dw_write(priv, DW_SPI_SSIENR, 0);

	dev_dbg(dev, "cr0=%08x rx=%p tx=%p frames=%d\n", cr0, rx, tx,
		frames);
	/* Reprogram cr0 only if changed */
	if (dw_read(priv, DW_SPI_CTRLR0) != cr0)
		dw_write(priv, DW_SPI_CTRLR0, cr0);

	if (rx)
		dw_write(priv, DW_SPI_CTRLR1, frames - 1);

	/*
	 * Configure the desired SS (slave select 0...3) in the controller
	 * The DW SPI controller will activate and deactivate this CS
	 * automatically. So no cs_activate() etc is needed in this driver.
	 */
	cs = spi_chip_select(dev);
	dw_write(priv, DW_SPI_SER, 1 << cs);

	/* Enable controller after writing control registers */
	dw_write(priv, DW_SPI_SSIENR, 1);

	/*
	 * Prime the pump. RO-mode doesn't work unless something gets written to
	 * the FIFO
	 */
	if (rx && !tx)
		dw_write(priv, DW_SPI_DR, 0xFFFFFFFF);

	/* Start transfer in a polling loop */
	poll_transfer(priv, tx, rx, frames);

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
		dev_dbg(bus, "timed out; sr=%x\n", dw_read(priv, DW_SPI_SR));
		ret = -ETIMEDOUT;
	}

	/* Stop the transaction if necessary */
	if (flags & SPI_XFER_END)
		external_cs_manage(dev, true);

	dw_spi_mux_deselect(bus);

	return ret;
}

/*
 * This function is necessary for reading SPI flash with the native CS
 * c.f. https://lkml.org/lkml/2015/12/23/132
 *
 * It also lets us handle DUAL/QUAD/OCTAL transfers in a much more idiomatic
 * way.
 */
static int dw_spi_exec_op(struct spi_slave *slave, const struct spi_mem_op *op)
{
	bool read = op->data.dir == SPI_MEM_DATA_IN;
	int pos, i, ret = 0;
	struct udevice *bus = slave->dev->parent;
	struct dw_spi_priv *priv = dev_get_priv(bus);
	struct spi_mem_op *mut_op = (struct spi_mem_op *)op;
	u8 op_len = sizeof(op->cmd.opcode) + op->addr.nbytes + op->dummy.nbytes;
	u8 op_buf[op_len];
	u32 cr0, spi_cr0, val;

	/* Only bytes are supported for spi-mem transfers */
	if (priv->bits_per_word != 8)
		return -EINVAL;

	switch (op->data.buswidth) {
	case 0:
	case 1:
		priv->spi_frf = CTRLR0_SPI_FRF_BYTE;
		break;
	case 2:
		priv->spi_frf = CTRLR0_SPI_FRF_DUAL;
		break;
	case 4:
		priv->spi_frf = CTRLR0_SPI_FRF_QUAD;
		break;
	case 8:
		priv->spi_frf = CTRLR0_SPI_FRF_OCTAL;
		break;
	/* BUG: should have been filtered out by supports_op */
	default:
		return -EINVAL;
	}

	if (read)
		if (priv->spi_frf == CTRLR0_SPI_FRF_BYTE)
			priv->tmode = CTRLR0_TMOD_EPROMREAD;
		else
			priv->tmode = CTRLR0_TMOD_RO;
	else
		priv->tmode = CTRLR0_TMOD_TO;

	ret = dw_spi_mux_regs(bus);
	if (ret)
		return ret;

	cr0 = dw_spi_update_cr0(priv);
	spi_cr0 = dw_spi_update_spi_cr0(op);
	dev_dbg(bus, "cr0=%08x spi_cr0=%08x buf=%p len=%u [bytes]\n", cr0,
		spi_cr0, op->data.buf.in, op->data.nbytes);

	dw_write(priv, DW_SPI_SSIENR, 0);
	dw_write(priv, DW_SPI_CTRLR0, cr0);
	dw_write(priv, DW_SPI_CTRLR1, op->data.nbytes - 1);
	if (priv->spi_frf != CTRLR0_SPI_FRF_BYTE)
		dw_write(priv, DW_SPI_SPI_CTRL0, spi_cr0);
	dw_write(priv, DW_SPI_SSIENR, 1);

	/* Write out the instruction */
	if (priv->spi_frf == CTRLR0_SPI_FRF_BYTE) {
		/* From spi_mem_exec_op */
		pos = 0;
		op_buf[pos++] = op->cmd.opcode;
		if (op->addr.nbytes) {
			for (i = 0; i < op->addr.nbytes; i++)
				op_buf[pos + i] = op->addr.val >>
					(8 * (op->addr.nbytes - i - 1));

			pos += op->addr.nbytes;
		}
		memset(op_buf + pos, 0xff, op->dummy.nbytes);

		dw_writer(priv, &op_buf, 0, op_len, 0, sizeof(u8));
	} else {
		/* MUST be written as byte/long; don't ask me why */
		writeb(op->cmd.opcode, priv->regs + DW_SPI_DR);
		writel(op->addr.val, priv->regs + DW_SPI_DR);
	}

	external_cs_manage(slave->dev, false);
	dw_write(priv, DW_SPI_SER, 1 << spi_chip_select(slave->dev));

	/*
	 * XXX: The following are tight loops! Enabling debug messages may cause
	 * them to fail because we are not reading/writing the fifo fast enough.
	 */
	if (read)
		mut_op->data.nbytes = poll_transfer(priv, NULL, op->data.buf.in,
						    op->data.nbytes);
	else
		mut_op->data.nbytes = poll_transfer(priv, op->data.buf.out,
						    NULL, op->data.nbytes);

	/*
	 * Ensure the data (or the instruction for zero-data instructions) has
	 * been transmitted from the fifo/shift register before disabling the
	 * device.
	 */
	if (readl_poll_timeout(priv->regs + DW_SPI_SR, val,
			       (val & SR_TF_EMPT) && !(val & SR_BUSY),
			       RX_TIMEOUT * 1000)) {
		dev_dbg(bus, "timed out; sr=%x\n", dw_read(priv, DW_SPI_SR));
		ret = -ETIMEDOUT;
	}
	dw_write(priv, DW_SPI_SER, 0);
	external_cs_manage(slave->dev, true);
	dw_spi_mux_deselect(bus);

	dev_dbg(bus, "%u bytes xfered\n", op->data.nbytes);
	return ret;
}

bool dw_spi_supports_op(struct spi_slave *slave, const struct spi_mem_op *op)
{
	struct dw_spi_priv *priv = dev_get_priv(slave->dev->parent);

	if (!spi_mem_default_supports_op(slave, op))
		return false;

	/*
	 * Everything before the data must fit in the fifo.
	 * In EEPROM mode we also need to fit the dummy.
	 */
	if (1 + op->addr.nbytes +
	    (op->data.buswidth == 1 ? op->dummy.nbytes : 0) > priv->fifo_len)
		return false;

	/* We only support 1_1_X, 1_X_X, and X_X_X formats */
	if (op->cmd.buswidth == 1 &&
	    (!op->addr.nbytes || op->addr.buswidth == 1))
		return true;

	if (op->cmd.buswidth == 1 &&
	    (!op->addr.nbytes || op->addr.buswidth == op->data.buswidth))
		return true;

	if (op->cmd.buswidth == op->data.buswidth &&
	    (!op->addr.nbytes || op->addr.buswidth == op->data.buswidth))
		return true;

	return false;
}

/* The size of ctrl1 limits data transfers to 64K */
static int dw_spi_adjust_op_size(struct spi_slave *slave, struct spi_mem_op *op)
{
	op->data.nbytes = min(op->data.nbytes, (unsigned int)SZ_64K);

	return 0;
}

static const struct spi_controller_mem_ops dw_spi_mem_ops = {
	.exec_op = dw_spi_exec_op,
	.supports_op = dw_spi_supports_op,
	.adjust_op_size = dw_spi_adjust_op_size,
};

static int dw_spi_set_speed(struct udevice *bus, uint speed)
{
	int ret;
	struct dw_spi_plat *plat = dev_get_plat(bus);
	struct dw_spi_priv *priv = dev_get_priv(bus);
	u16 clk_div;

	ret = dw_spi_mux_regs(bus);
	if (ret)
		return ret;

	if (speed > plat->frequency)
		speed = plat->frequency;

	/* Disable controller before writing control registers */
	dw_write(priv, DW_SPI_SSIENR, 0);

	/* clk_div doesn't support odd number */
	clk_div = priv->bus_clk_rate / speed;
	clk_div = (clk_div + 1) & 0xfffe;
	dw_write(priv, DW_SPI_BAUDR, clk_div);

	/* Enable controller after writing control registers */
	dw_write(priv, DW_SPI_SSIENR, 1);
	mux_control_deselect(priv->mux);

	priv->freq = speed;
	dev_dbg(bus, "speed=%d clk_div=%d\n", priv->freq, clk_div);

	return 0;
}

static int dw_spi_set_mode(struct udevice *bus, uint mode)
{
	struct dw_spi_priv *priv = dev_get_priv(bus);

	if (!(priv->caps & DW_SPI_CAP_ENHANCED) &&
	    (mode & (SPI_RX_DUAL | SPI_TX_DUAL |
		     SPI_RX_QUAD | SPI_TX_QUAD |
		     SPI_RX_OCTAL | SPI_TX_OCTAL)))
		return -EINVAL;

	/*
	 * Can't set mode yet. Since this depends on if rx, tx, or
	 * rx & tx is requested. So we have to defer this to the
	 * real transfer function.
	 */
	priv->mode = mode;
	dev_dbg(bus, "mode=%x\n", mode);

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
	.mem_ops	= &dw_spi_mem_ops,
	.set_speed	= dw_spi_set_speed,
	.set_mode	= dw_spi_set_mode,
	/*
	 * cs_info is not needed, since we require all chip selects to be
	 * in the device tree explicitly
	 */
};

static const struct udevice_id dw_spi_ids[] = {
	/* Generic compatible strings */

	{ .compatible = "snps,dw-apb-ssi" },
	{ .compatible = "snps,dw-apb-ssi-3.20a" },
	{ .compatible = "snps,dw-apb-ssi-3.22a" },
	/* First version with SSI_MAX_XFER_SIZE */
	{ .compatible = "snps,dw-apb-ssi-3.23a" },
	/* First version with Dual/Quad SPI */
	{ .compatible = "snps,dw-apb-ssi-4.00a" },
	{ .compatible = "snps,dw-apb-ssi-4.01" },
	{ .compatible = "snps,dwc-ssi-1.01a", .data = DW_SPI_CAP_DWC_SSI },

	/* Compatible strings for specific SoCs */

	/*
	 * Both the Cyclone V and Arria V share a device tree and have the same
	 * version of this device. This compatible string is used for those
	 * devices, and is not used for sofpgas in general.
	 */
	{ .compatible = "altr,socfpga-spi" },
	{ .compatible = "altr,socfpga-arria10-spi" },
	{
		.compatible = "canaan,kendryte-k210-spi",
		.data = DW_SPI_CAP_XIP,
	},
	{
		.compatible = "canaan,kendryte-k210-ssi",
		.data = DW_SPI_CAP_DWC_SSI,
	},
	{ .compatible = "intel,stratix10-spi" },
	{ .compatible = "intel,agilex-spi" },
	{ .compatible = "mscc,ocelot-spi" },
	{ .compatible = "mscc,jaguar2-spi" },
	{ .compatible = "snps,axs10x-spi" },
	{ .compatible = "snps,hsdk-spi" },
	{ }
};

U_BOOT_DRIVER(dw_spi) = {
	.name = "dw_spi",
	.id = UCLASS_SPI,
	.of_match = dw_spi_ids,
	.ops = &dw_spi_ops,
	.of_to_plat = dw_spi_of_to_plat,
	.plat_auto	= sizeof(struct dw_spi_plat),
	.priv_auto	= sizeof(struct dw_spi_priv),
	.probe = dw_spi_probe,
	.remove = dw_spi_remove,
};

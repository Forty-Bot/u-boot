// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Exceet Electronics GmbH
 * Copyright (C) 2018 Bootlin
 *
 * Author: Boris Brezillon <boris.brezillon@bootlin.com>
 */

#include <common.h>
#include <dm.h>
#include <errno.h>
#include <log.h>
#include <malloc.h>
#include <spi.h>
#include <spi.h>
#include <spi-mem.h>

static int spi_check_buswidth_req(struct spi_slave *slave, u8 buswidth, bool tx)
{
	u32 mode = slave->mode;

	switch (buswidth) {
	case 1:
		return 0;

	case 2:
		if ((tx && (mode & (SPI_TX_DUAL | SPI_TX_QUAD))) ||
		    (!tx && (mode & (SPI_RX_DUAL | SPI_RX_QUAD))))
			return 0;

		break;

	case 4:
		if ((tx && (mode & SPI_TX_QUAD)) ||
		    (!tx && (mode & SPI_RX_QUAD)))
			return 0;

		break;
	case 8:
		if ((tx && (mode & SPI_TX_OCTAL)) ||
		    (!tx && (mode & SPI_RX_OCTAL)))
			return 0;

		break;

	default:
		break;
	}

	return -ENOTSUPP;
}

bool spi_mem_default_supports_op(struct spi_slave *slave,
				 const struct spi_mem_op *op)
{
	if (spi_check_buswidth_req(slave, op->cmd.buswidth, true))
		return false;

	if (op->addr.nbytes &&
	    spi_check_buswidth_req(slave, op->addr.buswidth, true))
		return false;

	if (op->dummy.nbytes &&
	    spi_check_buswidth_req(slave, op->dummy.buswidth, true))
		return false;

	if (op->data.dir != SPI_MEM_NO_DATA &&
	    spi_check_buswidth_req(slave, op->data.buswidth,
				   op->data.dir == SPI_MEM_DATA_OUT))
		return false;

	return true;
}

/**
 * spi_mem_supports_op() - Check if a memory device and the controller it is
 *			   connected to support a specific memory operation
 * @slave: the SPI device
 * @op: the memory operation to check
 *
 * Some controllers are only supporting Single or Dual IOs, others might only
 * support specific opcodes, or it can even be that the controller and device
 * both support Quad IOs but the hardware prevents you from using it because
 * only 2 IO lines are connected.
 *
 * This function checks whether a specific operation is supported.
 *
 * Return: true if @op is supported, false otherwise.
 */
bool spi_mem_supports_op(struct spi_slave *slave,
			 const struct spi_mem_op *op)
{
	struct udevice *bus = slave->dev->parent;
	struct dm_spi_ops *ops = spi_get_ops(bus);

	if (ops->mem_ops && ops->mem_ops->supports_op)
		return ops->mem_ops->supports_op(slave, op);

	return spi_mem_default_supports_op(slave, op);
}

/**
 * spi_mem_exec_op() - Execute a memory operation
 * @slave: the SPI device
 * @op: the memory operation to execute
 *
 * Executes a memory operation.
 *
 * This function first checks that @op is supported and then tries to execute
 * it.
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
int spi_mem_exec_op(struct spi_slave *slave, const struct spi_mem_op *op)
{
	struct udevice *bus = slave->dev->parent;
	struct dm_spi_ops *ops = spi_get_ops(bus);
	unsigned int pos = 0;
	const u8 *tx_buf = NULL;
	u8 *rx_buf = NULL;
	int op_len;
	u32 flag;
	int ret;
	int i;

	if (!spi_mem_supports_op(slave, op))
		return -ENOTSUPP;

	ret = spi_claim_bus(slave);
	if (ret < 0)
		return ret;

	if (ops->mem_ops && ops->mem_ops->exec_op) {
		ret = ops->mem_ops->exec_op(slave, op);

		/*
		 * Some controllers only optimize specific paths (typically the
		 * read path) and expect the core to use the regular SPI
		 * interface in other cases.
		 */
		if (!ret || ret != -ENOTSUPP) {
			spi_release_bus(slave);
			return ret;
		}
	}

	if (op->data.nbytes) {
		if (op->data.dir == SPI_MEM_DATA_IN)
			rx_buf = op->data.buf.in;
		else
			tx_buf = op->data.buf.out;
	}

	op_len = sizeof(op->cmd.opcode) + op->addr.nbytes + op->dummy.nbytes;

	/*
	 * Avoid using malloc() here so that we can use this code in SPL where
	 * simple malloc may be used. That implementation does not allow free()
	 * so repeated calls to this code can exhaust the space.
	 *
	 * The value of op_len is small, since it does not include the actual
	 * data being sent, only the op-code and address. In fact, it should be
	 * possible to just use a small fixed value here instead of op_len.
	 */
	u8 op_buf[op_len];

	op_buf[pos++] = op->cmd.opcode;

	if (op->addr.nbytes) {
		for (i = 0; i < op->addr.nbytes; i++)
			op_buf[pos + i] = op->addr.val >>
				(8 * (op->addr.nbytes - i - 1));

		pos += op->addr.nbytes;
	}

	if (op->dummy.nbytes)
		memset(op_buf + pos, 0xff, op->dummy.nbytes);

	/* 1st transfer: opcode + address + dummy cycles */
	flag = SPI_XFER_BEGIN;
	/* Make sure to set END bit if no tx or rx data messages follow */
	if (!tx_buf && !rx_buf)
		flag |= SPI_XFER_END;

	ret = spi_xfer(slave, op_len * 8, op_buf, NULL, flag);
	if (ret)
		return ret;

	/* 2nd transfer: rx or tx data path */
	if (tx_buf || rx_buf) {
		ret = spi_xfer(slave, op->data.nbytes * 8, tx_buf,
			       rx_buf, SPI_XFER_END);
		if (ret)
			return ret;
	}

	spi_release_bus(slave);

	for (i = 0; i < pos; i++)
		debug("%02x ", op_buf[i]);
	debug("| [%dB %s] ",
	      tx_buf || rx_buf ? op->data.nbytes : 0,
	      tx_buf || rx_buf ? (tx_buf ? "out" : "in") : "-");
	for (i = 0; i < op->data.nbytes; i++)
		debug("%02x ", tx_buf ? tx_buf[i] : rx_buf[i]);
	debug("[ret %d]\n", ret);

	if (ret < 0)
		return ret;

	return 0;
}

/**
 * spi_mem_adjust_op_size() - Adjust the data size of a SPI mem operation to
 *				 match controller limitations
 * @slave: the SPI device
 * @op: the operation to adjust
 *
 * Some controllers have FIFO limitations and must split a data transfer
 * operation into multiple ones, others require a specific alignment for
 * optimized accesses. This function allows SPI mem drivers to split a single
 * operation into multiple sub-operations when required.
 *
 * Return: a negative error code if the controller can't properly adjust @op,
 *	   0 otherwise. Note that @op->data.nbytes will be updated if @op
 *	   can't be handled in a single step.
 */
int spi_mem_adjust_op_size(struct spi_slave *slave, struct spi_mem_op *op)
{
	struct udevice *bus = slave->dev->parent;
	struct dm_spi_ops *ops = spi_get_ops(bus);

	if (ops->mem_ops && ops->mem_ops->adjust_op_size)
		return ops->mem_ops->adjust_op_size(slave, op);

	if (!ops->mem_ops || !ops->mem_ops->exec_op) {
		unsigned int len;

		len = sizeof(op->cmd.opcode) + op->addr.nbytes +
			op->dummy.nbytes;
		if (slave->max_write_size && len > slave->max_write_size)
			return -EINVAL;

		if (op->data.dir == SPI_MEM_DATA_IN) {
			if (slave->max_read_size)
				op->data.nbytes = min(op->data.nbytes,
					      slave->max_read_size);
		} else if (slave->max_write_size) {
			op->data.nbytes = min(op->data.nbytes,
					      slave->max_write_size - len);
		}

		if (!op->data.nbytes)
			return -EINVAL;
	}

	return 0;
}

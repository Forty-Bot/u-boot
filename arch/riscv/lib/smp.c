// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 Fraunhofer AISEC,
 * Lukas Auer <lukas.auer@aisec.fraunhofer.de>
 */

#include <common.h>
#include <cpu_func.h>
#include <dm.h>
#include <asm/barrier.h>
#include <asm/smp.h>

DECLARE_GLOBAL_DATA_PTR;

static int send_ipi_many(struct ipi_data *ipi, int wait)
{
	ofnode node, cpus;
	u32 reg;
	int ret, pending;

	cpus = ofnode_path("/cpus");
	if (!ofnode_valid(cpus)) {
		pr_err("Can't find cpus node!\n");
		return -EINVAL;
	}

	ofnode_for_each_subnode(node, cpus) {
		/* skip if hart is marked as not available in the device tree */
		if (!ofnode_is_available(node))
			continue;

		/* read hart ID of CPU */
		ret = ofnode_read_u32(node, "reg", &reg);
		if (ret)
			continue;

		/* skip if it is the hart we are running on */
		if (reg == gd->arch.boot_hart)
			continue;

		if (reg >= CONFIG_NR_CPUS) {
			pr_err("Hart ID %d is out of range, increase CONFIG_NR_CPUS\n",
			       reg);
			continue;
		}

#ifndef CONFIG_XIP
		/* skip if hart is not available */
		if (!(gd->arch.available_harts & (1 << reg)))
			continue;
#endif

		gd->arch.ipi[reg].addr = ipi->addr;
		gd->arch.ipi[reg].arg0 = ipi->arg0;
		gd->arch.ipi[reg].arg1 = ipi->arg1;

		ret = riscv_send_ipi(reg);
		if (ret) {
			pr_err("Cannot send IPI to hart %d\n", reg);
			return ret;
		}

		if (wait) {
			pending = 1;
			while (pending) {
				ret = riscv_get_ipi(reg, &pending);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

void handle_ipi(ulong hart)
{
	int ret;
	void (*smp_function)(ulong hart, ulong arg0, ulong arg1);

	if (hart >= CONFIG_NR_CPUS || !READ_ONCE(gd->arch.ipi_ready))
		return;

	/*
	 * Clear the IPI to acknowledge the request before jumping to the
	 * requested function.
	 */
	ret = riscv_clear_ipi(hart);
	if (ret) {
		pr_err("Cannot clear IPI of hart %ld (error %d)\n", hart, ret);
		return;
	}

	__smp_mb();

	smp_function = (void (*)(ulong, ulong, ulong))gd->arch.ipi[hart].addr;
	/*
	 * There may be an IPI raised before u-boot begins execution, so check
	 * to ensure we actually have a function to call.
	 */
	if (!smp_function)
		return;
	log_debug("hart = %lu func = %p\n", hart, smp_function);
	invalidate_icache_all();

	smp_function(hart, gd->arch.ipi[hart].arg0, gd->arch.ipi[hart].arg1);
}

int smp_call_function(ulong addr, ulong arg0, ulong arg1, int wait)
{
	struct ipi_data ipi = {
		.addr = addr,
		.arg0 = arg0,
		.arg1 = arg1,
	};

	return send_ipi_many(&ipi, wait);
}

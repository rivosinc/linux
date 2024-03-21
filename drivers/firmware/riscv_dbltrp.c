// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Rivos Inc.
 */

#define pr_fmt(fmt) "riscv-dbltrp: " fmt

#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/riscv_dbltrp.h>
#include <linux/riscv_sse.h>

#include <asm/sbi.h>

static bool double_trap_enabled;

static int riscv_sse_dbltrp_handle(uint32_t evt, void *arg,
				   struct pt_regs *regs)
{
	__show_regs(regs);
	panic("Double trap !\n");

	return 0;
}

struct cpu_dbltrp_data {
	int error;
};

static void
sbi_cpu_enable_double_trap(void *data)
{
	struct sbiret ret;
	struct cpu_dbltrp_data *cdd = data;

	ret = sbi_ecall(SBI_EXT_FWFT, SBI_EXT_FWFT_SET,
			SBI_FWFT_DOUBLE_TRAP_ENABLE, 1, 0, 0, 0, 0);

	if (ret.error) {
		cdd->error = 1;
		pr_err("Failed to enable double trap on cpu %d\n", smp_processor_id());
	}
}

static int sbi_enable_double_trap(void)
{
	struct cpu_dbltrp_data cdd = {0};

	on_each_cpu(sbi_cpu_enable_double_trap, &cdd, 1);
	if (cdd.error)
		return -1;

	double_trap_enabled = true;

	return 0;
}

bool riscv_double_trap_enabled(void)
{
	return double_trap_enabled;
}
EXPORT_SYMBOL(riscv_double_trap_enabled);

static int __init riscv_dbltrp(void)
{
	struct sse_event *evt;

	if (!riscv_has_extension_unlikely(RISCV_ISA_EXT_SSDBLTRP)) {
		pr_err("Ssdbltrp extension not available\n");
		return 1;
	}

	if (!sbi_probe_extension(SBI_EXT_FWFT)) {
		pr_err("Can not enable double trap, SBI_EXT_FWFT is not available\n");
		return 1;
	}

	if (sbi_enable_double_trap()) {
		pr_err("Failed to enable double trap on all cpus\n");
		return 1;
	}

	evt = sse_event_register(SBI_SSE_EVENT_LOCAL_DOUBLE_TRAP, 0,
				 riscv_sse_dbltrp_handle, NULL);
	if (IS_ERR(evt)) {
		pr_err("SSE double trap register failed\n");
		return PTR_ERR(evt);
	}

	sse_event_enable(evt);
	pr_info("Double trap handling registered\n");

	return 0;
}
device_initcall(riscv_dbltrp);

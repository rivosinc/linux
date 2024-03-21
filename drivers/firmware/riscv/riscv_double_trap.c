// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Rivos Inc.
 */

#define pr_fmt(fmt) "riscv-double_trap: " fmt

#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/riscv_double_trap.h>
#include <linux/riscv_sse.h>

#include <asm/sbi.h>

static bool double_trap_enabled;

static int riscv_sse_double_trap_handler(uint32_t evt, void *arg,
					 struct pt_regs *regs)
{
	__show_regs(regs);
	panic("Double trap !\n");
}

struct cpu_double_trap_data {
	int error;
};

static void sbi_cpu_enable_double_trap(void *data)
{
	struct sbiret ret;
	struct cpu_double_trap_data *cdd = data;

	ret = sbi_ecall(SBI_EXT_FWFT, SBI_EXT_FWFT_SET, SBI_FWFT_DOUBLE_TRAP, 1,
			0, 0, 0, 0);

	if (ret.error) {
		cdd->error = true;
		pr_err("Failed to enable double trap on cpu %d\n",
		       smp_processor_id());
	}
}

static int sbi_enable_double_trap(void)
{
	struct cpu_double_trap_data cdd = { false };

	on_each_cpu(sbi_cpu_enable_double_trap, &cdd, 1);
	if (cdd.error)
		return -EOPNOTSUPP;

	double_trap_enabled = true;

	return 0;
}

bool riscv_double_trap_enabled(void)
{
	return double_trap_enabled;
}
EXPORT_SYMBOL(riscv_double_trap_enabled);

static int __init riscv_double_trap_init(void)
{
	int ret;
	struct sse_event *evt;

	if (!riscv_has_extension_unlikely(RISCV_ISA_EXT_SSDBLTRP)) {
		pr_err("Ssdbltrp extension not available\n");
		return -EOPNOTSUPP;
	}

	if (!sbi_probe_extension(SBI_EXT_FWFT)) {
		pr_err("Can not enable double trap, SBI_EXT_FWFT is not available\n");
		return -EOPNOTSUPP;
	}

	ret = sbi_enable_double_trap();
	if (ret) {
		pr_err("Failed to enable double trap on all cpus\n");
		return ret;
	}

	evt = sse_event_register(SBI_SSE_EVENT_LOCAL_DOUBLE_TRAP, 0,
				 riscv_sse_double_trap_handler, NULL);
	if (IS_ERR(evt)) {
		pr_err("SSE double trap register failed\n");
		return PTR_ERR(evt);
	}

	ret = sse_event_enable(evt);
	if (ret) {
		pr_err("Failed to enable double trap SSE event\n");
		sse_event_unregister(evt);
		return ret;
	}

	pr_info("Double trap handling registered\n");

	return 0;
}
device_initcall(riscv_double_trap_init);

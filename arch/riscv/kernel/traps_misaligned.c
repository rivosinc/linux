// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Western Digital Corporation or its affiliates.
 */
#include <linux/kernel.h>
#include <linux/perf_event.h>

#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/csr.h>
#include <asm/entry-common.h>
#include <asm/hwprobe.h>
#include <asm/reg_mem_access.h>
#include <asm/sbi.h>
#include <asm/vector.h>

#define PT_REGS_PTR(regs, reg)	\
	((ulong *)((ulong)(regs) + reg))

static int host_read_mem(void *priv, unsigned long addr, unsigned long len, void *data)
{
	struct pt_regs *regs = priv;

	if (user_mode(regs)) {
		if (copy_from_user(data, (u8 __user *)addr, len))
			return -1;
	} else {
		memcpy(data, (u8 *)addr, len);
	}

	return 0;
}

static int host_write_mem(void *priv, unsigned long addr, unsigned long len, void *data)
{
	struct pt_regs *regs = priv;

	if (user_mode(regs)) {
		if (copy_to_user((u8 __user *)addr, data, len))
			return -1;
	} else {
		memcpy((u8 *)addr, data, len);
	}

	return 0;
}

static void host_epc_set(void *priv, unsigned long advance)
{
	struct pt_regs *regs = priv;

	regs->epc += advance;
}

static unsigned long host_epc_get(void *priv)
{
	struct pt_regs *regs = priv;

	return regs->epc;
}

static unsigned long host_badaddr_get(void *priv)
{
	struct pt_regs *regs = priv;

	return regs->badaddr;
}

static void host_reg_set(void *priv, struct access_reg_data reg) {

	struct pt_regs *regs = priv;
	unsigned int reg_offset = reg.reg;

	if (!reg.fp)
		*PT_REGS_PTR(regs, reg_offset) = reg.data.data_ulong;
	else if (reg.len == 8)
		riscv_set_f64_reg(reg_offset, reg.data.data_u64);
	else
		riscv_set_f32_reg(reg_offset, reg.data.data_ulong);

	if (reg.fp)
		regs->status |= SR_FS_DIRTY;
}

static void host_reg_get(void *priv, struct access_reg_data *reg)
{
	struct pt_regs *regs = priv;
	unsigned int reg_offset = reg->reg;

	if (!reg->fp)
		reg->data.data_ulong = *PT_REGS_PTR(regs, reg_offset);
	else if (reg->len == 8)
		reg->data.data_u64 = riscv_get_f64_reg(reg_offset);
	else
		reg->data.data_ulong = riscv_get_f32_reg(reg_offset);

	if (reg->fp)
		regs->status |= SR_FS_DIRTY;
}

static const struct access_ops host_access_ops = {
	.read_mem = host_read_mem,
	.write_mem = host_write_mem,
	.reg_set = host_reg_set,
	.reg_get = host_reg_get,
	.epc_set = host_epc_set,
	.epc_get = host_epc_get,
	.badaddr_get = host_badaddr_get,
};

/* sysctl hooks */
int unaligned_enabled __read_mostly = 1;	/* Enabled by default */

#ifdef CONFIG_RISCV_VECTOR_MISALIGNED
static int handle_vector_misaligned_load(struct pt_regs *regs)
{
	unsigned long epc = regs->epc;
	unsigned long insn;

	if (riscv_get_insn(regs, &host_access_ops, &insn))
		return -1;

	/* Only return 0 when in check_vector_unaligned_access_emulated */
	if (*this_cpu_ptr(&vector_misaligned_access) == RISCV_HWPROBE_MISALIGNED_VECTOR_UNKNOWN) {
		*this_cpu_ptr(&vector_misaligned_access) = RISCV_HWPROBE_MISALIGNED_VECTOR_UNSUPPORTED;
		regs->epc = epc + INSN_LEN(insn);
		return 0;
	}

	/* If vector instruction we don't emulate it yet */
	regs->epc = epc;
	return -1;
}
#else
static int handle_vector_misaligned_load(struct pt_regs *regs)
{
	return -1;
}
#endif

static int handle_scalar_misaligned_load(struct pt_regs *regs)
{
	perf_sw_event(PERF_COUNT_SW_ALIGNMENT_FAULTS, 1, regs, regs->badaddr);

	*this_cpu_ptr(&misaligned_access_speed) = RISCV_HWPROBE_MISALIGNED_SCALAR_EMULATED;

	if (!unaligned_enabled)
		return -1;

	if (user_mode(regs) && (current->thread.align_ctl & PR_UNALIGN_SIGBUS))
		return -1;

	return riscv_scalar_misaligned_load(regs, &host_access_ops);
}

static int handle_scalar_misaligned_store(struct pt_regs *regs)
{
	perf_sw_event(PERF_COUNT_SW_ALIGNMENT_FAULTS, 1, regs, regs->badaddr);

	if (!unaligned_enabled)
		return -1;

	if (user_mode(regs) && (current->thread.align_ctl & PR_UNALIGN_SIGBUS))
		return -1;

	return riscv_scalar_misaligned_store(regs, &host_access_ops);
}

int handle_misaligned_load(struct pt_regs *regs)
{
	unsigned long insn;

	if (IS_ENABLED(CONFIG_RISCV_VECTOR_MISALIGNED)) {
		if (riscv_get_insn(regs, &host_access_ops, &insn))
			return -1;

		if (insn_is_vector(insn))
			return handle_vector_misaligned_load(regs);
	}

	if (IS_ENABLED(CONFIG_RISCV_SCALAR_MISALIGNED))
		return handle_scalar_misaligned_load(regs);

	return -1;
}

int handle_misaligned_store(struct pt_regs *regs)
{
	if (IS_ENABLED(CONFIG_RISCV_SCALAR_MISALIGNED))
		return handle_scalar_misaligned_store(regs);

	return -1;
}

#ifdef CONFIG_RISCV_VECTOR_MISALIGNED
void check_vector_unaligned_access_emulated(struct work_struct *work __always_unused)
{
	long *mas_ptr = this_cpu_ptr(&vector_misaligned_access);
	unsigned long tmp_var;

	*mas_ptr = RISCV_HWPROBE_MISALIGNED_VECTOR_UNKNOWN;

	kernel_vector_begin();
	/*
	 * In pre-13.0.0 versions of GCC, vector registers cannot appear in
	 * the clobber list. This inline asm clobbers v0, but since we do not
	 * currently build the kernel with V enabled, the v0 clobber arg is not
	 * needed (as the compiler will not emit vector code itself). If the kernel
	 * is changed to build with V enabled, the clobber arg will need to be
	 * added here.
	 */
	__asm__ __volatile__ (
		".balign 4\n\t"
		".option push\n\t"
		".option arch, +zve32x\n\t"
		"       vsetivli zero, 1, e16, m1, ta, ma\n\t"	// Vectors of 16b
		"       vle16.v v0, (%[ptr])\n\t"		// Load bytes
		".option pop\n\t"
		: : [ptr] "r" ((u8 *)&tmp_var + 1));
	kernel_vector_end();
}

bool __init check_vector_unaligned_access_emulated_all_cpus(void)
{
	int cpu;

	/*
	 * While being documented as very slow, schedule_on_each_cpu() is used since
	 * kernel_vector_begin() expects irqs to be enabled or it will panic()
	 */
	schedule_on_each_cpu(check_vector_unaligned_access_emulated);

	for_each_online_cpu(cpu)
		if (per_cpu(vector_misaligned_access, cpu)
		    == RISCV_HWPROBE_MISALIGNED_VECTOR_UNKNOWN)
			return false;

	return true;
}
#else
bool __init check_vector_unaligned_access_emulated_all_cpus(void)
{
	return false;
}
#endif

static bool all_cpus_unaligned_scalar_access_emulated(void)
{
	int cpu;

	for_each_online_cpu(cpu)
		if (per_cpu(misaligned_access_speed, cpu) !=
		    RISCV_HWPROBE_MISALIGNED_SCALAR_EMULATED)
			return false;

	return true;
}

#ifdef CONFIG_RISCV_SCALAR_MISALIGNED

static bool unaligned_ctl __read_mostly;

static void check_unaligned_access_emulated(void *arg __always_unused)
{
	int cpu = smp_processor_id();
	long *mas_ptr = per_cpu_ptr(&misaligned_access_speed, cpu);
	unsigned long tmp_var, tmp_val;

	*mas_ptr = RISCV_HWPROBE_MISALIGNED_SCALAR_UNKNOWN;

	__asm__ __volatile__ (
		"       "REG_L" %[tmp], 1(%[ptr])\n"
		: [tmp] "=r" (tmp_val) : [ptr] "r" (&tmp_var) : "memory");
}

static int cpu_online_check_unaligned_access_emulated(unsigned int cpu)
{
	long *mas_ptr = per_cpu_ptr(&misaligned_access_speed, cpu);

	check_unaligned_access_emulated(NULL);

	/*
	 * If unaligned_ctl is already set, this means that we detected that all
	 * CPUS uses emulated misaligned access at boot time. If that changed
	 * when hotplugging the new cpu, this is something we don't handle.
	 */
	if (unlikely(unaligned_ctl && (*mas_ptr != RISCV_HWPROBE_MISALIGNED_SCALAR_EMULATED))) {
		pr_crit("CPU misaligned accesses non homogeneous (expected all emulated)\n");
		return -EINVAL;
	}

	return 0;
}

bool __init check_unaligned_access_emulated_all_cpus(void)
{
	/*
	 * We can only support PR_UNALIGN controls if all CPUs have misaligned
	 * accesses emulated since tasks requesting such control can run on any
	 * CPU.
	 */
	on_each_cpu(check_unaligned_access_emulated, NULL, 1);

	if (!all_cpus_unaligned_scalar_access_emulated())
		return false;

	unaligned_ctl = true;
	return true;
}

bool unaligned_ctl_available(void)
{
	return unaligned_ctl;
}
#else
bool __init check_unaligned_access_emulated_all_cpus(void)
{
	return false;
}
static int cpu_online_check_unaligned_access_emulated(unsigned int cpu)
{
	return 0;
}
#endif

static bool misaligned_traps_delegated;

#ifdef CONFIG_RISCV_SBI

static int cpu_online_sbi_unaligned_setup(unsigned int cpu)
{
	if (sbi_fwft_set(SBI_FWFT_MISALIGNED_EXC_DELEG, 1, 0) &&
	    misaligned_traps_delegated) {
		pr_crit("Misaligned trap delegation non homogeneous (expected delegated)");
		return -EINVAL;
	}

	return 0;
}

void __init unaligned_access_init(void)
{
	int ret;

	ret = sbi_fwft_set_online_cpus(SBI_FWFT_MISALIGNED_EXC_DELEG, 1, 0);
	if (ret)
		return;

	misaligned_traps_delegated = true;
	pr_info("SBI misaligned access exception delegation ok\n");
	/*
	 * Note that we don't have to take any specific action here, if
	 * the delegation is successful, then
	 * check_unaligned_access_emulated() will verify that indeed the
	 * platform traps on misaligned accesses.
	 */
}
#else
void __init unaligned_access_init(void) {}

static int cpu_online_sbi_unaligned_setup(unsigned int cpu __always_unused)
{
	return 0;
}

#endif

int cpu_online_unaligned_access_init(unsigned int cpu)
{
	int ret;

	ret = cpu_online_sbi_unaligned_setup(cpu);
	if (ret)
		return ret;

	return cpu_online_check_unaligned_access_emulated(cpu);
}

bool misaligned_traps_can_delegate(void)
{
	/*
	 * Either we successfully requested misaligned traps delegation for all
	 * CPUs, or the SBI does not implement the FWFT extension but delegated
	 * the exception by default.
	 */
	return misaligned_traps_delegated ||
	       all_cpus_unaligned_scalar_access_emulated();
}
EXPORT_SYMBOL_GPL(misaligned_traps_can_delegate);

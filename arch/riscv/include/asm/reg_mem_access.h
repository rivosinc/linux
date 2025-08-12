/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_RISCV_MISALIGNED_H
#define _ASM_RISCV_MISALIGNED_H

#include <linux/types.h>

#define INSN_LEN(insn)			((((insn) & 0x3) < 0x3) ? 2 : 4)

union reg_data {
	u8 data_bytes[8];
	ulong data_ulong;
	u64 data_u64;
};

struct access_reg_data {
	unsigned int len;
	unsigned int reg;
	bool fp;
	union reg_data data;
};

struct access_ops {
	int (*read_mem)(void *priv, unsigned long addr, unsigned long len, void *data);
	int (*write_mem)(void *priv, unsigned long addr, unsigned long len, void *data);
	void (*reg_set)(void *priv, struct access_reg_data reg);
	void (*reg_get)(void *priv, struct access_reg_data *reg);
	void (*epc_set)(void *priv, unsigned long advance);
	unsigned long (*epc_get)(void *priv);
	unsigned long (*badaddr_get)(void *priv);
};

int riscv_scalar_misaligned_load(void *priv, const struct access_ops *ops);
int riscv_scalar_misaligned_store(void *priv, const struct access_ops *ops);
int riscv_get_insn(void *priv, const struct access_ops *ops,
		   unsigned long *insn);

#ifdef CONFIG_FPU

extern void put_f32_reg(unsigned long fp_reg, unsigned long value);

static inline int riscv_set_f32_reg(unsigned long fp_reg, unsigned long val)
{
	put_f32_reg(fp_reg, val);

	return 0;
}

extern void put_f64_reg(unsigned long fp_reg, unsigned long value);

static inline int riscv_set_f64_reg(unsigned long fp_reg, u64 val)
{
	unsigned long value;

#if __riscv_xlen == 32
	value = (unsigned long) &val;
#else
	value = val;
#endif
	put_f64_reg(fp_reg, value);

	return 0;
}

#if __riscv_xlen == 32
extern void get_f64_reg(unsigned long fp_reg, u64 *value);

static inline u64 riscv_get_f64_reg(unsigned long insn, unsigned long fp_reg)
{
	u64 val;

	get_f64_reg(fp_reg, &val);

	return val;
}
#else

extern unsigned long get_f64_reg(unsigned long fp_reg);

static inline unsigned long riscv_get_f64_reg(unsigned long fp_reg)
{
	return get_f64_reg(fp_reg);
}

#endif

extern unsigned long get_f32_reg(unsigned long fp_reg);

static inline unsigned long riscv_get_f32_reg(unsigned long fp_reg)
{
	return get_f32_reg(fp_reg);
}

#else /* CONFIG_FPU */
static inline void riscv_set_f32_reg(unsigned long fp_reg, unsigned long val) {}

static inline void riscv_set_f64_reg(unsigned long fp_reg, u64 val) {}

static inline unsigned long riscv_get_f32_reg(unsigned long fp_reg);
{
	return 0;
}

static inline unsigned long riscv_get_f64_reg(unsigned long fp_reg)
{
	return 0;
}
#endif

#endif /* _ASM_RISCV_MISALIGNED_H */

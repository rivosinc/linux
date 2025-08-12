// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Rivos Inc.
 */
#include <linux/kernel.h>

#include <asm/reg_mem_access.h>

#define INSN_MATCH_LB			0x3
#define INSN_MASK_LB			0x707f
#define INSN_MATCH_LH			0x1003
#define INSN_MASK_LH			0x707f
#define INSN_MATCH_LW			0x2003
#define INSN_MASK_LW			0x707f
#define INSN_MATCH_LD			0x3003
#define INSN_MASK_LD			0x707f
#define INSN_MATCH_LBU			0x4003
#define INSN_MASK_LBU			0x707f
#define INSN_MATCH_LHU			0x5003
#define INSN_MASK_LHU			0x707f
#define INSN_MATCH_LWU			0x6003
#define INSN_MASK_LWU			0x707f
#define INSN_MATCH_SB			0x23
#define INSN_MASK_SB			0x707f
#define INSN_MATCH_SH			0x1023
#define INSN_MASK_SH			0x707f
#define INSN_MATCH_SW			0x2023
#define INSN_MASK_SW			0x707f
#define INSN_MATCH_SD			0x3023
#define INSN_MASK_SD			0x707f

#define INSN_MATCH_FLW			0x2007
#define INSN_MASK_FLW			0x707f
#define INSN_MATCH_FLD			0x3007
#define INSN_MASK_FLD			0x707f
#define INSN_MATCH_FLQ			0x4007
#define INSN_MASK_FLQ			0x707f
#define INSN_MATCH_FSW			0x2027
#define INSN_MASK_FSW			0x707f
#define INSN_MATCH_FSD			0x3027
#define INSN_MASK_FSD			0x707f
#define INSN_MATCH_FSQ			0x4027
#define INSN_MASK_FSQ			0x707f

#define INSN_MATCH_C_LD			0x6000
#define INSN_MASK_C_LD			0xe003
#define INSN_MATCH_C_SD			0xe000
#define INSN_MASK_C_SD			0xe003
#define INSN_MATCH_C_LW			0x4000
#define INSN_MASK_C_LW			0xe003
#define INSN_MATCH_C_SW			0xc000
#define INSN_MASK_C_SW			0xe003
#define INSN_MATCH_C_LDSP		0x6002
#define INSN_MASK_C_LDSP		0xe003
#define INSN_MATCH_C_SDSP		0xe002
#define INSN_MASK_C_SDSP		0xe003
#define INSN_MATCH_C_LWSP		0x4002
#define INSN_MASK_C_LWSP		0xe003
#define INSN_MATCH_C_SWSP		0xc002
#define INSN_MASK_C_SWSP		0xe003

#define INSN_MATCH_C_FLD		0x2000
#define INSN_MASK_C_FLD			0xe003
#define INSN_MATCH_C_FLW		0x6000
#define INSN_MASK_C_FLW			0xe003
#define INSN_MATCH_C_FSD		0xa000
#define INSN_MASK_C_FSD			0xe003
#define INSN_MATCH_C_FSW		0xe000
#define INSN_MASK_C_FSW			0xe003
#define INSN_MATCH_C_FLDSP		0x2002
#define INSN_MASK_C_FLDSP		0xe003
#define INSN_MATCH_C_FSDSP		0xa002
#define INSN_MASK_C_FSDSP		0xe003
#define INSN_MATCH_C_FLWSP		0x6002
#define INSN_MASK_C_FLWSP		0xe003
#define INSN_MATCH_C_FSWSP		0xe002
#define INSN_MASK_C_FSWSP		0xe003

#define INSN_MATCH_C_LHU		0x8400
#define INSN_MASK_C_LHU			0xfc43
#define INSN_MATCH_C_LH			0x8440
#define INSN_MASK_C_LH			0xfc43
#define INSN_MATCH_C_SH			0x8c00
#define INSN_MASK_C_SH			0xfc43

#if defined(CONFIG_64BIT)
#define LOG_REGBYTES			3
#define XLEN				64
#else
#define LOG_REGBYTES			2
#define XLEN				32
#endif

#define SH_RD				7
#define SH_RS1				15
#define SH_RS2				20
#define SH_RS2C				2

#define RV_X(x, s, n)			(((x) >> (s)) & ((1 << (n)) - 1))
#define RVC_RS1S(insn)			(8 + RV_X(insn, SH_RD, 3))
#define RVC_RS2S(insn)			(8 + RV_X(insn, SH_RS2C, 3))
#define RVC_RS2(insn)			RV_X(insn, SH_RS2C, 5)

#define SHIFT_RIGHT(x, y)		\
	((y) < 0 ? ((x) << -(y)) : ((x) >> (y)))

#define REG_MASK			\
	((1 << (5 + LOG_REGBYTES)) - (1 << LOG_REGBYTES))

#define REG_OFFSET(insn, pos)		\
	(SHIFT_RIGHT((insn), (pos) - LOG_REGBYTES) & REG_MASK)

#define GET_RS1(insn)			REG_OFFSET(insn, SH_RS1)
#define GET_RS2(insn)			REG_OFFSET(insn, SH_RS2)
#define GET_RS1S(insn)			REG_OFFSET(RVC_RS1S(insn), 0)
#define GET_RS2S(insn)			REG_OFFSET(RVC_RS2S(insn), 0)
#define GET_RS2C(insn)			REG_OFFSET(insn, SH_RS2C)
#define GET_SP(regs)			REG_OFFSET(2, 0)

#define FP_GET_RD(insn)		(insn >> 7 & 0x1F)

#define GET_F_RS(insn, fp_reg_offset)	((insn >> fp_reg_offset) & 0x1F)
#define GET_F_RS2(insn)			GET_F_RS(insn, 20)
#define GET_F_RS2C(insn)		GET_F_RS(insn, 2)
#define GET_F_RS2S(insn)		GET_F_RS(RVC_RS2S(insn), 0)

int riscv_get_insn(void *priv, const struct access_ops *ops, ulong *r_insn)
{
	ulong insn = 0, epc = ops->epc_get(priv);

	if (epc & 0x2) {
		ulong tmp = 0;

		if (ops->read_mem(priv, epc, sizeof(u16), &insn))
			return -EFAULT;
		/* __get_user() uses regular "lw" which sign extend the loaded
		 * value make sure to clear higher order bits in case we "or" it
		 * below with the upper 16 bits half.
		 */
		insn &= GENMASK(15, 0);
		if ((insn & __INSN_LENGTH_MASK) != __INSN_LENGTH_32) {
			*r_insn = insn;
			return 0;
		}
		epc += sizeof(u16);
		if (ops->read_mem(priv, epc, sizeof(u16), &tmp))
			return -EFAULT;
		*r_insn = (tmp << 16) | insn;

		return 0;
	} else {
		if (ops->read_mem(priv, epc, sizeof(u32), &insn))
			return -EFAULT;
		if ((insn & __INSN_LENGTH_MASK) == __INSN_LENGTH_32) {
			*r_insn = insn;
			return 0;
		}
		insn &= GENMASK(15, 0);
		*r_insn = insn;

		return 0;
	}
}

int riscv_scalar_misaligned_load(void *priv, const struct access_ops *ops)
{
	unsigned long insn, addr;
	int fp = 0, shift = 0, len = 0;
	struct access_reg_data reg = {0};

	if (riscv_get_insn(priv, ops, &insn))
		return -1;

	if ((insn & INSN_MASK_LW) == INSN_MATCH_LW) {
		reg.len = 4;
		shift = 8 * (sizeof(unsigned long) - len);
#if defined(CONFIG_64BIT)
	} else if ((insn & INSN_MASK_LD) == INSN_MATCH_LD) {
		reg.len = 8;
		shift = 8 * (sizeof(unsigned long) - len);
	} else if ((insn & INSN_MASK_LWU) == INSN_MATCH_LWU) {
		reg.len = 4;
#endif
	} else if ((insn & INSN_MASK_FLD) == INSN_MATCH_FLD) {
		reg.fp = true;
		reg.len = 8;
	} else if ((insn & INSN_MASK_FLW) == INSN_MATCH_FLW) {
		reg.fp = true;
		reg.len = 4;
	} else if ((insn & INSN_MASK_LH) == INSN_MATCH_LH) {
		reg.len = 2;
		shift = 8 * (sizeof(unsigned long) - len);
	} else if ((insn & INSN_MASK_LHU) == INSN_MATCH_LHU) {
		reg.len = 2;
#if defined(CONFIG_64BIT)
	} else if ((insn & INSN_MASK_C_LD) == INSN_MATCH_C_LD) {
		reg.len = 8;
		shift = 8 * (sizeof(unsigned long) - len);
		insn = RVC_RS2S(insn) << SH_RD;
	} else if ((insn & INSN_MASK_C_LDSP) == INSN_MATCH_C_LDSP &&
		   ((insn >> SH_RD) & 0x1f)) {
		reg.len = 8;
		shift = 8 * (sizeof(unsigned long) - len);
#endif
	} else if ((insn & INSN_MASK_C_LW) == INSN_MATCH_C_LW) {
		reg.len = 4;
		shift = 8 * (sizeof(unsigned long) - len);
		insn = RVC_RS2S(insn) << SH_RD;
	} else if ((insn & INSN_MASK_C_LWSP) == INSN_MATCH_C_LWSP &&
		   ((insn >> SH_RD) & 0x1f)) {
		reg.len = 4;
		shift = 8 * (sizeof(unsigned long) - len);
	} else if ((insn & INSN_MASK_C_FLD) == INSN_MATCH_C_FLD) {
		reg.fp = true;
		reg.len = 8;
		insn = RVC_RS2S(insn) << SH_RD;
	} else if ((insn & INSN_MASK_C_FLDSP) == INSN_MATCH_C_FLDSP) {
		reg.fp = true;
		reg.len = 8;
#if defined(CONFIG_32BIT)
	} else if ((insn & INSN_MASK_C_FLW) == INSN_MATCH_C_FLW) {
		reg.fp = true;
		reg.len = 4;
		insn = RVC_RS2S(insn) << SH_RD;
	} else if ((insn & INSN_MASK_C_FLWSP) == INSN_MATCH_C_FLWSP) {
		reg.fp = true;
		reg.len = 4;
#endif
	} else if ((insn & INSN_MASK_C_LHU) == INSN_MATCH_C_LHU) {
		reg.len = 2;
		insn = RVC_RS2S(insn) << SH_RD;
	} else if ((insn & INSN_MASK_C_LH) == INSN_MATCH_C_LH) {
		reg.len = 2;
		shift = 8 * (sizeof(ulong) - len);
		insn = RVC_RS2S(insn) << SH_RD;
	} else {
		return -1;
	}

	if (!IS_ENABLED(CONFIG_FPU) && fp)
		return -EOPNOTSUPP;

	reg.data.data_u64 = 0;

	addr = ops->badaddr_get(priv);
	if (ops->read_mem(priv, addr, reg.len, (void *)&reg.data))
		return -1;

	if (fp) {
		reg.reg = FP_GET_RD(insn);
	} else {
		reg.data.data_ulong = (long)(reg.data.data_ulong << shift) >> shift;
		reg.reg = REG_OFFSET(insn, SH_RD);
	}

	ops->reg_set(priv, reg);
	ops->epc_set(priv, INSN_LEN(insn));

	return 0;
}

int riscv_scalar_misaligned_store(void *priv, const struct access_ops *ops)
{
	unsigned long insn, addr;
	int len = 0, fp = 0;
	struct access_reg_data reg = {0};

	if (riscv_get_insn(priv, ops, &insn))
		return -1;

	reg.reg = GET_RS2(insn);

	if ((insn & INSN_MASK_SW) == INSN_MATCH_SW) {
		len = 4;
#if defined(CONFIG_64BIT)
	} else if ((insn & INSN_MASK_SD) == INSN_MATCH_SD) {
		len = 8;
#endif
	} else if ((insn & INSN_MASK_FSD) == INSN_MATCH_FSD) {
		fp = 1;
		len = 8;
		reg.reg = GET_F_RS2(insn);
	} else if ((insn & INSN_MASK_FSW) == INSN_MATCH_FSW) {
		fp = 1;
		len = 4;
		reg.reg = GET_F_RS2(insn);
	} else if ((insn & INSN_MASK_SH) == INSN_MATCH_SH) {
		len = 2;
#if defined(CONFIG_64BIT)
	} else if ((insn & INSN_MASK_C_SD) == INSN_MATCH_C_SD) {
		len = 8;
		reg.reg = GET_RS2S(insn);
	} else if ((insn & INSN_MASK_C_SDSP) == INSN_MATCH_C_SDSP) {
		len = 8;
		reg.reg = GET_RS2C(insn);
#endif
	} else if ((insn & INSN_MASK_C_SW) == INSN_MATCH_C_SW) {
		len = 4;
		reg.reg = GET_RS2S(insn);
	} else if ((insn & INSN_MASK_C_SWSP) == INSN_MATCH_C_SWSP) {
		len = 4;
		reg.reg = GET_RS2C(insn);
	} else if ((insn & INSN_MASK_C_FSD) == INSN_MATCH_C_FSD) {
		fp = 1;
		len = 8;
		reg.reg = GET_F_RS2S(insn);
	} else if ((insn & INSN_MASK_C_FSDSP) == INSN_MATCH_C_FSDSP) {
		fp = 1;
		len = 8;
		reg.reg = GET_F_RS2C(insn);
#if !defined(CONFIG_64BIT)
	} else if ((insn & INSN_MASK_C_FSW) == INSN_MATCH_C_FSW) {
		fp = 1;
		len = 4;
		reg.reg = GET_F_RS2S(insn);
	} else if ((insn & INSN_MASK_C_FSWSP) == INSN_MATCH_C_FSWSP) {
		fp = 1;
		len = 4;
		reg.reg = GET_F_RS2C(insn);
#endif
	} else if ((insn & INSN_MASK_C_SH) == INSN_MATCH_C_SH) {
		len = 2;
		reg.reg = GET_RS2S(insn);
	} else {
		return -1;
	}

	if (!IS_ENABLED(CONFIG_FPU) && fp)
		return -EOPNOTSUPP;

	ops->reg_get(priv, &reg);

	addr = ops->badaddr_get(priv);
	if (ops->write_mem(priv, addr, reg.len, &reg.data))
		return -1;

	ops->epc_set(priv, INSN_LEN(insn));

	return 0;
}

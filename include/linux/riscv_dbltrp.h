/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Rivos Inc.
 */

#ifndef __LINUX_RISCV_DBLTRP_H
#define __LINUX_RISCV_DBLTRP_H

#if defined(CONFIG_RISCV_DBLTRP)
bool riscv_double_trap_enabled(void);
#else

static inline bool riscv_double_trap_enabled(void)
{
	return false;
}
#endif

#endif /* __LINUX_RISCV_DBLTRP_H */

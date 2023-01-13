// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 Rivos Inc.
 *
 * Authors:
 *     Rajnesh Kanwal <rkanwal@rivosinc.com>
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <asm/csr.h>
#include <asm/sbi.h>
#include <asm/kvm_vcpu_sbi.h>
#include <asm/kvm_cove.h>

static int kvm_sbi_ext_covg_handler(struct kvm_vcpu *vcpu, struct kvm_run *run,
				    unsigned long *out_val,
				    struct kvm_cpu_trap *utrap, bool *exit)
{
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;
	uint32_t num_pages = cp->a1 / PAGE_SIZE;
	unsigned long funcid = cp->a6;
	uint32_t i;
	int ret;

	switch (funcid) {
	case SBI_EXT_COVG_SHARE_MEMORY:

		/* Initiate a fence if TVM is blocked in the sharing process. */
		if (vcpu->arch.tc->is_blocked) {
			ret = kvm_riscv_cove_tvm_fence(vcpu);
			if (ret)
				return ret;
		}

		for (i = 0; i < num_pages; i++) {
			ret = kvm_riscv_cove_share_page(
				vcpu, cp->a0 + i * PAGE_SIZE);
			if (ret)
				return ret;
		}
		return 0;

	case SBI_EXT_COVG_UNSHARE_MEMORY:

		/* Initiate a fence if TVM is blocked in the unsharing process. */
		if (vcpu->arch.tc->is_blocked) {
			ret = kvm_riscv_cove_tvm_fence(vcpu);
			if (ret)
				return ret;
		}

		for (i = 0; i < num_pages; i++) {
			ret = kvm_riscv_cove_unshare_page(
				vcpu, cp->a0 + i * PAGE_SIZE);
			if (ret)
				return ret;
		}
		return 0;

	case SBI_EXT_COVG_ADD_MMIO_REGION:
	case SBI_EXT_COVG_REMOVE_MMIO_REGION:
	case SBI_EXT_COVG_ALLOW_EXT_INTERRUPT:
	case SBI_EXT_COVG_DENY_EXT_INTERRUPT:
		/* We don't really need to do anything here for now. */
		return 0;

	default:
		kvm_err("%s: Unsupported guest SBI %ld.\n", __func__, funcid);
		return -EOPNOTSUPP;
	}
}

unsigned long kvm_sbi_ext_covg_probe(struct kvm_vcpu *vcpu)
{
	/* KVM COVG SBI handler is only meant for handling calls from TSM */
	return 0;
}

const struct kvm_vcpu_sbi_extension vcpu_sbi_ext_covg = {
	.extid_start = SBI_EXT_COVG,
	.extid_end = SBI_EXT_COVG,
	.handler = kvm_sbi_ext_covg_handler,
	.probe = kvm_sbi_ext_covg_probe,
};

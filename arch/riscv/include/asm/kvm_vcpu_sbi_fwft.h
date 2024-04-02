/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023 Rivos Inc
 *
 * Authors:
 *     Atish Patra <atishp@rivosinc.com>
 */

#ifndef __KVM_VCPU_RISCV_FWFT_H
#define __KVM_VCPU_RISCV_FWFT_H

#include <asm/sbi.h>

#define KVM_SBI_FWFT_FEATURE_COUNT	2

struct kvm_sbi_fwft_config;
struct kvm_vcpu;

struct kvm_sbi_fwft_feature {
	enum sbi_fwft_feature_t id;
	int (*supported)(struct kvm_vcpu *vcpu);
	int (*set)(struct kvm_vcpu *vcpu, struct kvm_sbi_fwft_config *conf, unsigned long value);
	int (*get)(struct kvm_vcpu *vcpu, struct kvm_sbi_fwft_config *conf, unsigned long *value);
};

struct kvm_sbi_fwft_config {
	const struct kvm_sbi_fwft_feature *feature;
	unsigned long flags;
};

/* FWFT data structure per vcpu */
struct kvm_sbi_fwft {
	struct kvm_sbi_fwft_config configs[KVM_SBI_FWFT_FEATURE_COUNT];
};

#define vcpu_to_fwft(vcpu) (&(vcpu)->arch.fwft_context)

#endif /* !__KVM_VCPU_RISCV_FWFT_H */

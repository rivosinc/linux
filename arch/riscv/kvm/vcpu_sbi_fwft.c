// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *     Atish Patra <atish.patra@wdc.com>
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <linux/riscv_double_trap.h>
#include <asm/cpufeature.h>
#include <asm/sbi.h>
#include <asm/kvm_vcpu_sbi.h>
#include <asm/kvm_vcpu_sbi_fwft.h>

#ifdef CONFIG_32BIT
# define CSR_HENVCFG_DBLTRP	CSR_HENVCFGH
# define DBLTRP_DTE	(ENVCFG_DTE >> 32)
#else
# define CSR_HENVCFG_DBLTRP	CSR_HENVCFG
# define DBLTRP_DTE	ENVCFG_DTE
#endif

#define MIS_DELEG (1UL << EXC_LOAD_MISALIGNED | 1UL << EXC_STORE_MISALIGNED)

static const enum sbi_fwft_feature_t kvm_fwft_defined_features[] = {
	SBI_FWFT_MISALIGNED_EXC_DELEG,
	SBI_FWFT_LANDING_PAD,
	SBI_FWFT_SHADOW_STACK,
	SBI_FWFT_DOUBLE_TRAP,
	SBI_FWFT_PTE_AD_HW_UPDATING,
};

static bool kvm_fwft_is_defined_feature(enum sbi_fwft_feature_t feature)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(kvm_fwft_defined_features); i++) {
		if (kvm_fwft_defined_features[i] == feature)
			return true;
	}

	return false;
}

static int kvm_sbi_fwft_misaligned_delegation_supported(struct kvm_vcpu *vcpu)
{
	if (!unaligned_ctl_available())
		return SBI_ERR_NOT_SUPPORTED;

	return 0;
}

static int kvm_sbi_fwft_set_misaligned_delegation(struct kvm_vcpu *vcpu,
					struct kvm_sbi_fwft_config *conf,
					unsigned long value)
{
	if (value)
		csr_set(CSR_HEDELEG, MIS_DELEG);
	else
		csr_clear(CSR_HEDELEG, MIS_DELEG);

	return SBI_SUCCESS;
}

static int kvm_sbi_fwft_get_misaligned_delegation(struct kvm_vcpu *vcpu,
					struct kvm_sbi_fwft_config *conf,
					unsigned long *value)
{
	if (!unaligned_ctl_available())
		return SBI_ERR_NOT_SUPPORTED;

	*value = (csr_read(CSR_HEDELEG) & MIS_DELEG) != 0;

	return SBI_SUCCESS;
}

static int kvm_sbi_fwft_set_double_trap(struct kvm_vcpu *vcpu,
					struct kvm_sbi_fwft_config *conf,
					unsigned long value)
{
	if (!riscv_double_trap_enabled())
		return SBI_ERR_NOT_SUPPORTED;

	if (value)
		csr_set(CSR_HENVCFG_DBLTRP, DBLTRP_DTE);
	else
		csr_clear(CSR_HENVCFG_DBLTRP, DBLTRP_DTE);

	return SBI_SUCCESS;
}

static int kvm_sbi_fwft_get_double_trap(struct kvm_vcpu *vcpu,
					struct kvm_sbi_fwft_config *conf,
					unsigned long *value)
{
	if (!riscv_double_trap_enabled())
		return SBI_ERR_NOT_SUPPORTED;

	*value = (csr_read(CSR_HENVCFG_DBLTRP) & DBLTRP_DTE) != 0;

	return SBI_SUCCESS;
}

static struct kvm_sbi_fwft_config *
kvm_sbi_fwft_get_config(struct kvm_vcpu *vcpu, enum sbi_fwft_feature_t feature)
{
	int i = 0;
	struct kvm_sbi_fwft *fwft = vcpu_to_fwft(vcpu);

	for (i = 0; i < KVM_SBI_FWFT_FEATURE_COUNT; i++) {
		if (fwft->configs[i].feature->id == feature) {
			return &fwft->configs[i];
		}
	}

	return NULL;
}

static int kvm_fwft_get_feature(struct kvm_vcpu *vcpu,
				enum sbi_fwft_feature_t feature,
				struct kvm_sbi_fwft_config **conf)
{
	int ret;
	struct kvm_sbi_fwft_config *tconf;

	tconf = kvm_sbi_fwft_get_config(vcpu, feature);
	if (!tconf) {
		if (kvm_fwft_is_defined_feature(feature))
			return SBI_ERR_NOT_SUPPORTED;

		return SBI_ERR_DENIED;
	}

	if (tconf->feature->supported) {
		ret = tconf->feature->supported(vcpu);
		if (ret)
			return ret;
	}
	*conf = tconf;

	return SBI_SUCCESS;
}

static int kvm_sbi_fwft_set(struct kvm_vcpu *vcpu,
			    enum sbi_fwft_feature_t feature,
			    unsigned long value, unsigned long flags)
{
	int ret;
	struct kvm_sbi_fwft_config *conf;

	ret = kvm_fwft_get_feature(vcpu, feature, &conf);
	if (ret)
		return ret;

	if ((flags & ~SBI_FWFT_SET_FLAG_LOCK) != 0)
		return SBI_ERR_INVALID_PARAM;

	if (conf->flags & SBI_FWFT_SET_FLAG_LOCK)
		return SBI_ERR_DENIED;

	conf->flags = flags;

	return conf->feature->set(vcpu, conf, value);
}

static int kvm_sbi_fwft_get(struct kvm_vcpu *vcpu,
			    enum sbi_fwft_feature_t feature,
			    unsigned long *value)
{
	int ret;
	struct kvm_sbi_fwft_config *conf;

	ret = kvm_fwft_get_feature(vcpu, feature, &conf);
	if (ret)
		return ret;

	return conf->feature->get(vcpu, conf, value);
}

static int kvm_sbi_ext_fwft_handler(struct kvm_vcpu *vcpu, struct kvm_run *run,
				    struct kvm_vcpu_sbi_return *retdata)
{
	int ret = 0;
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;
	unsigned long funcid = cp->a6;

	switch (funcid) {
	case SBI_EXT_FWFT_SET:
		ret = kvm_sbi_fwft_set(vcpu, cp->a0, cp->a1, cp->a2);
		break;
	case SBI_EXT_FWFT_GET:
		ret = kvm_sbi_fwft_get(vcpu, cp->a0, &retdata->out_val);
		break;
	default:
		ret = SBI_ERR_NOT_SUPPORTED;
		break;
	}

	retdata->err_val = ret;

	return 0;
}

static const struct kvm_sbi_fwft_feature features[] = {
	{
		.id = SBI_FWFT_MISALIGNED_EXC_DELEG,
		.supported = kvm_sbi_fwft_misaligned_delegation_supported,
		.set = kvm_sbi_fwft_set_misaligned_delegation,
		.get = kvm_sbi_fwft_get_misaligned_delegation,
	},
	{
		.id = SBI_FWFT_DOUBLE_TRAP,
		.set = kvm_sbi_fwft_set_double_trap,
		.get = kvm_sbi_fwft_get_double_trap,
	}
};

static_assert(ARRAY_SIZE(features) == KVM_SBI_FWFT_FEATURE_COUNT);


static unsigned long kvm_sbi_ext_fwft_probe(struct kvm_vcpu *vcpu)
{
	struct kvm_sbi_fwft *fwft = vcpu_to_fwft(vcpu);
	int i;

	for (i = 0; i < ARRAY_SIZE(features); i++) {
		fwft->configs[i].feature = &features[i];
	}

	return 1;
}

const struct kvm_vcpu_sbi_extension vcpu_sbi_ext_fwft = {
	.extid_start = SBI_EXT_FWFT,
	.extid_end = SBI_EXT_FWFT,
	.handler = kvm_sbi_ext_fwft_handler,
	.probe = kvm_sbi_ext_fwft_probe,
};

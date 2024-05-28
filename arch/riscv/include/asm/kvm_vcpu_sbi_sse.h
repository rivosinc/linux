/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023 Rivos Inc
 *
 * Authors:
 *     Atish Patra <atishp@rivosinc.com>
 */

#ifndef __KVM_VCPU_RISCV_SBI_SSE_H
#define __KVM_VCPU_RISCV_SBI_SSE_H

#include <linux/types.h>
#include <linux/mutex.h>
#include <asm/sbi.h>

struct kvm;
struct kvm_vcpu;
struct kvm_vcpu_sbi_return;

#if IS_ENABLED(CONFIG_RISCV_SSE)

struct sse_event;

struct kvm_sbi_sse_events {
	struct kvm_sbi_sse_event *events;
	unsigned int count;
};

struct kvm_sbi_sse {
	struct kvm_sbi_sse_events events;
	bool init;
};

/* SSE data structure per vcpu */
struct kvm_sbi_sse_context {
	/* Local events are per-vcpu */
	struct kvm_sbi_sse_events local;
	struct list_head enabled_event_list;
	struct mutex enabled_lock;
	/* Global events are shared between all vcpus */
	struct kvm_sbi_sse_events *global;
	bool masked;
};

struct kvm_sbi_sse_event_ops {
	bool (*is_supported)(void *priv, struct kvm_vcpu *vcpu);
};

#define vcpu_to_sbi_sse(vcpu) (&(vcpu)->arch.sbi_sse_context)

int kvm_sbi_sse_inject_event(struct kvm_vcpu *vcpu, u32 event_id);
void kvm_sbi_sse_process_pending_events(struct kvm_vcpu *vcpu);
int kvm_riscv_sbi_sse_init(void);
void kvm_riscv_sbi_sse_exit(void);
int kvm_sbi_sse_add_event(u32 event_id, struct kvm_sbi_sse_event_ops *ops, void *priv);
int kvm_sbi_sse_global_init_vm(struct kvm *kvm);
void kvm_sbi_sse_global_destroy_vm(struct kvm *kvm);

#else

static inline int kvm_sbi_sse_inject_event(struct kvm_vcpu *vcpu, u32 event_id)
{
	return SBI_ERR_NOT_SUPPORTED;
}

static inline void kvm_sbi_sse_process_pending_events(struct kvm_vcpu *vcpu) {}

static inline int kvm_riscv_sbi_sse_init(void)
{
	return 0
}

static inline void kvm_riscv_sbi_sse_exit(void) {}

static inline int kvm_sbi_sse_add_event(u32 event_id, struct kvm_sbi_sse_event_ops *ops, void *priv)
{
	return 0;
}

static inline int kvm_sbi_sse_global_init_vm(struct kvm *kvm)
{
	return 0;
}

static inline void kvm_sbi_sse_global_destroy_vm(struct kvm *kvm) {}

#endif /* IS_ENABLED(CONFIG_RISCV_SSE) */
#endif /* __KVM_VCPU_RISCV_SBI_SSE_H */

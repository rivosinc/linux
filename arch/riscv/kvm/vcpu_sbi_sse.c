// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Rivos Inc
 *
 * Authors:
 *     Clément Léger <cleger@rivosinc.com>
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kvm_host.h>
#include <asm/csr.h>
#include <asm/sbi.h>
#include <asm/kvm_vcpu_sbi.h>
#include <asm/kvm_vcpu_sbi_sse.h>

#define EVENT_IS_GLOBAL(__event_id)	(((__event_id) & SBI_SSE_EVENT_GLOBAL) ? 1 : 0)
#define SBI_SSE_ATTR_STATUS_PENDING_BIT	BIT(SBI_SSE_ATTR_STATUS_PENDING_OFFSET)

static bool sse_event_is_global(struct kvm_sbi_sse_event *event);

struct sbi_sse_entry_state {
	/** entry pc */
	unsigned long pc;
	/** a6 register state */
	unsigned long arg;
};

struct sbi_sse_interrupted_state {
	/** sepc register state */
	unsigned long sepc;
	/** flags register state */
	unsigned long flags;
	/** a6 register state */
	unsigned long a6;
	/** a7 register state */
	unsigned long a7;
};

struct kvm_sbi_sse_event {
	struct kvm_sbi_sse_event_desc *desc;
	unsigned long state;
	unsigned long pending;
	unsigned long prio;
	unsigned long config;
	unsigned long hartid;
	struct sbi_sse_entry_state entry;
	struct sbi_sse_interrupted_state interrupted;
	struct list_head node;
	struct mutex lock;
};

/* We only need to lock global events */
DEFINE_GUARD(sse_event, struct kvm_sbi_sse_event *,
	     if (sse_event_is_global(_T)) mutex_lock(&_T->lock),
	     if (sse_event_is_global(_T)) mutex_unlock(&_T->lock))

DEFINE_GUARD(enabled_events, struct kvm_sbi_sse_context *,
	     mutex_lock(&_T->enabled_lock),
	     mutex_unlock(&_T->enabled_lock))

struct kvm_sbi_sse_event_desc {
	struct list_head node;
	u32 event_id;
	struct kvm_sbi_sse_event_ops *ops;
	void *priv;
};

enum event_desc_type {
	EVENT_DESC_LOCAL,
	EVENT_DESC_GLOBAL,
	EVENT_DESC_COUNT,
};

static DEFINE_MUTEX(event_descs_lock);

struct sse_event_desc_list {
	int count;
	struct list_head list;
};

static struct sse_event_desc_list event_descs[EVENT_DESC_COUNT] = {
	{
		.count = 0,
		.list = LIST_HEAD_INIT(event_descs[0].list)
	},
	{
		.count = 0,
		.list = LIST_HEAD_INIT(event_descs[1].list)
	},
};

/*
 * This array is used to distinguish between standard event and platform
 * events in order to return SBI_ERR_NOT_SUPPORTED for them.
 */
static const u32 standard_events[] = {
	SBI_SSE_EVENT_LOCAL_HIGH_PRIO_RAS,
	SBI_SSE_EVENT_LOCAL_DOUBLE_TRAP,
	SBI_SSE_EVENT_GLOBAL_HIGH_PRIO_RAS,
	SBI_SSE_EVENT_LOCAL_PMU_OVERFLOW,
	SBI_SSE_EVENT_LOCAL_LOW_PRIO_RAS,
	SBI_SSE_EVENT_GLOBAL_LOW_PRIO_RAS,
	SBI_SSE_EVENT_LOCAL_SOFTWARE_INJECTED,
	SBI_SSE_EVENT_GLOBAL_SOFTWARE_INJECTED,
};

static bool sse_is_standard_event(uint32_t event_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(standard_events); i++) {
		if (event_id == standard_events[i])
			return true;
	}

	return false;
}

static uint32_t sse_event_id(struct kvm_sbi_sse_event *event)
{
	return event->desc->event_id;
}

static bool sse_event_is_global(struct kvm_sbi_sse_event *event)
{
	return EVENT_IS_GLOBAL(sse_event_id(event));
}

/* Must be called with enabled_lock held */
static void kvm_sbi_sse_event_insert_ordered(struct kvm_vcpu *vcpu,
					     struct kvm_sbi_sse_event *event)
{
	struct kvm_sbi_sse_event *tmp;
	struct kvm_sbi_sse_context *ctx = vcpu_to_sbi_sse(vcpu);

	/* 0 is the highest priority */
	list_for_each_entry(tmp, &ctx->enabled_event_list, node) {
		if (event->prio < tmp->prio)
			break;
		if (event->prio == tmp->prio &&
		    sse_event_id(event) < sse_event_id(tmp))
			break;
	}
	list_add_tail(&event->node, &tmp->node);
}

static struct kvm_sbi_sse_event *
kvm_sbi_sse_event_get(struct kvm_sbi_sse_context *ctx, u32 event_id)
{
	int i;
	struct kvm_sbi_sse_events *events;
	struct kvm_sbi_sse_event *event;

	if (EVENT_IS_GLOBAL(event_id)) {
		events = ctx->global;
	} else {
		events = &ctx->local;
	}

	for (i = 0; i < events->count; i++) {
		event = &events->events[i];
		if (sse_event_id(event) == event_id)
			return event;
	}

	if (sse_is_standard_event(event_id))
		return ERR_PTR(SBI_ERR_NOT_SUPPORTED);

	return ERR_PTR(SBI_ERR_INVALID_PARAM);
}

static int kvm_sbi_sse_event_register(struct kvm_sbi_sse_event *event,
				      unsigned long handler_entry_pc,
				      unsigned long handler_entry_arg)
{

	if (event->state != SBI_SSE_STATE_UNUSED)
		return SBI_ERR_INVALID_STATE;

	if (handler_entry_pc & 0x1)
		return SBI_ERR_INVALID_PARAM;

	event->state = SBI_SSE_STATE_REGISTERED;
	event->entry.arg = handler_entry_arg;
	event->entry.pc = handler_entry_pc;

	return SBI_SUCCESS;
}

static int kvm_sbi_sse_register(struct kvm_vcpu *vcpu, u32 event_id,
				unsigned long handler_entry_pc,
				unsigned long handler_entry_arg)
{
	struct kvm_sbi_sse_event *event;
	struct kvm_sbi_sse_context *ctx = vcpu_to_sbi_sse(vcpu);

	event = kvm_sbi_sse_event_get(ctx, event_id);
	if (IS_ERR(event))
		return PTR_ERR(event);

	scoped_guard(sse_event, event) {
		return kvm_sbi_sse_event_register(event, handler_entry_pc,
						handler_entry_arg);
	}
}

static int kvm_sbi_sse_event_unregister(struct kvm_vcpu *vcpu,
					struct kvm_sbi_sse_event *event)
{
	if (event->state != SBI_SSE_STATE_REGISTERED)
		return SBI_ERR_INVALID_STATE;

	event->state = SBI_SSE_STATE_UNUSED;

	return SBI_SUCCESS;
}

static int kvm_sbi_sse_unregister(struct kvm_vcpu *vcpu, u32 event_id)
{
	struct kvm_sbi_sse_event *event;
	struct kvm_sbi_sse_context *ctx = vcpu_to_sbi_sse(vcpu);

	event = kvm_sbi_sse_event_get(ctx, event_id);
	if (IS_ERR(event))
		return PTR_ERR(event);

	scoped_guard(sse_event, event) {
		return kvm_sbi_sse_event_unregister(vcpu, event);
	}
}

static int kvm_sbi_sse_enable(struct kvm_vcpu *vcpu, u32 event_id)
{
	struct kvm_sbi_sse_context *ctx = vcpu_to_sbi_sse(vcpu);
	struct kvm_vcpu *target_vcpu = vcpu;
	struct kvm_sbi_sse_event *event;

	event = kvm_sbi_sse_event_get(ctx, event_id);
	if (IS_ERR(event))
		return PTR_ERR(event);

	scoped_guard(sse_event, event) {
		if (event->state != SBI_SSE_STATE_REGISTERED)
			return SBI_ERR_INVALID_STATE;

		if (sse_event_is_global(event)) {
			target_vcpu = kvm_get_vcpu_by_id(vcpu->kvm, event->hartid);
			/*
			* If the requested vcpu was offlined in between, assign the
			* global event to the current vcpu.
			*/
			if (!target_vcpu) {
				target_vcpu = vcpu;
				event->hartid = vcpu->vcpu_id;
			}
		}
		ctx = vcpu_to_sbi_sse(target_vcpu);

		event->state = SBI_SSE_STATE_ENABLED;

		scoped_guard(enabled_events, ctx) {
			kvm_sbi_sse_event_insert_ordered(target_vcpu, event);
		}

		if (sse_event_is_global(event) && event->pending)
			kvm_make_request(KVM_REQ_SSE_EVENT_INJECT, target_vcpu);
	}

	return SBI_SUCCESS;
}

static int kvm_sbi_sse_event_disable(struct kvm_sbi_sse_event *event)
{
	if (event->state != SBI_SSE_STATE_ENABLED)
		return SBI_ERR_INVALID_STATE;

	list_del(&event->node);
	event->state = SBI_SSE_STATE_REGISTERED;

	return SBI_SUCCESS;
}

static int kvm_sbi_sse_disable(struct kvm_vcpu *vcpu, u32 event_id)
{
	struct kvm_sbi_sse_event *event;
	struct kvm_sbi_sse_context *ctx = vcpu_to_sbi_sse(vcpu);

	event = kvm_sbi_sse_event_get(ctx, event_id);
	if (IS_ERR(event))
		return PTR_ERR(event);

	scoped_guard(sse_event, event) {
		scoped_guard(enabled_events, ctx) {
			kvm_sbi_sse_event_disable(event);
		}
	}

	return SBI_SUCCESS;
}

static void kvm_sbi_sse_event_resume(struct kvm_cpu_context *cp,
				     struct kvm_sbi_sse_event *event)
{
	unsigned long vsstatus = csr_read(CSR_VSSTATUS);

	cp->sepc = csr_read(CSR_VSEPC);

	cp->sstatus &= ~SR_SPP;
	if (vsstatus & SR_SPP)
		cp->sstatus |= SR_SPP;

	vsstatus &= ~SR_SIE;
	if (vsstatus & SR_SPIE)
		vsstatus |= SR_SIE;

#define check_resume_flag(_flag) \
	vsstatus &= ~SR_##_flag; \
	if (event->interrupted.flags & SBI_SSE_ATTR_INTERRUPTED_FLAGS_SSTATUS_##_flag) \
		vsstatus |= SR_##_flag

	check_resume_flag(SPIE);
	check_resume_flag(SPP);
	check_resume_flag(SPELP);
	check_resume_flag(SDT);

	csr_write(CSR_VSSTATUS, vsstatus);
	csr_write(CSR_VSEPC, event->interrupted.sepc);

	cp->a7 = event->interrupted.a7;
	cp->a6 = event->interrupted.a6;
}

static int kvm_sbi_sse_event_complete(struct kvm_vcpu *vcpu,
				      struct kvm_sbi_sse_event *event,
				      struct kvm_vcpu_sbi_return *retdata)
{
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;

	if (event->state != SBI_SSE_STATE_RUNNING)
		return SBI_ERR_INVALID_STATE;

	if (event->hartid != vcpu->vcpu_id)
		return SBI_ERR_INVALID_PARAM;

	event->state = SBI_SSE_STATE_ENABLED;
	if (event->config == SBI_SSE_ATTR_CONFIG_ONESHOT)
		kvm_sbi_sse_event_disable(event);

	kvm_sbi_sse_event_resume(cp, event);
	retdata->skip_regs_update = true;

	return SBI_SUCCESS;
}

static int kvm_sbi_sse_complete(struct kvm_vcpu *vcpu,
				struct kvm_vcpu_sbi_return *retdata)
{
	int ret = SBI_SUCCESS;
	struct kvm_sbi_sse_event *event = NULL;
	struct kvm_sbi_sse_context *ctx = vcpu_to_sbi_sse(vcpu);

	scoped_guard(enabled_events, ctx) {
		list_for_each_entry(event, &ctx->enabled_event_list, node) {
			if (event->state == SBI_SSE_STATE_RUNNING) {
				ret = kvm_sbi_sse_event_complete(vcpu, event, retdata);
				break;
			}
		}
	}

	kvm_make_request(KVM_REQ_SSE_EVENT_INJECT, vcpu);

	return ret;
}

static unsigned long kvm_sbi_sse_interrupted_flags(unsigned long vsstatus)
{
	unsigned long flags = 0;

#define check_save_flag(_flag) \
	if (vsstatus & SR_##_flag) \
		flags |= SBI_SSE_ATTR_INTERRUPTED_FLAGS_SSTATUS_##_flag;

	check_save_flag(SPIE);
	check_save_flag(SPP);
	check_save_flag(SPELP);
	check_save_flag(SDT);

	return flags;
}

static void kvm_sbi_sse_inject(struct kvm_vcpu *vcpu,
			       struct kvm_sbi_sse_event *event)
{
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;
	unsigned long vsstatus = csr_read(CSR_VSSTATUS);

	event->state = SBI_SSE_STATE_RUNNING;
	event->pending = 0;

	event->interrupted.a6 = cp->a6;
	event->interrupted.a7 = cp->a7;
	event->interrupted.flags = kvm_sbi_sse_interrupted_flags(vsstatus);
	event->interrupted.sepc = csr_read(CSR_VSEPC);

	vsstatus &= ~(SR_SPP | SR_SPIE);
	if (cp->sstatus & SR_SPP)
		vsstatus |= SR_SPP;
	if (vsstatus & SR_SIE)
		vsstatus |= SR_SPIE;

	/* Clear Guest SSTATUS.SIE bit */
	vsstatus &= ~SR_SIE;

	/* Update Guest SSTATUS */
	csr_write(CSR_VSSTATUS, vsstatus);

	/* Update Guest SEPC with the interrupted pc */
	csr_write(CSR_VSEPC, cp->sepc);

	/* Setup entry context */
	cp->a6 = vcpu->vcpu_id;
	cp->a7 = event->entry.arg;
	cp->sepc = event->entry.pc;

	/* Return to VS-mode */
	cp->sstatus |= SR_SPP;
}

static bool kvm_sbi_sse_hart_id_valid(struct kvm *kvm,
				      unsigned long hart_id)
{
	struct kvm_vcpu *vcpu;

	/*
	 * Quick check for hart validity, this will be checked again once
	 * enabling the event on the vcpu
	 */
	vcpu = kvm_get_vcpu_by_id(kvm, hart_id);
	if (!vcpu)
		return false;

	scoped_guard(spinlock, &vcpu->arch.mp_state_lock) {
		return !kvm_riscv_vcpu_stopped(vcpu);
	}
}

static int kvm_sbi_sse_inject_event_to_hart(struct kvm_vcpu *vcpu,
					    uint32_t event_id,
					    unsigned long hartid,
					    struct kvm_vcpu_sbi_return *retdata)
{
	struct kvm_sbi_sse_context *ctx;
	struct kvm_sbi_sse_event *event;

	/* Request to inject a local event on another CPU */
	if (!EVENT_IS_GLOBAL(event_id) && hartid != vcpu->vcpu_id)
		vcpu = kvm_get_vcpu_by_id(vcpu->kvm, hartid);

	ctx = vcpu_to_sbi_sse(vcpu);

	event = kvm_sbi_sse_event_get(ctx, event_id);
	if (IS_ERR(event))
		return PTR_ERR(event);

	scoped_guard(sse_event, event) {
		if (event->state == SBI_SSE_STATE_UNUSED)
			return SBI_ERR_INVALID_STATE;

		/* In case of global event, provided hart_id is ignored */
		if (sse_event_is_global(event)) {
			hartid = event->hartid;
			vcpu = kvm_get_vcpu_by_id(vcpu->kvm, hartid);
			if (!vcpu)
				return SBI_ERR_FAILURE;
		}

		event->pending = 1;
		kvm_make_request(KVM_REQ_SSE_EVENT_INJECT, vcpu);
		kvm_vcpu_kick(vcpu);
	}


	return SBI_SUCCESS;
}

static int kvm_sbi_sse_inject_from_ecall(struct kvm_vcpu *vcpu, u32 event_id,
					 unsigned long hart_id,
					 struct kvm_vcpu_sbi_return *retdata)
{
	if (!kvm_sbi_sse_hart_id_valid(vcpu->kvm, hart_id))
		return SBI_ERR_INVALID_PARAM;

	return kvm_sbi_sse_inject_event_to_hart(vcpu, event_id, hart_id, retdata);
}

int kvm_sbi_sse_inject_event(struct kvm_vcpu *vcpu, u32 event_id)
{
	int ret;
	/* We don't really care about return value here */
	struct kvm_vcpu_sbi_return out;

	ret = kvm_sbi_sse_inject_event_to_hart(vcpu, event_id, vcpu->vcpu_id, &out);

	return sbi_err_map_linux_errno(ret);
}

static int sbi_sse_event_access_attr_buf(struct kvm_vcpu *vcpu, gpa_t ga,
					 size_t size, bool read, u8 *attrs)
{
	unsigned long hva;
	bool writable;

	hva = kvm_vcpu_gfn_to_hva_prot(vcpu, ga >> PAGE_SHIFT, &writable);
	if (kvm_is_error_hva(hva))
		return SBI_ERR_INVALID_ADDRESS;

	if (read) {
		return kvm_vcpu_read_guest(vcpu, ga, attrs, size);
	} else {
		if (!writable)
			return SBI_ERR_INVALID_ADDRESS;

		return kvm_vcpu_write_guest(vcpu, ga, attrs, size);
	}
}

static int sbi_sse_event_access_attr(struct kvm_vcpu *vcpu,
				     u32 attr_count, unsigned long phys_lo,
				     unsigned long phys_hi, bool read,
				     u8 *attrs)
{
	gpa_t ga = phys_lo;
	size_t size = attr_count * sizeof(unsigned long);

	if (!IS_ENABLED(CONFIG_64BIT))
		ga |= (gpa_t)phys_hi << 32;

	return sbi_sse_event_access_attr_buf(vcpu, ga, size, read, (u8 *)attrs);
}

static int sbi_sse_attr_check(u32 base_attr_id, u32 attr_count,
			      unsigned long phys_lo)
{
	const unsigned align = __riscv_xlen >> 3;
	uint64_t end_id = (uint64_t)base_attr_id + (attr_count - 1);

	if (attr_count == 0)
		return SBI_ERR_INVALID_PARAM;

	if (end_id >= SBI_SSE_ATTR_MAX)
		return SBI_ERR_BAD_RANGE;

	if (!IS_ALIGNED(phys_lo, align))
		return SBI_ERR_INVALID_ADDRESS;

	return 0;
}

static int sbi_sse_read_attrs(struct kvm_vcpu *vcpu,
			      struct kvm_sbi_sse_event *event, u32 base_attr_id,
			      u32 attr_count, unsigned long phys_lo,
			      unsigned long phys_hi)
{
	u32 attr, attr_id;
	unsigned long attrs_buf[SBI_SSE_ATTR_MAX] = {0};

	for (attr = 0; attr < attr_count; attr++) {
		attr_id = base_attr_id + attr;
		switch (attr_id) {
		case SBI_SSE_ATTR_STATUS:
			attrs_buf[attr] = event->state |
					  (event->pending << SBI_SSE_ATTR_STATUS_PENDING_OFFSET) |
					  BIT(SBI_SSE_ATTR_STATUS_INJECT_OFFSET);
			break;
		case SBI_SSE_ATTR_PRIO:
			attrs_buf[attr] = event->prio;
			break;
		case SBI_SSE_ATTR_CONFIG:
			attrs_buf[attr] = event->config;
			break;
		case SBI_SSE_ATTR_PREFERRED_HART:
			attrs_buf[attr] = event->hartid;
			break;
		case SBI_SSE_ATTR_ENTRY_PC:
			attrs_buf[attr] = event->entry.pc;
			break;
		case SBI_SSE_ATTR_ENTRY_ARG:
			attrs_buf[attr] = event->entry.arg;
			break;
		case SBI_SSE_ATTR_INTERRUPTED_SEPC:
			attrs_buf[attr] = event->interrupted.sepc;
			break;
		case SBI_SSE_ATTR_INTERRUPTED_FLAGS:
			attrs_buf[attr] = event->interrupted.flags;
			break;
		case SBI_SSE_ATTR_INTERRUPTED_A6:
			attrs_buf[attr] = event->interrupted.a6;
			break;
		case SBI_SSE_ATTR_INTERRUPTED_A7:
			attrs_buf[attr] = event->interrupted.a7;
			break;
		}
	}

	return sbi_sse_event_access_attr(vcpu, attr_count, phys_lo, phys_hi,
					 false, (u8 *)attrs_buf);
}

static int kvm_sbi_sse_read_attrs(struct kvm_vcpu *vcpu, u32 event_id,
				  u32 base_attr_id, u32 attr_count,
				  unsigned long phys_lo, unsigned long phys_hi)
{
	int ret;
	struct kvm_sbi_sse_event *event;
	struct kvm_sbi_sse_context *ctx = vcpu_to_sbi_sse(vcpu);

	event = kvm_sbi_sse_event_get(ctx, event_id);
	if (IS_ERR(event))
		return PTR_ERR(event);

	scoped_guard(sse_event, event) {
		ret = sbi_sse_attr_check(base_attr_id, attr_count, phys_lo);
		if (ret)
			return ret;

		return sbi_sse_read_attrs(vcpu, event, base_attr_id, attr_count,
					  phys_lo, phys_hi);
	}
}

static int sbi_sse_attr_write_check(struct kvm_vcpu *vcpu,
				    struct kvm_sbi_sse_event *event,
				    u32 attr, unsigned long val)
{
	switch (attr) {
	case SBI_SSE_ATTR_CONFIG:
		if (event->state >= SBI_SSE_STATE_ENABLED)
			return SBI_ERR_INVALID_STATE;

		if (val & ~SBI_SSE_ATTR_CONFIG_ONESHOT)
			return SBI_ERR_INVALID_PARAM;

		return SBI_SUCCESS;
	case SBI_SSE_ATTR_PRIO:
		if (event->state >= SBI_SSE_STATE_ENABLED)
			return SBI_ERR_INVALID_STATE;

#if __riscv_xlen > 32
		if (val != (u32)val)
			return SBI_ERR_INVALID_PARAM;
#endif
		return SBI_SUCCESS;
	case SBI_SSE_ATTR_PREFERRED_HART:
		if (event->state >= SBI_SSE_STATE_ENABLED)
			return SBI_ERR_INVALID_STATE;

		if (!sse_event_is_global(event))
			return SBI_ERR_DENIED;

		if (!kvm_sbi_sse_hart_id_valid(vcpu->kvm, val))
			return SBI_ERR_INVALID_PARAM;

		return SBI_SUCCESS;
	case SBI_SSE_ATTR_INTERRUPTED_FLAGS:
		if (val & ~(SBI_SSE_ATTR_INTERRUPTED_FLAGS_SSTATUS_SPP |
			    SBI_SSE_ATTR_INTERRUPTED_FLAGS_SSTATUS_SPIE |
			    SBI_SSE_ATTR_INTERRUPTED_FLAGS_HSTATUS_SPV |
			    SBI_SSE_ATTR_INTERRUPTED_FLAGS_HSTATUS_SPVP |
			    SBI_SSE_ATTR_INTERRUPTED_FLAGS_SSTATUS_SPELP |
			    SBI_SSE_ATTR_INTERRUPTED_FLAGS_SSTATUS_SDT))
			return SBI_ERR_INVALID_PARAM;
		fallthrough;
	case SBI_SSE_ATTR_INTERRUPTED_SEPC:
	case SBI_SSE_ATTR_INTERRUPTED_A6:
	case SBI_SSE_ATTR_INTERRUPTED_A7:
		if (event->state != SBI_SSE_STATE_RUNNING)
			return SBI_ERR_INVALID_STATE;

		if (vcpu->vcpu_id != event->hartid)
			return SBI_ERR_INVALID_PARAM;

		return SBI_SUCCESS;
	default:
		return SBI_ERR_DENIED;
	}

	return SBI_SUCCESS;
}

static int sbi_sse_attr_write(struct kvm_vcpu *vcpu,
			      struct kvm_sbi_sse_event *event,
			      u32 attr, unsigned long val)
{
	switch (attr) {
	case SBI_SSE_ATTR_PRIO:
		event->prio = (u32)val;
		break;
	case SBI_SSE_ATTR_CONFIG:
		event->config = val;
		break;
	case SBI_SSE_ATTR_PREFERRED_HART:
		event->hartid = val;
		break;
	case SBI_SSE_ATTR_INTERRUPTED_SEPC:
		event->interrupted.sepc = val;
		break;
	case SBI_SSE_ATTR_INTERRUPTED_FLAGS:
		event->interrupted.flags = val;
		break;
	case SBI_SSE_ATTR_INTERRUPTED_A6:
		event->interrupted.a6 = val;
		break;
	case SBI_SSE_ATTR_INTERRUPTED_A7:
		event->interrupted.a7 = val;
		break;
	}

	return SBI_SUCCESS;
}

static int sbi_sse_write_attrs(struct kvm_vcpu *vcpu,
			       struct kvm_sbi_sse_event *event,
			       u32 base_attr_id, u32 attr_count,
			       unsigned long phys_lo, unsigned long phys_hi)
{
	unsigned long attrs_buf[SBI_SSE_ATTR_MAX] = {0};
	int ret;
	u32 i;

	ret = sbi_sse_event_access_attr(vcpu, attr_count, phys_lo, phys_hi,
					 true, (u8 *)attrs_buf);
	if (ret)
		return ret;

	for (i = 0; i < attr_count; i++) {
		ret = sbi_sse_attr_write_check(vcpu, event, base_attr_id + i, attrs_buf[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < attr_count; i++) {
		ret = sbi_sse_attr_write(vcpu, event, base_attr_id + i, attrs_buf[i]);
		if (ret)
			return ret;
	}

	return SBI_SUCCESS;
}
static int kvm_sbi_sse_write_attrs(struct kvm_vcpu *vcpu, u32 event_id,
			    u32 base_attr_id, u32 attr_count,
			    unsigned long phys_lo,
			    unsigned long phys_hi)
{
	struct kvm_sbi_sse_context *ctx = vcpu_to_sbi_sse(vcpu);
	struct kvm_sbi_sse_event *event;
	int ret;

	event = kvm_sbi_sse_event_get(ctx, event_id);
	if (IS_ERR(event))
		return PTR_ERR(event);

	scoped_guard(sse_event, event) {
		ret = sbi_sse_attr_check(base_attr_id, attr_count, phys_lo);
		if (ret)
			return ret;

		return sbi_sse_write_attrs(vcpu, event, base_attr_id, attr_count,
					  phys_lo, phys_hi);
	}
}

static bool kvm_sbi_sse_event_is_ready(struct kvm_sbi_sse_event *event)
{
	return event->pending && event->state == SBI_SSE_STATE_ENABLED;
}

static bool kvm_sbi_sse_event_check_inject(struct kvm_vcpu *vcpu,
					  struct kvm_sbi_sse_event *event)
{
	/*
	 * List of event is ordered by priority, stop at first running
	 * event since all other events after this one are of lower
	 * priority. This means an event of higher priority is already
	 * running.
	 */
	if (event->state == SBI_SSE_STATE_RUNNING)
		return true;

	if (kvm_sbi_sse_event_is_ready(event)) {
		kvm_sbi_sse_inject(vcpu, event);
		return true;
	}

	return false;
}

static int kvm_sbi_sse_hart_unmask(struct kvm_vcpu *vcpu)
{
	struct kvm_sbi_sse_context *ctx = vcpu_to_sbi_sse(vcpu);

	if (!ctx->masked)
		return SBI_ERR_ALREADY_STARTED;

	ctx->masked = false;
	kvm_make_request(KVM_REQ_SSE_EVENT_INJECT, vcpu);

	return SBI_SUCCESS;
}

static int kvm_sbi_sse_hart_mask(struct kvm_vcpu *vcpu)
{
	struct kvm_sbi_sse_context *ctx = vcpu_to_sbi_sse(vcpu);

	if (ctx->masked)
		return SBI_ERR_ALREADY_STOPPED;

	ctx->masked = true;

	return SBI_SUCCESS;
}

void kvm_sbi_sse_process_pending_events(struct kvm_vcpu *vcpu)
{
	struct kvm_sbi_sse_context *ctx = vcpu_to_sbi_sse(vcpu);
	struct kvm_sbi_sse_event *event;

	if (ctx->masked)
		return;

	scoped_guard(enabled_events, ctx) {
		list_for_each_entry(event, &ctx->enabled_event_list, node) {
			if (kvm_sbi_sse_event_check_inject(vcpu, event))
				return;
		}
	}
}

static int kvm_sbi_ext_sse_handler(struct kvm_vcpu *vcpu, struct kvm_run *run,
				   struct kvm_vcpu_sbi_return *retdata)
{
	struct kvm_cpu_context *cp = &vcpu->arch.guest_context;
	unsigned long funcid = cp->a6;
	int ret = 0;

	switch (funcid) {
	case SBI_SSE_EVENT_ATTR_READ:
		ret = kvm_sbi_sse_read_attrs(vcpu, cp->a0, cp->a1, cp->a2,
					     cp->a3, cp->a4);
		break;
	case SBI_SSE_EVENT_ATTR_WRITE:
		ret = kvm_sbi_sse_write_attrs(vcpu, cp->a0, cp->a1, cp->a2,
					      cp->a3, cp->a4);
		break;
	case SBI_SSE_EVENT_REGISTER:
		ret = kvm_sbi_sse_register(vcpu, cp->a0, cp->a1, cp->a2);
		break;
	case SBI_SSE_EVENT_UNREGISTER:
		ret = kvm_sbi_sse_unregister(vcpu, cp->a0);
		break;
	case SBI_SSE_EVENT_ENABLE:
		ret = kvm_sbi_sse_enable(vcpu, cp->a0);
		break;
	case SBI_SSE_EVENT_DISABLE:
		ret = kvm_sbi_sse_disable(vcpu, cp->a0);
		break;
	case SBI_SSE_EVENT_COMPLETE:
		ret = kvm_sbi_sse_complete(vcpu, retdata);
		break;
	case SBI_SSE_EVENT_HART_UNMASK:
		ret = kvm_sbi_sse_hart_unmask(vcpu);
		break;
	case SBI_SSE_EVENT_HART_MASK:
		ret = kvm_sbi_sse_hart_mask(vcpu);
		break;
	case SBI_SSE_EVENT_SIGNAL:
		ret = kvm_sbi_sse_inject_from_ecall(vcpu, cp->a0, cp->a1, retdata);
		break;
	default:
		ret = SBI_ERR_NOT_SUPPORTED;
	}

	retdata->err_val = ret;

	return 0;
}

static void kvm_sbi_sse_deinit(struct kvm_vcpu *vcpu)
{
	struct kvm_sbi_sse_context *ctx = vcpu_to_sbi_sse(vcpu);

	/* FIXME: check for global events still assigned to this vcpu */
	kfree(ctx->local.events);
}

int kvm_sbi_sse_add_event(u32 event_id, struct kvm_sbi_sse_event_ops *ops, void *priv)
{
	struct sse_event_desc_list *descs = &event_descs[EVENT_IS_GLOBAL(event_id)];
	struct kvm_sbi_sse_event_desc *desc;

	scoped_guard(mutex, &event_descs_lock) {
		list_for_each_entry(desc, &descs->list, node) {
			if (desc->event_id == event_id)
				return -EEXIST;
		}

		desc = kzalloc(sizeof(*desc), GFP_KERNEL);
		if (!desc)
			return -ENOMEM;

		descs->count++;

		desc->event_id = event_id;
		desc->ops = ops;
		desc->priv = priv;
		list_add(&desc->node, &descs->list);
	}

	return 0;
}

int kvm_riscv_sbi_sse_init(void)
{
	int ret;
	u32 default_events[] = {SBI_SSE_EVENT_LOCAL_SOFTWARE_INJECTED,
			       SBI_SSE_EVENT_GLOBAL_SOFTWARE_INJECTED};

	for (int i = 0; i < ARRAY_SIZE(default_events); i++) {
		ret = kvm_sbi_sse_add_event(default_events[i], NULL, NULL);
		if (ret) {
			kvm_riscv_sbi_sse_exit();
			return ret;
		}
	}

	return 0;
}

void kvm_riscv_sbi_sse_exit(void)
{
	struct kvm_sbi_sse_event_desc *desc, *tmp;
	struct sse_event_desc_list *descs;
	int i = 0;

	scoped_guard(mutex, &event_descs_lock) {
		for (i = 0; i < EVENT_DESC_COUNT; i++) {
			descs = &event_descs[i];
			list_for_each_entry_safe(desc, tmp, &descs->list, node) {
				kfree(desc);
			}
			descs->count = 0;
		}
	}
}

static int kvm_sbi_events_alloc(struct kvm_sbi_sse_events *event, unsigned int count)
{
	event->count = count;
	event->events = kcalloc(count, sizeof(struct kvm_sbi_sse_event), GFP_KERNEL);
	if (!event->events)
		return -ENOMEM;

	return 0;
}

void kvm_sbi_sse_global_destroy_vm(struct kvm *kvm)
{
	struct kvm_sbi_sse *global = &kvm->arch.sse;

	kfree(global->events.events);
	global->init = false;
}

int kvm_sbi_sse_global_init_vm(struct kvm *kvm)
{
	struct sse_event_desc_list *descs = &event_descs[EVENT_DESC_GLOBAL];
	struct kvm_sbi_sse *global = &kvm->arch.sse;

	scoped_guard(mutex, &event_descs_lock) {
		return kvm_sbi_events_alloc(&global->events, descs->count);
	}
}

static void kvm_sbi_sse_init_events(struct kvm_vcpu *vcpu, struct kvm_sbi_sse_event *events,
				    enum event_desc_type event_type)
{
	struct sse_event_desc_list *descs = &event_descs[event_type];
	struct kvm_sbi_sse_event_desc *desc;
	struct kvm_sbi_sse_event *event;
	int i = 0;

	list_for_each_entry(desc, &descs->list, node) {
		event = &events[i];
		event->desc = desc;
		event->hartid = vcpu->vcpu_id;
		mutex_init(&event->lock);
		i++;
	}
}

static int kvm_sbi_sse_init(struct kvm_vcpu *vcpu)
{
	struct kvm_sbi_sse *global = &vcpu->kvm->arch.sse;
	struct kvm_sbi_sse_context *ctx = vcpu_to_sbi_sse(vcpu);
	int ret;

	INIT_LIST_HEAD(&ctx->enabled_event_list);
	ctx->global = &global->events;
	ctx->masked = true;
	mutex_init(&ctx->enabled_lock);

	scoped_guard(mutex, &event_descs_lock) {
		ret = kvm_sbi_events_alloc(&ctx->local, event_descs[EVENT_DESC_LOCAL].count);
		if (ret)
			return ret;

		kvm_sbi_sse_init_events(vcpu, ctx->local.events, EVENT_DESC_LOCAL);

		/* Assign global events to first vcpu only */
		if (!global->init) {
			kvm_sbi_sse_init_events(vcpu, global->events.events, EVENT_DESC_GLOBAL);
			global->init = true;
		}
	}

	return 0;
}

const struct kvm_vcpu_sbi_extension vcpu_sbi_ext_sse = {
	.extid_start = SBI_EXT_SSE,
	.extid_end = SBI_EXT_SSE,
	.handler = kvm_sbi_ext_sse_handler,
	.init = kvm_sbi_sse_init,
	.deinit = kvm_sbi_sse_deinit,
};

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Rivos Inc.
 */

#define pr_fmt(fmt) "sse_test: " fmt

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/riscv_sse.h>
#include <linux/slab.h>
#include <linux/smp.h>

#include <asm/sbi.h>
#include <asm/sse.h>

static struct sbiret sbi_sse_ecall(int fid, unsigned long arg0,
				   unsigned long arg1)
{
	return sbi_ecall(SBI_EXT_SSE, fid, arg0, arg1, 0, 0, 0, 0);
}

static bool sse_event_is_global(u32 evt)
{
	return !!(evt & SBI_SSE_EVENT_GLOBAL);
}

static int sse_event_attr_get(u32 evt, unsigned long attr_id,
			      unsigned long *val)
{
	struct sbiret sret;
	unsigned long *attr_buf, phys;

	attr_buf = kmalloc(sizeof(unsigned long), GFP_KERNEL);
	if (!attr_buf)
		return -ENOMEM;

	phys = virt_to_phys(attr_buf);

	sret = sbi_ecall(SBI_EXT_SSE, SBI_SSE_EVENT_ATTR_READ, evt,
				     attr_id, 1, phys, 0, 0);
	if (sret.error) {
		pr_err("Failed to get event %x attr %lx, error %ld\n", evt,
		       attr_id, sret.error);
		return sbi_err_map_linux_errno(sret.error);
	}

	*val = *attr_buf;

	return 0;
}

static bool sse_test_can_inject_event(u32 evt)
{
	int ret;
	unsigned long val;

	/* Check if injection is supported */
	ret = sse_event_attr_get(evt, SBI_SSE_ATTR_STATUS, &val);
	if (ret) {
		pr_err("Event %x get injection attribute failed, error %d\n",
		       evt, ret);
		return false;
	}

	return !!(val & BIT(SBI_SSE_ATTR_STATUS_INJECT_OFFSET));
}

static int sse_test_signal(u32 evt, unsigned int cpu)
{
	unsigned int hart_id = cpuid_to_hartid_map(cpu);
	struct sbiret ret;

	ret = sbi_sse_ecall(SBI_SSE_EVENT_SIGNAL, evt, hart_id);
	if (ret.error) {
		pr_err("Failed to signal event %x, error %ld\n", evt,
		       ret.error);
		return sbi_err_map_linux_errno(ret.error);
	}

	return 0;
}

static int sse_test_inject_event(struct sse_event *event, u32 evt,
				 unsigned int cpu)
{
	int res;
	unsigned long status;

	if (sse_event_is_global(evt)) {
		/*
		 * Due to the fact the completion might happen faster than
		 * the call to SBI_SSE_COMPLETE in the handler, if the event was
		 * running on another CPU, we need to wait for the event status
		 * to be !RUNNING.
		 */
		do {
			res = sse_event_attr_get(evt, SBI_SSE_ATTR_STATUS, &status);
			if (res) {
				pr_err("Failed to get status for evt %x, error %d\n", evt,
					res);
				return res;
			}
			status = status & SBI_SSE_ATTR_STATUS_STATE_MASK;
		} while (status == SBI_SSE_STATE_RUNNING);

		res = sse_event_set_target_cpu(event, cpu);
		if (res) {
			pr_err("Failed to set cpu for evt %x, error %d\n", evt,
				res);
			return res;
		}
	}

	return sse_test_signal(evt, cpu);
}

static const char *sse_evt_name(u32 evt)
{
	switch (evt) {
	case SBI_SSE_EVENT_LOCAL_RAS:
		return "RAS";
	case SBI_SSE_EVENT_LOCAL_PMU:
		return "PMU";
	case SBI_SSE_EVENT_GLOBAL_RAS:
		return "GLOBAL_RAS";
	case SBI_SSE_EVENT_LOCAL_SOFTWARE:
		return "SW";
	case SBI_SSE_EVENT_GLOBAL_SOFTWARE:
		return "GLOBAL_SW";
	}

	return 0;
}

struct fast_test_arg {
	u32 evt;
	int cpu;
	struct completion completion;
};

static int sse_test_handler(u32 evt, void *arg, struct pt_regs *regs)
{
	int ret = 0;
	struct fast_test_arg *targ = arg;

	if (evt != targ->evt) {
		pr_err("Received SSE event id %x instead of %x\n", targ->evt,
		       evt);
		ret = -EINVAL;
	}

	if (targ->cpu != smp_processor_id()) {
		pr_err("Received SSE event %d on CPU %d instead of %d\n",
		       evt, smp_processor_id(), targ->cpu);
		ret = -EINVAL;
	}

	complete(&targ->completion);

	return ret;
}

static void sse_test_injection_fast(void)
{
	int i, ret, cpu, j;
	u32 evt;
	struct fast_test_arg test_arg;
	struct sse_event *event;
	u32 events[] = {
		SBI_SSE_EVENT_LOCAL_RAS,
		SBI_SSE_EVENT_GLOBAL_RAS,
		SBI_SSE_EVENT_LOCAL_PMU,
		SBI_SSE_EVENT_LOCAL_SOFTWARE,
		SBI_SSE_EVENT_GLOBAL_SOFTWARE,
	};

	pr_info("Starting SSE test (fast)\n");
	init_completion(&test_arg.completion);

	for (i = 0; i < ARRAY_SIZE(events); i++) {
		evt = events[i];
		test_arg.evt = evt;

		if (!sse_test_can_inject_event(evt)) {
			pr_err("Can not inject event %s, skipping\n", sse_evt_name(evt));
			continue;
		}

		event = sse_event_register(evt, 0, sse_test_handler,
					   (void *) &test_arg);
		if (IS_ERR(event)) {
			pr_err("Failed to register SSE event %s\n", sse_evt_name(evt));
			continue;
		}

		ret = sse_event_enable(event);
		if (ret) {
			pr_err("Failed to enable SSE event %s\n", sse_evt_name(evt));
			continue;
		}

		pr_err("Starting handling event %s\n", sse_evt_name(evt));
		for (j = 0; j < 1000; j++) {
			for_each_online_cpu(cpu) {
				test_arg.cpu = cpu;
				/* Test arg is used on another CPU */
				smp_wmb();

				ret = sse_test_inject_event(event, evt, cpu);
				if (ret) {
					pr_err("SSE event %s injection failed\n",
					       sse_evt_name(evt));
					continue;
				}

				ret = wait_for_completion_timeout(&test_arg.completion,
								msecs_to_jiffies(100));
				if (!ret) {
					panic("Failed to wait for event %s completion on CPU %d\n",
					sse_evt_name(evt), cpu);
				}

				reinit_completion(&test_arg.completion);
			}
		}

		pr_err("Finished handling event %s\n", sse_evt_name(evt));

		sse_event_disable(event);
		sse_event_unregister(event);

	}
	pr_info("Finished SSE test (fast)\n");
}

struct priority_test_arg {
	unsigned long evt;
	struct sse_event *event;
	bool called;
	u32 prio;
	struct priority_test_arg *next_evt_arg;
	void (*check_func)(struct priority_test_arg *arg);
};

static int sse_hi_priority_test_handler(u32 evt, void *arg, struct pt_regs *regs)
{
	struct priority_test_arg *targ = arg;
	struct priority_test_arg *next = targ->next_evt_arg;

	targ->called = 1;

	if (next) {
		sse_test_signal(next->evt, smp_processor_id());
		if (!next->called)
			panic("Higher priority event was not handled\n");
	}

	return 0;
}

static int sse_low_priority_test_handler(u32 evt, void *arg, struct pt_regs *regs)
{
	struct priority_test_arg *targ = arg;
	struct priority_test_arg *next = targ->next_evt_arg;

	targ->called = 1;

	if (next) {
		sse_test_signal(next->evt, smp_processor_id());
		if (next->called)
			panic("Lower priority event %s was handle before %s\n",
			      sse_evt_name(next->evt), sse_evt_name(evt));
	}

	return 0;
}

static void sse_test_injection_priority_arg(struct priority_test_arg *args,
					    unsigned int args_size,
					    sse_event_handler handler,
					    const char *test_name)
{
	unsigned int i;
	int ret;
	struct sse_event *event;
	struct priority_test_arg *arg;

	pr_info("Starting SSE priority test (%s)\n", test_name);
	for (i = 0; i < args_size; i++) {
		arg = &args[i];

		if (i < (args_size - 1))
			arg->next_evt_arg = &args[i + 1];
		else
			arg->next_evt_arg = NULL;

		event = sse_event_register(arg->evt, arg->prio, handler,
					   (void *) arg);
		if (IS_ERR(event)) {
			pr_err("Failed to register SSE event %s\n", sse_evt_name(arg->evt));
			continue;
		}
		sse_event_set_target_cpu(event, smp_processor_id());

		arg->event = event;

		sse_event_enable(event);
	}

	/* Inject first event */
	arg = &args[0];
	ret = sse_test_inject_event(arg->event, arg->evt, smp_processor_id());
	if (ret) {
		pr_err("SSE event %s injection failed\n", sse_evt_name(arg->evt));
		return;
	}

	for (i = 0; i < args_size; i++) {
		arg = &args[i];
		if (!arg->called)
			panic("Event %s handler was not called\n", sse_evt_name(arg->evt));

		event = arg->event;
		sse_event_disable(event);
		sse_event_unregister(event);
	}

	pr_info("Finished SSE test (%s)\n", test_name);
}

static void sse_test_injection_priority(void)
{
	struct priority_test_arg hi_prio_args[] = {
		{.evt = SBI_SSE_EVENT_GLOBAL_SOFTWARE},
		{.evt = SBI_SSE_EVENT_LOCAL_SOFTWARE},
		{.evt = SBI_SSE_EVENT_LOCAL_PMU},
		{.evt = SBI_SSE_EVENT_GLOBAL_RAS},
		{.evt = SBI_SSE_EVENT_LOCAL_RAS},
	};

	struct priority_test_arg low_prio_args[] = {
		{.evt = SBI_SSE_EVENT_LOCAL_RAS},
		{.evt = SBI_SSE_EVENT_GLOBAL_RAS},
		{.evt = SBI_SSE_EVENT_LOCAL_PMU},
		{.evt = SBI_SSE_EVENT_LOCAL_SOFTWARE},
		{.evt = SBI_SSE_EVENT_GLOBAL_SOFTWARE},

	};
	struct priority_test_arg prio_args[] = {
		{.evt = SBI_SSE_EVENT_GLOBAL_SOFTWARE, .prio = 5},
		{.evt = SBI_SSE_EVENT_LOCAL_SOFTWARE, .prio = 10},
		{.evt = SBI_SSE_EVENT_LOCAL_PMU, .prio = 15},
		{.evt = SBI_SSE_EVENT_GLOBAL_RAS, .prio = 20},
		{.evt = SBI_SSE_EVENT_LOCAL_RAS, .prio = 25},
	};

	struct priority_test_arg same_prio_args[] = {
		{.evt = SBI_SSE_EVENT_LOCAL_PMU, .prio = 0},
		{.evt = SBI_SSE_EVENT_LOCAL_RAS, .prio = 10},
		{.evt = SBI_SSE_EVENT_LOCAL_SOFTWARE, .prio = 10},
		{.evt = SBI_SSE_EVENT_GLOBAL_SOFTWARE, .prio = 10},
		{.evt = SBI_SSE_EVENT_GLOBAL_RAS, .prio = 20},
	};

	sse_test_injection_priority_arg(hi_prio_args, ARRAY_SIZE(hi_prio_args),
					sse_hi_priority_test_handler, "high");

	sse_test_injection_priority_arg(low_prio_args, ARRAY_SIZE(low_prio_args),
					sse_low_priority_test_handler, "low");

	sse_test_injection_priority_arg(prio_args, ARRAY_SIZE(prio_args),
					sse_low_priority_test_handler, "changed");

	sse_test_injection_priority_arg(same_prio_args, ARRAY_SIZE(same_prio_args),
					sse_low_priority_test_handler, "same_prio_args");
}

void __init sse_test_init(void)
{
	sse_test_injection_fast();
	sse_test_injection_priority();
}

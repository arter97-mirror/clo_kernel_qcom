// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#include <trace/hooks/cpufreq.h>

#include "walt.h"

struct walt_cpu_cycle {
	rwlock_t	lock;
	u64		cycles;
	u64		last_time_ns;
	unsigned int	cur_freq_khz;
	unsigned int	mult_fact;
};

static DEFINE_PER_CPU(struct walt_cpu_cycle, walt_cc);

/*
 * Convert a frequency in kHz into the multiplier used by the cycle
 * computations below, which do:
 *
 *	cycles = (delta_ns * mult_fact) >> 20
 *
 * i.e. mult_fact is freq_khz scaled by 2^20 / 10^6, applied as two
 * 1024/1000 steps, so the >> 20 above is 2 * SCHED_CAPACITY_SHIFT.
 *
 * Both shifts must be evaluated in 64 bits.  Shift operators do not apply the
 * usual arithmetic conversions - the result type is that of the promoted left
 * operand (C11 6.5.7p3) - so shifting a 32-bit value here wraps modulo 2^32
 * from 4,096,000 kHz upwards.  That wrap is well defined for unsigned types,
 * so it draws no warning, and it silently under-reports cycles on parts whose
 * fmax exceeds ~4.1 GHz (Glymur's gold cluster tops out at 4,723,200 kHz,
 * where the counter advanced 8.9x too slowly).
 */
static inline unsigned int walt_cycle_mult_fact(unsigned int freq_khz)
{
	u64 mult_fact;

	mult_fact = div_u64((u64)freq_khz << SCHED_CAPACITY_SHIFT, 1000);
	mult_fact = div_u64(mult_fact << SCHED_CAPACITY_SHIFT, 1000);

	return mult_fact;
}

static u64 walt_compute_cpu_cycles(struct walt_cpu_cycle *wcc, u64 wc)
{
	unsigned long flags;
	u64 delta;
	u64 ret;

	/*
	 * freq is in KHz. so multiply by 1000.
	 * time in nsec. so divide by NSEC_PER_SEC.
	 *
	 * cycles = (freq * 1000) * (t/10^9)
	 *        = (freq * t)/10^6
	 *
	 */
	read_lock_irqsave(&wcc->lock, flags);
	delta = wc - wcc->last_time_ns;
	ret = wcc->cycles + ((delta * wcc->mult_fact) >> 20);
	read_unlock_irqrestore(&wcc->lock, flags);

	return ret;
}

static void update_walt_compute_cpu_cycles(struct walt_cpu_cycle *wcc, u64 wc)
{
	unsigned long flags;
	u64 delta;

	/*
	 * freq is in KHz. so multiply by 1000.
	 * time in nsec. so divide by NSEC_PER_SEC.
	 *
	 * cycles = (freq * 1000) * (t/10^9)
	 *        = (freq * t)/10^6
	 *
	 */
	write_lock_irqsave(&wcc->lock, flags);
	delta = wc - wcc->last_time_ns;
	wcc->cycles += (delta * wcc->mult_fact) >> 20;
	wcc->last_time_ns = wc;
	write_unlock_irqrestore(&wcc->lock, flags);
}

u64 walt_cpu_cycle_counter(int cpu, u64 wc)
{
	struct walt_cpu_cycle *wcc = &per_cpu(walt_cc, cpu);
	u64 cycles;

	cycles = walt_compute_cpu_cycles(wcc, wc);

	return cycles;
}

static void walt_cpufreq_transition(void *unused, struct cpufreq_policy *policy)
{
	int i;
	struct walt_cpu_cycle *wcc;
	u64 wc;
	unsigned int mult_fact;

	wc = sched_clock();
	for_each_cpu(i, policy->related_cpus) {
		wcc = &per_cpu(walt_cc, i);
		update_walt_compute_cpu_cycles(wcc, wc);
		wcc->cur_freq_khz = policy->cur;
	}

	mult_fact = walt_cycle_mult_fact(policy->cur);
	for_each_cpu(i, policy->related_cpus) {
		wcc = &per_cpu(walt_cc, i);
		wcc->mult_fact = mult_fact;
	}

}

void walt_cycle_counter_init(void)
{
	int i;

	for_each_possible_cpu(i) {
		struct walt_cpu_cycle *wcc = &per_cpu(walt_cc, i);

		rwlock_init(&wcc->lock);
		wcc->cur_freq_khz = cpufreq_quick_get(i);
		wcc->last_time_ns = 0;
		wcc->cycles = 0;
		wcc->mult_fact = walt_cycle_mult_fact(wcc->cur_freq_khz);
	}

	walt_get_cycle_counts_cb = walt_cpu_cycle_counter;
	use_cycle_counter = true;
	complete(&walt_get_cycle_counts_cb_completion);

	register_trace_android_rvh_cpufreq_transition(walt_cpufreq_transition, NULL);
}

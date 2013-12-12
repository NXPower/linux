/*
 * cpufreq driver for the POWER8 processor
 *
 * (C) Copyright IBM 2013
 *
 * Author: Vaidyanathan Srinivasan <svaidy@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/of_platform.h>

#include <asm/machdep.h>
#include <asm/prom.h>
#include <asm/scom.h>

/* XXX FIXME: Make this per-core */
static DEFINE_MUTEX(freq_switch_mutex);

#define POWERNV_MAX_PSTATES	256

static struct cpufreq_frequency_table powernv_freqs[POWERNV_MAX_PSTATES+1];
static unsigned long powernv_freqs_data[POWERNV_MAX_PSTATES+1];

/*
 * Initialize the freq table based on data obtained
 * from the OCC passed via device-tree
 */

static int init_powernv_pstates(void)
{
	struct device_node *power_mgt;
	struct property *prop;
	int nr_pstates = 0;
	int pstate_min, pstate_max, pstate_nominal;
	u32 *pstate_ids, *pstate_freqs;
	int i;

	power_mgt = of_find_node_by_path("/ibm,opal/power-mgt");
	if (!power_mgt) {
		pr_warn("powernv-cpufreq: \
			DT node /ibm,opal/power-mgt not found\n");
		return -ENODEV;
	}

	prop = of_find_property(power_mgt, "ibm,pstate-min", NULL);
	if (!prop) {
		pr_warn("powernv-cpufreq: \
			DT node /ibm,opal/power-mgt/ibm,pstate-min \
			not found\n");
		return -ENODEV;
	}
	pstate_min = * (u32 *) prop->value;

	prop = of_find_property(power_mgt, "ibm,pstate-max", NULL);
	if (!prop) {
		pr_warn("powernv-cpufreq: \
			DT node /ibm,opal/power-mgt/ibm,pstate-max \
			not found\n");
		return -ENODEV;
	}
	pstate_max = * (u32 *) prop->value;

	prop = of_find_property(power_mgt, "ibm,pstate-nominal", NULL);
	if (!prop) {
		pr_warn("powernv-cpufreq: \
			DT node /ibm,opal/power-mgt/ibm,pstate-nominal \
			not found\n");
		return -ENODEV;
	}
	pstate_nominal = * (u32 *) prop->value;
	printk(KERN_INFO "cpufreq pstate min %d nominal %d max %d\n", pstate_min, pstate_nominal,
						pstate_max);

	prop = of_find_property(power_mgt, "ibm,pstate-ids", NULL);
	if (!prop) {
		pr_warn("powernv-cpufreq: \
			DT node /ibm,opal/power-mgt/ibm,pstate-ids \
			not found\n");
		return -ENODEV;
	}
	pstate_ids = (u32 *) prop->value;

	prop = of_find_property(power_mgt, "ibm,pstate-frequencies-mhz", NULL);
	if (!prop) {
		/* Older firmware */
		prop = of_find_property(power_mgt, "ibm,pstate-freqencies-mhz", NULL);
		if (!prop) {
			pr_warn("powernv-cpufreq: \
			DT node /ibm,opal/power-mgt/ibm,pstate-frequencies-mhz \
						not found\n");
			return -ENODEV;
		}
	}
	pstate_freqs = (u32 *) prop->value;

	nr_pstates = prop->length/sizeof(u32);

	WARN_ON(!nr_pstates);

	pr_debug("NR PStates %d\n", nr_pstates);
	for (i = 0; i < nr_pstates; i++) {
		pr_debug("PState id %d freq %d MHz\n", pstate_ids[i], pstate_freqs[i]);
		powernv_freqs[i].index = i;
		powernv_freqs_data[i] = pstate_ids[i];
		powernv_freqs[i].frequency = pstate_freqs[i] * 1000; /* kHz */
	}

	/* End entry */
	powernv_freqs[i].index = i;
	powernv_freqs[i].frequency = CPUFREQ_TABLE_END;
	powernv_freqs_data[i] = 0;

	return 0;
}

static struct freq_attr* powernv_cpu_freq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

/* Helper routines */

/* Access method to these SPR is special */

static inline unsigned long get_pmspr(unsigned long sprn)
{
	/* TBD: Fix the access protocol */
	switch(sprn) {
		case SPRN_PMCR:
			return mfspr(SPRN_PMCR);

		case SPRN_PMICR:
			return mfspr(SPRN_PMICR);

		case SPRN_PMSR:
			return mfspr(SPRN_PMSR);
	}
	BUG();
}

static inline void set_pmspr(unsigned long sprn, unsigned long val)
{
	/* TBD: Fix the access protocol */
	switch(sprn) {
		case SPRN_PMCR:
			mtspr(SPRN_PMCR, val);
			return;

		case SPRN_PMICR:
			mtspr(SPRN_PMICR, val);
			return;

		case SPRN_PMSR:
			mtspr(SPRN_PMSR, val);
			return;
	}
	BUG();
}

static void set_pstate(void *pstate)
{
	unsigned long val;
	unsigned long pstate_ul = *(unsigned long *) pstate;
	val = get_pmspr(SPRN_PMCR);
	val = val & 0x0000ffffffffffffULL;
	/* Set both local and global state */
	val = val | (pstate_ul << 56) | (pstate_ul << 48);
	pr_debug("Setting cpu %d pmcr to %lX\n", smp_processor_id(), val);
	set_pmspr(SPRN_PMCR, val);
}

int powernv_set_freq(cpumask_var_t cpus, unsigned int new_index)
{
	unsigned long val = powernv_freqs_data[new_index];

	/*
	 * Use smp_call_function to send IPI and execute the
	 * mtspr on target cpu. We could do that without IPI
	 * if current CPU is within policy->cpus (core)
	 */

	val = val & 0xFF;
	smp_call_function_any(cpus, set_pstate, &val, 1);
	return 0;
}

static int powernv_cpufreq_cpu_init(struct cpufreq_policy *policy)
{
	int i;

#ifdef CONFIG_SMP
	cpumask_copy(policy->cpus, topology_thread_cpumask(policy->cpu));
#endif
	policy->cpuinfo.transition_latency = 25000;

	/* if DEBUG is enabled set_pmode() measures the latency
	 * of a transition */

	/* Print frequency table */
	for (i=0; powernv_freqs[i].frequency!=CPUFREQ_TABLE_END; i++)
		pr_debug("%d: %d\n", i, powernv_freqs[i].frequency);

	policy->cur = powernv_freqs[0].frequency;
	cpufreq_frequency_table_get_attr(powernv_freqs, policy->cpu);
	return cpufreq_frequency_table_cpuinfo(policy, powernv_freqs);
}

static int powernv_cpufreq_cpu_exit(struct cpufreq_policy *policy)
{
	cpufreq_frequency_table_put_attr(policy->cpu);
	return 0;
}

static int powernv_cpufreq_verify(struct cpufreq_policy *policy)
{
	return cpufreq_frequency_table_verify(policy, powernv_freqs);
}

static int powernv_cpufreq_target(struct cpufreq_policy *policy,
			      unsigned int target_freq,
			      unsigned int relation)
{
	int rc;
	struct cpufreq_freqs freqs;
	unsigned int new_index;

	cpufreq_frequency_table_target(policy,
				       powernv_freqs,
				       target_freq,
				       relation,
				       &new_index);

	freqs.old = policy->cur;
	freqs.new = powernv_freqs[new_index].frequency;
	freqs.cpu = policy->cpu;

	mutex_lock(&freq_switch_mutex);
	cpufreq_notify_transition(policy, &freqs, CPUFREQ_PRECHANGE);

	pr_debug("setting frequency for cpu %d to %d kHz (data: 0x%lx)",
		 policy->cpu,
		 powernv_freqs[new_index].frequency,
		 powernv_freqs_data[new_index]);

	rc = powernv_set_freq(policy->cpus, new_index);

	cpufreq_notify_transition(policy, &freqs, CPUFREQ_POSTCHANGE);
	mutex_unlock(&freq_switch_mutex);

	return rc;
}

static struct cpufreq_driver powernv_cpufreq_driver = {
	.verify		= powernv_cpufreq_verify,
	.target		= powernv_cpufreq_target,
	.init		= powernv_cpufreq_cpu_init,
	.exit		= powernv_cpufreq_cpu_exit,
	.name		= "powernv-cpufreq",
	.flags		= CPUFREQ_CONST_LOOPS,
	.attr 		= powernv_cpu_freq_attr,
};

static int __init powernv_cpufreq_init(void)
{
	int rc = 0;

	/* Detect pstates from device tree and init */

	rc = init_powernv_pstates();

	if (rc) {
		printk(KERN_INFO "powernv-cpufreq disabled\n");
		return rc;
	}

	rc = cpufreq_register_driver(&powernv_cpufreq_driver);
	return rc;
}

static void __exit powernv_cpufreq_exit(void)
{
	cpufreq_unregister_driver(&powernv_cpufreq_driver);
}

module_init(powernv_cpufreq_init);
module_exit(powernv_cpufreq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vaidyanathan Srinivasan <svaidy@linux.vnet.ibm.com>");



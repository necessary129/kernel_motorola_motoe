/*
 * arch/arm/kernel/clarity_hotplug.c
 *
 * smart automatically hotplug/unplug multiple cpu cores
 * based on cpu freq and loads with suspend state
 *
 * based on the msm_mpdecision code by
 * Copyright (c) 2012-2013, Dennis Rassmann <showp1984@gmail.com>
 *
 * based on the autosmp code by
 * Copyright (C) 2013-2014, Rauf Gungor, http://github.com/mrg666
 *
 * Copyright (C) 2016-2017, Ryan Andri <ryanandri@linuxmail.org>
 *                                     https://github.com/ryan-andri
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. For more details, see the GNU
 * General Public License included with the Linux kernel or available
 * at www.gnu.org/licenses
 */

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#include <linux/mutex.h>
#include <linux/tick.h>
#include <linux/kernel_stat.h>
#include <linux/powersuspend.h>

#define CLARITY_TAG "Clarity_Hotplug: "
#define CLARITY_STARTDELAY 10000

static struct delayed_work clarity_work;
static struct workqueue_struct *clarity_workq;

static DEFINE_MUTEX(clarity_hp_mutex);

static bool suspended = false;

struct clarity_cpu_data {
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_wall;
};
static DEFINE_PER_CPU(struct clarity_cpu_data, clarity_data);

static bool clarity_ready = false;

static struct clarity_param_struct {
	int enabled;
	unsigned int delay;
	unsigned int max_cpus;
	unsigned int min_cpus;
	unsigned int cpufreq_up;
	unsigned int cpuload_up;
	unsigned int cpufreq_down;
	unsigned int cpuload_down;
	int io_is_busy;
	struct mutex clarity_hp_mutexed;
} clarity_param = {
	.enabled = 0,
	.delay = 100,
	.max_cpus = 2,
	.min_cpus = 1,
	.cpufreq_up = 98,
	.cpufreq_down = 70,
	.cpuload_up = 80,
	.cpuload_down = 50,
	.io_is_busy = 0,
};

static inline cputime64_t get_cpu_idle_time_jiffy(unsigned int cpu,
						  cputime64_t *wall)
{
	u64 idle_time;
	u64 cur_wall_time;
	u64 busy_time;

	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());

	busy_time  = kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];

	idle_time = cur_wall_time - busy_time;
	if (wall)
		*wall = jiffies_to_usecs(cur_wall_time);

	return jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu,
					    cputime64_t *wall)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, wall);

	if (idle_time == -1ULL)
		idle_time = get_cpu_idle_time_jiffy(cpu, wall);
	else if (!clarity_param.io_is_busy)
		idle_time += get_cpu_iowait_time_us(cpu, wall);

	return idle_time;
}

static int get_cpu_loads(unsigned int cpu)
{
	struct clarity_cpu_data *data = &per_cpu(clarity_data, cpu);
	struct cpufreq_policy policy;
	u64 cur_wall_time, cur_idle_time;
	unsigned int idle_time, wall_time;
	unsigned int load = 0, max_load = 0;

	cpufreq_get_policy(&policy, cpu);

	cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time);

	wall_time = (unsigned int) (cur_wall_time - data->prev_cpu_wall);
	data->prev_cpu_wall = cur_wall_time;

	idle_time = (unsigned int) (cur_idle_time - data->prev_cpu_idle);
	data->prev_cpu_idle = cur_idle_time;

	if (unlikely(!wall_time || wall_time < idle_time))
		return 0;

	load = 100 * (wall_time - idle_time) / wall_time;

	max_load = (load * policy.cur) / policy.max;

	return max_load;
}

static void __ref clarity_work_fn(struct work_struct *work)
{
	struct cpufreq_policy policy;
	unsigned int cpu = 0, slow_cpu = 0;
	unsigned int rate, cpu0_rate, slow_rate, fast_rate;
	unsigned int up_rate, down_rate, max_freq = 0;
	unsigned int fast_load = 0, slow_load = 0;
	int nr_cpu_online;

	if (suspended)
		return;

	/* get policy at cpu 0 */
	cpufreq_get_policy(&policy, 0);

	max_freq = policy.max;
	slow_rate = max_freq;

	up_rate = (clarity_param.cpufreq_up * max_freq) / 100;
	down_rate = (clarity_param.cpufreq_down * max_freq) / 100;

	/* get cpu loads from cpu 0 */
	fast_load = get_cpu_loads(0);

	/* find current max and min cpu freq to estimate load */
	get_online_cpus();
	nr_cpu_online = num_online_cpus();
	cpu0_rate = policy.cur;
	fast_rate = cpu0_rate;
	for_each_online_cpu(cpu) {
		if (cpu) {
			struct cpufreq_policy pcpu;

			cpufreq_get_policy(&pcpu, cpu);
			rate = pcpu.cur;
			if (rate <= slow_rate) {
				slow_cpu = cpu;
				slow_rate = rate;
			} else if (rate > fast_rate) {
				fast_rate = rate;
			}
			slow_load = get_cpu_loads(cpu);
		}
	}
	put_online_cpus();
	if (cpu0_rate < slow_rate)
		slow_rate = cpu0_rate;

	/* hotplug one core if all online cores are over up_rate limit */
	if ((slow_rate > up_rate) &&
		(fast_load > clarity_param.cpuload_up)) {
		if (nr_cpu_online < clarity_param.max_cpus) {
			cpu = cpumask_next_zero(0, cpu_online_mask);
			cpu_up(cpu);
		}
	/* unplug slowest core if all online cores are under down_rate limit */
	} else if (slow_cpu && (fast_rate < down_rate) &&
			(slow_load < clarity_param.cpuload_down)) {
		if (nr_cpu_online > clarity_param.min_cpus) {
 			cpu_down(slow_cpu);
		}
	} /* else do nothing */

	queue_delayed_work(clarity_workq, &clarity_work,
				msecs_to_jiffies(clarity_param.delay));
}

static void clarity_power_suspend(struct power_suspend *h)
{
	unsigned int cpu;

	mutex_lock(&clarity_param.clarity_hp_mutexed);

	suspended = true;

	/* flush workqueue on suspend */
	flush_workqueue(clarity_workq);

	/* suspend main work thread */
	cancel_delayed_work_sync(&clarity_work);

	/* unplug online cpu cores */
	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
		cpu_down(cpu);
	}

	mutex_unlock(&clarity_param.clarity_hp_mutexed);

	pr_info(CLARITY_TAG"suspended with %d core online\n", num_online_cpus());
}

static void __ref clarity_power_resume(struct power_suspend *h)
{
	unsigned int cpu;

	mutex_lock(&clarity_param.clarity_hp_mutexed);

	suspended = false;

	/* hotplug offline cpu cores */
	for_each_present_cpu(cpu) {
		if (num_online_cpus() >= clarity_param.max_cpus)
			break;
		if (!cpu_online(cpu))
			cpu_up(cpu);
	}

	/* resume main work thread in 3 seconds */
	queue_delayed_work(clarity_workq, &clarity_work, msecs_to_jiffies(3000));

	mutex_unlock(&clarity_param.clarity_hp_mutexed);

	pr_info(CLARITY_TAG"resumed with %d core online\n", num_online_cpus());
}

static struct power_suspend clarity_power_suspend_handler = {
	.suspend = clarity_power_suspend,
	.resume = clarity_power_resume,
};

static int clarity_start(void)
{
	unsigned int cpu;
	int ret = 0;

	/* bail out if already enabled before */
	if (clarity_ready) {
		pr_info(CLARITY_TAG"Already enabled!\n");
		return ret;
	}

	mutex_lock(&clarity_hp_mutex);

	clarity_ready = true;

	clarity_workq = alloc_workqueue("clarity_hp", WQ_HIGHPRI | WQ_FREEZABLE, 0);
	if (!clarity_workq) {
		pr_err("%s: Failed to allocate clarity hotplug workqueue\n", CLARITY_TAG);
		ret = -ENOMEM;
		goto error;
	}

	/* record previous cpu idle */
	get_online_cpus();
	for_each_online_cpu(cpu) {
		struct clarity_cpu_data *data;

		data = &per_cpu(clarity_data, cpu);
		data->prev_cpu_idle = get_cpu_idle_time(cpu, &data->prev_cpu_wall);
	}
	put_online_cpus();

	INIT_DELAYED_WORK(&clarity_work, clarity_work_fn);
	register_power_suspend(&clarity_power_suspend_handler);

	mutex_init(&clarity_param.clarity_hp_mutexed);

	mutex_unlock(&clarity_hp_mutex);

	queue_delayed_work(clarity_workq, &clarity_work,
				msecs_to_jiffies(CLARITY_STARTDELAY));

	pr_info(CLARITY_TAG"enabled\n");

	return ret;
error:
	mutex_unlock(&clarity_hp_mutex);
	clarity_param.enabled = 0;
	clarity_ready = false;
	return ret;
}

static int __ref clarity_stop(void)
{
	unsigned int cpu;

	/* bail out if already disabled before */
	if (!clarity_ready) {
		pr_info(CLARITY_TAG"Already disabled!\n");
		return 0;
	}

	mutex_lock(&clarity_hp_mutex);

	clarity_ready = false;

	cancel_delayed_work_sync(&clarity_work);
	flush_workqueue(clarity_workq);

	unregister_power_suspend(&clarity_power_suspend_handler);

	mutex_destroy(&clarity_param.clarity_hp_mutexed);

	destroy_workqueue(clarity_workq);

	mutex_unlock(&clarity_hp_mutex);

	for_each_present_cpu(cpu) {
		if (num_online_cpus() >= clarity_param.max_cpus)
			break;
		if (!cpu_online(cpu))
			cpu_up(cpu);
	}

	pr_info(CLARITY_TAG"disabled\n");

	return 0;
}

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", clarity_param.object);			\
}
show_one(enabled, enabled);
show_one(delay, delay);
show_one(min_cpus, min_cpus);
show_one(max_cpus, max_cpus);
show_one(cpufreq_up, cpufreq_up);
show_one(cpufreq_down, cpufreq_down);
show_one(cpuload_up, cpuload_up);
show_one(cpuload_down, cpuload_down);
show_one(io_is_busy, io_is_busy);

static ssize_t store_enabled(struct kobject *a, struct attribute *b,
			const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	clarity_param.enabled = input;

	if (clarity_param.enabled)
		clarity_start();
	else
		clarity_stop();

	return count;
}

static ssize_t store_delay(struct kobject *a, struct attribute *b,
			const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	clarity_param.delay = input;

	return count;
}

static ssize_t store_min_cpus(struct kobject *a, struct attribute *b,
			const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	clarity_param.min_cpus = input;

	return count;
}

static ssize_t store_max_cpus(struct kobject *a, struct attribute *b,
			const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	clarity_param.max_cpus = input;

	return count;
}

static ssize_t store_cpufreq_up(struct kobject *a, struct attribute *b,
			const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	clarity_param.cpufreq_up = input;

	return count;
}

static ssize_t store_cpufreq_down(struct kobject *a, struct attribute *b,
			const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	clarity_param.cpufreq_down = input;

	return count;
}

static ssize_t store_cpuload_up(struct kobject *a, struct attribute *b,
			const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	clarity_param.cpuload_up = input;

	return count;
}

static ssize_t store_cpuload_down(struct kobject *a, struct attribute *b,
			const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	clarity_param.cpuload_down = input;

	return count;
}

static ssize_t store_io_is_busy(struct kobject *a, struct attribute *b,
			const char *buf, size_t count)
{
	unsigned int cpu, input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	clarity_param.io_is_busy = input;

	/* record previous cpu idle */
	get_online_cpus();
	for_each_online_cpu(cpu) {
		struct clarity_cpu_data *data;

		data = &per_cpu(clarity_data, cpu);
		data->prev_cpu_idle = get_cpu_idle_time(cpu, &data->prev_cpu_wall);
	}
	put_online_cpus();

	return count;
}

define_one_global_rw(enabled);
define_one_global_rw(delay);
define_one_global_rw(min_cpus);
define_one_global_rw(max_cpus);
define_one_global_rw(cpufreq_up);
define_one_global_rw(cpufreq_down);
define_one_global_rw(cpuload_up);
define_one_global_rw(cpuload_down);
define_one_global_rw(io_is_busy);

static struct attribute *clarity_attributes[] = {
	&enabled.attr,
	&delay.attr,
	&min_cpus.attr,
	&max_cpus.attr,
	&cpufreq_up.attr,
	&cpufreq_down.attr,
	&cpuload_up.attr,
	&cpuload_down.attr,
	&io_is_busy.attr,
	NULL
};

static struct attribute_group clarity_attr_group = {
	.attrs = clarity_attributes,
};

struct kobject *clarity_kobject;

static int __init clarity_init(void)
{
	int rc;

	clarity_kobject = kobject_create_and_add("clarity_hotplug", kernel_kobj);
	if (clarity_kobject) {
		rc = sysfs_create_group(clarity_kobject, &clarity_attr_group);
		if (rc)
			pr_warn(CLARITY_TAG"ERROR, create sysfs group");
	} else
		pr_warn(CLARITY_TAG"ERROR, create sysfs kobj");

	if (clarity_param.enabled)
		clarity_start();

	pr_info(CLARITY_TAG"initialized\n");

	return 0;
}

MODULE_AUTHOR("Dennis Rassmann <showp1984@gmail.com>, "
	      "Rauf Gungor <http://github.com/mrg666>,"
	      "Ryan Andri <ryanandri@linuxmail.org>");
MODULE_DESCRIPTION("hotplug/unplug cpu cores based on cpu freq and loads");
MODULE_LICENSE("GPLv2");

late_initcall(clarity_init);

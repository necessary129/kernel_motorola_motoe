/*
 * Copyright (C) 2017, Sultanxda <sultanxda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "iosched-swch: " fmt

#include <linux/blkdev.h>
#include <linux/blk_types.h>
#include <linux/elevator.h>
#include <linux/powersuspend.h>

#define NOOP_IOSCHED "noop"
#define RESTORE_DELAY_MS (5000)
#define SUSPEND_DELAY_MS (5000)

struct req_queue_data {
	struct list_head list;
	struct request_queue *queue;
	char prev_e[ELV_NAME_MAX];
	bool using_noop;
};

static bool resumed = false;

static struct delayed_work restore_prev, suspend_work;
static struct workqueue_struct *is_wq;
static DEFINE_SPINLOCK(init_lock);
static struct req_queue_data req_queues = {
	.list = LIST_HEAD_INIT(req_queues.list),
};

static void change_elevator(struct req_queue_data *r, bool use_noop)
{
	struct request_queue *q = r->queue;

	if (r->using_noop == use_noop)
		return;

	r->using_noop = use_noop;

	if (use_noop) {
		strcpy(r->prev_e, q->elevator->type->elevator_name);
		elevator_change(q, NOOP_IOSCHED);
	} else {
		elevator_change(q, r->prev_e);
	}
}

static void change_all_elevators(struct list_head *head, bool use_noop)
{
	struct req_queue_data *r;

	list_for_each_entry(r, head, list)
		change_elevator(r, use_noop);
}

static void restore_prev_fn(struct work_struct *work)
{
	change_all_elevators(&req_queues.list, false);
}

static void suspend_work_fn(struct work_struct *work)
{
	/*
	 * Switch to noop when the screen turns off. Purposely block
	 * the fb notifier chain call in case weird things can happen
	 * when switching elevators while the screen is off.
	 */
	change_all_elevators(&req_queues.list, true);
}

static void is_power_suspend(struct power_suspend *h)
{
	if (resumed) {
		cancel_delayed_work_sync(&restore_prev);
	}

	resumed = false;

	queue_delayed_work(is_wq, &suspend_work,
			msecs_to_jiffies(SUSPEND_DELAY_MS));
}

static void is_power_resume(struct power_suspend *h)
{
	if (!resumed) {
		resumed = true;

		cancel_delayed_work_sync(&suspend_work);

		/*
		 * Switch back from noop to the original iosched after a delay
		 * when the screen is turned on.
		 */
		queue_delayed_work(is_wq, &restore_prev,
				msecs_to_jiffies(RESTORE_DELAY_MS));
	}
}

static struct power_suspend is_power_suspend_handler = {
	.suspend = is_power_suspend,
	.resume = is_power_resume,
};

int init_iosched_switcher(struct request_queue *q)
{
	struct req_queue_data *r;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;

	r->queue = q;

	spin_lock(&init_lock);
	list_add(&r->list, &req_queues.list);
	spin_unlock(&init_lock);

	return 0;
}

static int iosched_switcher_core_init(void)
{
	is_wq = alloc_workqueue("io_switcher", WQ_HIGHPRI, 0);
	if (!is_wq) {
		pr_info("io_switcher: Failed to allocate workqueue\n");
		return -ENOMEM;
	}
	INIT_DELAYED_WORK(&restore_prev, restore_prev_fn);
	INIT_DELAYED_WORK(&suspend_work, suspend_work_fn);
	register_power_suspend(&is_power_suspend_handler);

	return 0;
}
late_initcall(iosched_switcher_core_init);

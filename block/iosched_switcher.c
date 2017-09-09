/*
 * I/O Scheduler Swicher.
 *
 * Switches the I/O scheduler for a single block
 * device to Fifo/Noop when the screen turns off,
 * and back to its original I/O scheduler after
 * a delay when the screen is turned back on.
 *
 * Copyright (C) 2017, Sultanxda <sultanxda@gmail.com>
 *		       Authored by Sultanxda @xda-developers.com
 *
 * Copyright (C) 2017, Ryan Andri <github.com/ryan-andri>
 *		       maintained on linux 3.4.y
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

#include <linux/blkdev.h>
#include <linux/blk_types.h>
#include <linux/elevator.h>
#include <linux/powersuspend.h>

#ifdef CONFIG_IOSCHED_FIFO
#define IOSCHED "fifo"
#else
#define IOSCHED "noop"
#endif
#define DELAY_MS (10000)

struct req_queue_data {
	struct list_head list;
	struct request_queue *queue;
	char prev_e[ELV_NAME_MAX];
	bool is_state;
};

/* flags for state suspend/resume */
static bool resumed = false;
static bool suspended = false;

static DEFINE_SPINLOCK(init_lock);

static struct delayed_work resume_work;
static struct delayed_work suspend_work;
static struct workqueue_struct *is_wq;
static struct req_queue_data req_queues = {
	.list = LIST_HEAD_INIT(req_queues.list),
};

static void change_elevator(struct req_queue_data *r, bool is_suspend)
{
	struct request_queue *q = r->queue;

	if (r->is_state == is_suspend)
		return;

	r->is_state = is_suspend;

	if (is_suspend) {
		/* copy previuos iosched */
		strcpy(r->prev_e, q->elevator->type->elevator_name);
		/* skip if equal with previous iosched */
		if (strcmp(r->prev_e, IOSCHED) != 0) {
			elevator_change(q, IOSCHED);
		}
	} else {
		/* skip if NULL and not equal with fifo/noop iosched */
		if ((r->prev_e != NULL) &&
			(strcmp(q->elevator->type->elevator_name, IOSCHED) == 0)) {
			elevator_change(q, r->prev_e);
		}
	}
}

static void resume_work_fn(struct work_struct *work)
{
	struct req_queue_data *r;
	struct list_head *head = &req_queues.list;

	/*
	 * Switch to original iosched when the screen turns on. Purposely block
	 * the powersuspend notifier chain call in case weird things can happen
	 * when switching elevators while the screen is on.
	 */
	list_for_each_entry(r, head, list) {
		change_elevator(r, false);
	}
}

static void suspend_work_fn(struct work_struct *work)
{
	struct req_queue_data *r;
	struct list_head *head = &req_queues.list;

	/*
	 * Switch to fifo/noop when the screen turns off. Purposely block
	 * the powersuspend notifier chain call in case weird things can happen
	 * when switching elevators while the screen is off.
	 */
	list_for_each_entry(r, head, list) {
		change_elevator(r, true);
	}
}

static void is_power_suspend(struct power_suspend *h)
{
	if (resumed) {
		cancel_delayed_work_sync(&resume_work);
	}

	resumed = false;

	/*
	 * change from the original to fifo/noop iosched after a delay
	 * when the screen is turned off.
	 */
	queue_delayed_work(is_wq, &suspend_work,
			msecs_to_jiffies(DELAY_MS));

	suspended = true;
}

static void is_power_resume(struct power_suspend *h)
{
	if (suspended) {
		cancel_delayed_work_sync(&suspend_work);
	}

	suspended = false;

	/*
	 * Switch back from fifo/noop to the original iosched after a delay
	 * when the screen is turned on.
	 */
	queue_delayed_work(is_wq, &resume_work,
			msecs_to_jiffies(DELAY_MS));

	resumed = true;
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

static int __init iosched_switcher_core_init(void)
{
	is_wq = alloc_workqueue("io_switcher", WQ_HIGHPRI, 0);
	if (!is_wq) {
		pr_info("%s: Failed to allocate workqueue\n", __func__);
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&resume_work, resume_work_fn);
	INIT_DELAYED_WORK(&suspend_work, suspend_work_fn);
	register_power_suspend(&is_power_suspend_handler);

	return 0;
}
late_initcall(iosched_switcher_core_init);

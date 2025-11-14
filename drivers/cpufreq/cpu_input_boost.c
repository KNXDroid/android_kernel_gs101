// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 * Copyright (C) 2025-2025 danya2271 <danya2271@yandex.ru>
 */

#define pr_fmt(fmt) "cpu_input_boost: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#define INPUT_BOOST_DURATION_MS 150
#define INPUT_BOOST_COOLDOWN_MS 850

static unsigned int input_boost_freq __read_mostly =
CONFIG_INPUT_BOOST_FREQ;
static unsigned int max_boost_freq __read_mostly =
CONFIG_MAX_BOOST_FREQ;
static unsigned short wake_boost_duration __read_mostly =
CONFIG_WAKE_BOOST_DURATION_MS;

module_param(input_boost_freq, uint, 0644);
module_param(max_boost_freq, uint, 0644);
module_param(wake_boost_duration, short, 0644);

enum {
	SCREEN_OFF,
	INPUT_BOOST,
	MAX_BOOST
};

#define MAX_CPU_POLICIES 8

struct boost_drv {
	struct delayed_work input_unboost;
	struct delayed_work max_unboost;
	struct notifier_block fb_notif;
	struct freq_qos_request qos_reqs[MAX_CPU_POLICIES];
	int num_policies;
	atomic_long_t max_boost_expires;
	unsigned long state;
	struct delayed_work init_work;

	unsigned long last_input_boost_jiffies;
};

/* Forward declarations */
static void update_cpu_boost_qos(struct boost_drv *b);
static void input_unboost_worker(struct work_struct *work);
static void max_unboost_worker(struct work_struct *work);
static void cpu_input_boost_init_work(struct work_struct *work);

static struct boost_drv boost_drv_g __read_mostly = {
	.input_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.input_unboost,
												input_unboost_worker, 0),
												.max_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.max_unboost,
																						  max_unboost_worker, 0),
																						  .init_work = __DELAYED_WORK_INITIALIZER(boost_drv_g.init_work,
																																  cpu_input_boost_init_work, 0),
};

static void update_cpu_boost_qos(struct boost_drv *b)
{
	s32 target_freq = FREQ_QOS_MIN_DEFAULT_VALUE;
	int i;

	if (test_bit(SCREEN_OFF, &b->state)) {
		goto update;
	}

	if (test_bit(MAX_BOOST, &b->state)) {
		target_freq = max_boost_freq;
	} else if (test_bit(INPUT_BOOST, &b->state)) {
		target_freq = input_boost_freq;
	}

	update:
	//pr_info("Updating Freq QoS for %d policies to: %d KHz\n", b->num_policies, target_freq);
	for (i = 0; i < b->num_policies; i++) {
		freq_qos_update_request(&b->qos_reqs[i], target_freq);
	}
}

static void __cpu_input_boost_kick(struct boost_drv *b)
{
	/*
	 * Cooldown / Rate-limiting logic.
	 * Check if the cooldown period has passed since the last boost.
	 * The time_before() macro correctly handles jiffies wraparound.
	 */
	if (time_before(jiffies, b->last_input_boost_jiffies +
		msecs_to_jiffies(INPUT_BOOST_COOLDOWN_MS))) {
		return;
		}

		if (test_bit(SCREEN_OFF, &b->state))
			return;

	b->last_input_boost_jiffies = jiffies;

	set_bit(INPUT_BOOST, &b->state);
	update_cpu_boost_qos(b);

	mod_delayed_work(system_unbound_wq, &b->input_unboost,
					 msecs_to_jiffies(INPUT_BOOST_DURATION_MS));
}

void cpu_input_boost_kick(void)
{
	__cpu_input_boost_kick(&boost_drv_g);
}

static void __cpu_input_boost_kick_max(struct boost_drv *b, unsigned int ms)
{
	unsigned long j = msecs_to_jiffies(ms);
	unsigned long curr_expires, new_expires;

	if (test_bit(SCREEN_OFF, &b->state)) return;
	do {
		curr_expires = atomic_long_read(&b->max_boost_expires);
		new_expires = jiffies + j;
		if (time_after(curr_expires, new_expires)) return;
	} while (atomic_long_cmpxchg(&b->max_boost_expires, curr_expires, new_expires) != curr_expires);

		set_bit(MAX_BOOST, &b->state);
		update_cpu_boost_qos(b);
		mod_delayed_work(system_unbound_wq, &b->max_unboost, j);
}

void cpu_input_boost_kick_max(unsigned int ms) { __cpu_input_boost_kick_max(&boost_drv_g, ms); }

static void input_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work), typeof(*b), input_unboost);
	clear_bit(INPUT_BOOST, &b->state);
	update_cpu_boost_qos(b);
}

static void max_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work), typeof(*b), max_unboost);
	clear_bit(MAX_BOOST, &b->state);
	update_cpu_boost_qos(b);
}

static int fb_notifier_cb(struct notifier_block *nb, unsigned long action, void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), fb_notif);
	struct fb_event *evdata = data;
	int *blank = evdata->data;

	if (action != FB_EVENT_BLANK) return NOTIFY_OK;
	if (*blank == FB_BLANK_UNBLANK) {
		clear_bit(SCREEN_OFF, &b->state);
		__cpu_input_boost_kick_max(b, wake_boost_duration);
	} else {
		set_bit(SCREEN_OFF, &b->state);
		cancel_delayed_work(&b->input_unboost);
		cancel_delayed_work(&b->max_unboost);
		clear_bit(INPUT_BOOST, &b->state);
		clear_bit(MAX_BOOST, &b->state);
		update_cpu_boost_qos(b);
	}
	return NOTIFY_OK;
}

static void cpu_input_boost_input_event(struct input_handle *handle, unsigned int type, unsigned int code, int value)
{
	__cpu_input_boost_kick(handle->handler->private);
}

static int cpu_input_boost_input_connect(struct input_handler *handler, struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;
	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle) return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpu_input_boost_handle";
	ret = input_register_handle(handle);
	if (ret) goto free_handle;
	ret = input_open_device(handle);
	if (ret) goto unregister_handle;
	return 0;
	unregister_handle:
	input_unregister_handle(handle);
	free_handle:
	kfree(handle);
	return ret;
}

static void cpu_input_boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpu_input_boost_ids[] = {
	{ .flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] = BIT_MASK(ABS_MT_POSITION_X) | BIT_MASK(ABS_MT_POSITION_Y) } },
		{ .flags = INPUT_DEVICE_ID_MATCH_KEYBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
			.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
			.absbit = { [BIT_WORD(ABS_X)] = BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) } },
			{ .flags = INPUT_DEVICE_ID_MATCH_EVBIT, .evbit = { BIT_MASK(EV_KEY) } },
			{ }
};

static struct input_handler cpu_input_boost_input_handler = {
	.event = cpu_input_boost_input_event, .connect = cpu_input_boost_input_connect,
	.disconnect = cpu_input_boost_input_disconnect, .name = "cpu_input_boost_handler",
	.id_table = cpu_input_boost_ids,
};

static void cpu_input_boost_init_work(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work), struct boost_drv, init_work);
	struct cpufreq_policy *policy;
	int ret, cpu = -1;

	b->num_policies = 0;

	while ((cpu = cpumask_next(cpu, cpu_possible_mask)) < nr_cpu_ids) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_info("cpufreq policies not ready, retrying...\n");
			schedule_delayed_work(&b->init_work, msecs_to_jiffies(1000));
			return;
		}

		if (policy->cpu == cpu) {
			if (b->num_policies >= MAX_CPU_POLICIES) {
				pr_warn("Found more policies than supported (%d)\n", MAX_CPU_POLICIES);
				cpufreq_cpu_put(policy);
				break;
			}
			ret = freq_qos_add_request(&policy->constraints,
									   &b->qos_reqs[b->num_policies],
							  FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE);
			if (ret < 0) {
				pr_err("Failed to add QoS request for policy %d\n", cpu);
			} else {
				b->num_policies++;
			}
		}
		cpufreq_cpu_put(policy);
	}

	if (b->num_policies == 0) {
		pr_err("Could not find any cpufreq policies. Aborting.\n");
		return;
	}

	cpu_input_boost_input_handler.private = b;
	ret = input_register_handler(&cpu_input_boost_input_handler);
	if (ret) {
		for (cpu = 0; cpu < b->num_policies; cpu++)
			freq_qos_remove_request(&b->qos_reqs[cpu]);
		return;
	}

	b->fb_notif.notifier_call = fb_notifier_cb;
	ret = fb_register_client(&b->fb_notif);
	if (ret) {
		input_unregister_handler(&cpu_input_boost_input_handler);
		for (cpu = 0; cpu < b->num_policies; cpu++)
			freq_qos_remove_request(&b->qos_reqs[cpu]);
		return;
	}
	pr_info("Driver initialization complete for %d policies.\n", b->num_policies);
}

static int __init cpu_input_boost_init(void)
{
	boost_drv_g.last_input_boost_jiffies = jiffies - msecs_to_jiffies(INPUT_BOOST_COOLDOWN_MS);
	schedule_delayed_work(&boost_drv_g.init_work, msecs_to_jiffies(1000));
	return 0;
}
late_initcall(cpu_input_boost_init);

static void __exit cpu_input_boost_exit(void)
{
	struct boost_drv *b = &boost_drv_g;
	int i;

	cancel_delayed_work_sync(&b->init_work);
	fb_unregister_client(&b->fb_notif);
	input_unregister_handler(&cpu_input_boost_input_handler);
	cancel_delayed_work_sync(&b->input_unboost);
	cancel_delayed_work_sync(&b->max_unboost);
	for (i = 0; i < b->num_policies; i++) {
		freq_qos_remove_request(&b->qos_reqs[i]);
	}
}
module_exit(cpu_input_boost_exit);

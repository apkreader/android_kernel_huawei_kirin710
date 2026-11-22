/*
 * secs_power_ctrl.c
 *
 * Hisilicon secs power ctrl driver .
 *
 * Copyright (c) 2001-2021, Huawei Tech. Co., Ltd. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/clk.h>
#include <linux/cdev.h>
#include <linux/cpumask.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hisi/hisi_drmdriver.h>
#include <linux/hisi/secs_power_ctrl.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#define SECS_UNUSED(x)        ((void)x)
#define SECS_POWER_UP    (0xAA55A5A5)
#define SECS_POWER_DOWN  (0x55AA5A5A)
#define SECS_SECCLK_EN   (0x5A5A55AA)
#define SECS_SECCLK_DIS  (0xA5A5AA55)
#define SECS_CPU0        (0)
#define SECS_ONLY_CPU0_ONLINE (0xE5A5)
#define SECS_OTHER_CPU_ONLINE (0xA5E5)
#define SECS_SUSPEND_STATUS   (0xA5A5)
#define SECS_TRIGGER_TIMER (jiffies + HZ) /*trigger time 1s*/
/*defined for freq control*/
enum {
    SECS_HIGH_FREQ_INDEX = 0,
    SECS_LOW_TEMP_FREQ_INDEX,
    SECS_LOW_VOLT_FREQ_INDEX,
    SECS_FREQ_MAX,
};

static void hisi_secs_timer_func(struct timer_list *timer);

static DEFINE_MUTEX(secs_timer_lock);
static DEFINE_MUTEX(secs_count_lock);
static DEFINE_TIMER(secs_timer, hisi_secs_timer_func);/*lint -e785 */

static unsigned long g_secs_power_ctrl_count;
static bool secs_fault;
unsigned long g_secs_suspend_status = 0;
static struct regulator_bulk_data regu_burning;
static struct clk *secs_clk = NULL;
static struct device_node *secs_np = NULL;
static struct device *secs_dev =  NULL;

static void hisi_secs_timer_init(void)
{
	int cpu;
	unsigned int flag = SECS_ONLY_CPU0_ONLINE;

	mutex_lock(&secs_timer_lock);
	for_each_online_cpu(cpu) {/*lint -e713 */
		if (!timer_pending(&secs_timer)) {
			if (SECS_CPU0 == cpu) {
				flag = SECS_ONLY_CPU0_ONLINE;
				continue;
			} else {
				secs_timer.expires = SECS_TRIGGER_TIMER;
				add_timer_on(&secs_timer, cpu);
				flag = SECS_OTHER_CPU_ONLINE;
				break;
			}
		}
	}
	if (SECS_ONLY_CPU0_ONLINE == flag) {
		pr_err("only cpu0 online secure clk feature disable\n");
	}
	mutex_unlock(&secs_timer_lock);
	return;
}

static void hisi_secs_timer_func(struct timer_list *timer)
{
	int ret;

	ret = atfd_hisi_service_access_register_smc(ACCESS_REGISTER_FN_MAIN_ID,
                                                (u64)SECS_SECCLK_EN,
                                                (u64)0,
                                                ACCESS_REGISTER_FN_SUB_ID_SECS_POWER_CTRL);
	if (ret) {
		pr_err("failed to enable secs secclk %d\n", ret);
		return;
	} else {
		mod_timer(timer, SECS_TRIGGER_TIMER);
		return;
	}
}

static int hisi_secs_adjust_freq(void)
{
	u32 rates[SECS_FREQ_MAX];
	int i, ret;

	if (!secs_clk)
		return 0;

	ret = of_property_read_u32_array(secs_np, "secs_clk_rate", rates, ARRAY_SIZE(rates));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(rates); i++) {
		ret = clk_set_rate(secs_clk, rates[i]);
		if (ret)
			continue;
		ret = clk_prepare_enable(secs_clk);
		if (!ret)
			return 0;
	}

	return ret;
}

static int secs_command(u32 command)
{
	return atfd_hisi_service_access_register_smc(ACCESS_REGISTER_FN_MAIN_ID,
						       command, 0,
						       ACCESS_REGISTER_FN_SUB_ID_SECS_POWER_CTRL);
}

static int hisi_secs_atfd_power_on(void)
{
	int ret;

	if (g_secs_power_ctrl_count) {
		g_secs_power_ctrl_count++;
		return 0;
	}

	ret = hisi_secs_adjust_freq();
	if (ret)
		return ret;

	ret = secs_command(SECS_POWER_UP);
	if (ret)
		goto disable_clock;

	ret = secs_command(SECS_SECCLK_EN);
	if (ret) {
		if (secs_command(SECS_POWER_DOWN)) {
			secs_fault = true;
			dev_err(secs_dev, "failed to roll back secure processor power\n");
			return -EIO;
		}
		goto disable_clock;
	}

	g_secs_power_ctrl_count = 1;
	hisi_secs_timer_init();
	return 0;

disable_clock:
	clk_disable_unprepare(secs_clk);
	return ret < 0 ? ret : -EIO;
}

static int hisi_secs_atfd_power_down(void)
{
	int ret;

	if (WARN_ON(!g_secs_power_ctrl_count))
		return -EINVAL;
	if (g_secs_power_ctrl_count > 1) {
		g_secs_power_ctrl_count--;
		return 0;
	}

	timer_delete_sync(&secs_timer);
	ret = secs_command(SECS_SECCLK_DIS);
	if (!ret)
		ret = secs_command(SECS_POWER_DOWN);
	if (ret) {
		/* Keep the voltage vote while the secure processor's state is unknown. */
		secs_fault = true;
		dev_err(secs_dev, "secure processor power-down failed: %d\n", ret);
		return ret < 0 ? ret : -EIO;
	}

	g_secs_power_ctrl_count = 0;
	clk_disable_unprepare(secs_clk);
	return 0;
}

int hisi_secs_power_on(void)
{
	int ret = 0;

	mutex_lock(&secs_count_lock);
	if (!secs_dev) {
		ret = -EPROBE_DEFER;
	} else if (secs_fault) {
		ret = -EIO;
	} else if (g_secs_suspend_status == SECS_SUSPEND_STATUS) {
		ret = -EBUSY;
	} else if (regu_burning.consumer) {
		ret = clk_prepare_enable(secs_clk);
		if (!ret) {
			ret = regulator_bulk_enable(1, &regu_burning);
			if (ret)
				clk_disable_unprepare(secs_clk);
		}
	} else if (of_property_present(secs_np, "secs-atfd-power-ctrl")) {
		ret = hisi_secs_atfd_power_on();
	}
	mutex_unlock(&secs_count_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(hisi_secs_power_on);

int hisi_secs_power_down(void)
{
	int ret = 0;

	mutex_lock(&secs_count_lock);
	if (!secs_dev) {
		ret = -ENODEV;
	} else if (secs_fault) {
		ret = -EIO;
	} else if (regu_burning.consumer) {
		ret = regulator_bulk_disable(1, &regu_burning);
		if (!ret)
			clk_disable_unprepare(secs_clk);
	} else if (of_property_present(secs_np, "secs-atfd-power-ctrl")) {
		ret = hisi_secs_atfd_power_down();
	}
	mutex_unlock(&secs_count_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(hisi_secs_power_down);

int hisi_secs_link_consumer(struct device *consumer)
{
	int ret = 0;

	mutex_lock(&secs_count_lock);
	if (!secs_dev)
		ret = -EPROBE_DEFER;
	else if (!device_link_add(consumer, secs_dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		ret = -ENOMEM;
	mutex_unlock(&secs_count_lock);

	return ret;
}

static int hisi_secs_power_ctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct regulator_bulk_data regulator = { .supply = "sec-s-buring" };
	struct clk *clk = NULL;
	int ret;

	if (of_property_present(np, "sec-s-regulator-enable")) {
		ret = devm_regulator_bulk_get(dev, 1, &regulator);
		if (ret)
			return dev_err_probe(dev, ret, "could not get secure regulator\n");
	}

	if (of_property_present(np, "secs-clk-freq-adapt")) {
		clk = devm_clk_get(dev, "clk_secs");
		if (IS_ERR(clk)) {
			ret = PTR_ERR(clk);
			if (ret == -ENOENT)
				ret = -EPROBE_DEFER;
			return dev_err_probe(dev, ret, "could not get secure processor clock\n");
		}
	}

	mutex_lock(&secs_count_lock);
	secs_np = np;
	secs_clk = clk;
	secs_dev = dev;
	regu_burning = regulator;
	mutex_unlock(&secs_count_lock);

	return 0;
}

static void hisi_secs_power_ctrl_remove(struct platform_device *pdev)
{
	mutex_lock(&secs_count_lock);
	timer_delete_sync(&secs_timer);
	secs_dev = NULL;
	mutex_unlock(&secs_count_lock);
}

/*lint -e785 -esym(785,*)*/
static const struct of_device_id secs_ctrl_of_match[] = {
	{.compatible = "hisilicon,secs_power_ctrl"},
	{},
};

static int secs_power_ctrl_suspend(struct device *dev)
{
	pr_info("%s: suspend +\n", __func__);
	mutex_lock(&secs_count_lock);
	g_secs_suspend_status = SECS_SUSPEND_STATUS;
	mutex_unlock(&secs_count_lock);
	pr_info("%s: secs_power_ctrl_count=%lx\n", __func__, g_secs_power_ctrl_count);
	pr_info("%s: suspend -\n", __func__);
	return 0;
}

static int secs_power_ctrl_resume(struct device *dev)
{
	pr_info("%s: resume +\n", __func__);
	mutex_lock(&secs_count_lock);
	g_secs_suspend_status = 0;
	mutex_unlock(&secs_count_lock);
	pr_info("%s: secs_power_ctrl_count=%lx\n", __func__, g_secs_power_ctrl_count);
	pr_info("%s: resume -\n", __func__);
	return 0;
}
static SIMPLE_DEV_PM_OPS(secs_power_ctrl_pm_ops, secs_power_ctrl_suspend, secs_power_ctrl_resume);


static struct platform_driver hisi_secs_ctrl_driver = {
	.driver = {
		.owner = THIS_MODULE, /*lint !e64*/
		.name = "hisi-secs-power-ctrl",
		.of_match_table = of_match_ptr(secs_ctrl_of_match),
		.pm     = &secs_power_ctrl_pm_ops,
	},
	.probe = hisi_secs_power_ctrl_probe,
	.remove = hisi_secs_power_ctrl_remove,
};
/*lint -e785 +esym(785,*)*/

static int __init hisi_secs_power_ctrl_init(void)
{
	int ret = 0;

	pr_info("hisi secs power ctrl init\n");
	ret = platform_driver_register(&hisi_secs_ctrl_driver);/*lint !e64*/
	if (ret)
		pr_err("register secs power ctrl driver fail\n");

	return ret;
}
fs_initcall(hisi_secs_power_ctrl_init);
MODULE_DESCRIPTION("Hisilicon secs power ctrl driver");
MODULE_ALIAS("hisi secs power-ctrl module");
MODULE_LICENSE("GPL");

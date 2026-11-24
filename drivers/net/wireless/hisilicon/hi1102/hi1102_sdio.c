// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 apkreader
 *
 * HiSilicon Hi1102 SDIO Wi-Fi driver for Kirin 710 (Phase 1: power-on & SDIO probe).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>

extern void kirin710_sdio_set_card_present(bool present);

#define SDIO_VENDOR_ID_HISI		0x0296
#define SDIO_DEVICE_ID_HI1102		0x5347

/* Hi1102 SDIO Function 1 register offsets (boot ROM & HCC) */
#define HISDIO_REG_FUNC1_FIFO		0x00
#define HISDIO_REG_FUNC1_INT_STATUS	0x08
#define HISDIO_REG_FUNC1_INT_ENABLE	0x09
#define HISDIO_REG_FUNC1_XFER_COUNT	0x0c
#define HISDIO_REG_FUNC1_WRITE_MSG	0x24
#define HISDIO_REG_FUNC1_MSG_FROM_DEV	0x28

struct hi1102_platform {
	struct device *dev;
	struct clk *clk_32k;
	struct gpio_desc *power_gpio;
	struct gpio_desc *wakeup_gpio;
};

static struct hi1102_platform *g_hi1102_plat;

static int hi1102_sdio_probe(struct sdio_func *func,
			     const struct sdio_device_id *id)
{
	int ret;
	u8 int_stat = 0, msg_dev = 0;

	dev_info(&func->dev,
		 "MARIE_WIFI: Hi1102 SDIO card detected! vendor=0x%04x device=0x%04x func=%u class=0x%02x\n",
		 func->vendor, func->device, func->num, func->class);

	sdio_claim_host(func);
	ret = sdio_enable_func(func);
	if (ret) {
		dev_err(&func->dev, "MARIE_WIFI: sdio_enable_func failed: %d\n", ret);
		goto err_release;
	}

	ret = sdio_set_block_size(func, 512);
	if (ret) {
		dev_err(&func->dev, "MARIE_WIFI: sdio_set_block_size(512) failed: %d\n", ret);
		goto err_disable;
	}

	int_stat = sdio_readb(func, HISDIO_REG_FUNC1_INT_STATUS, &ret);
	if (!ret)
		msg_dev = sdio_readb(func, HISDIO_REG_FUNC1_MSG_FROM_DEV, &ret);

	dev_info(&func->dev,
		 "MARIE_WIFI: Hi1102 func1 enabled (blksz=512, int_stat=0x%02x, msg_from_dev=0x%02x, clock=%uHz, bus_width=%u)\n",
		 int_stat, msg_dev,
		 func->card->host->ios.clock,
		 1U << func->card->host->ios.bus_width);

	sdio_release_host(func);
	return 0;

err_disable:
	sdio_disable_func(func);
err_release:
	sdio_release_host(func);
	return ret;
}

static void hi1102_sdio_remove(struct sdio_func *func)
{
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
	dev_info(&func->dev, "MARIE_WIFI: Hi1102 SDIO removed\n");
}

static const struct sdio_device_id hi1102_sdio_ids[] = {
	{ SDIO_DEVICE(SDIO_VENDOR_ID_HISI, SDIO_DEVICE_ID_HI1102) },
	{ }
};
MODULE_DEVICE_TABLE(sdio, hi1102_sdio_ids);

static struct sdio_driver hi1102_sdio_driver = {
	.name = "hi1102_sdio",
	.id_table = hi1102_sdio_ids,
	.probe = hi1102_sdio_probe,
	.remove = hi1102_sdio_remove,
};

static struct clk *hi1102_get_pmu32kb(void)
{
	struct device_node *cnp;
	struct clk *clk = NULL;

	for_each_compatible_node(cnp, NULL, "hisilicon,clk-pmu-gate") {
		const char *cname = NULL;

		if (!of_property_read_string(cnp, "clock-output-names", &cname) &&
		    !strcmp(cname, "clk_pmu32kb")) {
			struct of_phandle_args clkspec = {
				.np = cnp,
				.args_count = 0,
			};

			clk = of_clk_get_from_provider(&clkspec);
			of_node_put(cnp);
			break;
		}
	}
	return clk;
}

static int hi1102_plat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi1102_platform *plat;
	struct device_node *wifi_np;
	int power_gpio, wake_gpio;
	int ret;

	if (!of_machine_is_compatible("hisilicon,kirin710"))
		return -ENODEV;

	plat = devm_kzalloc(dev, sizeof(*plat), GFP_KERNEL);
	if (!plat)
		return -ENOMEM;
	plat->dev = dev;

	/* Look up clk_pmu32kb first so we defer cleanly if PMIC hasn't probed yet */
	plat->clk_32k = hi1102_get_pmu32kb();
	if (IS_ERR(plat->clk_32k)) {
		if (PTR_ERR(plat->clk_32k) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_warn(dev, "MARIE_WIFI: clk_pmu32kb lookup returned %ld\n",
			 PTR_ERR(plat->clk_32k));
		plat->clk_32k = NULL;
	}

	power_gpio = of_get_named_gpio(dev->of_node, "hi1102,gpio_power_on", 0);
	if (power_gpio < 0)
		return dev_err_probe(dev, power_gpio,
				     "MARIE_WIFI: waiting for hi1102,gpio_power_on\n");

	wifi_np = of_find_compatible_node(NULL, NULL, "hisilicon,hisi_wifi");
	wake_gpio = -ENOENT;
	if (wifi_np) {
		wake_gpio = of_get_named_gpio(wifi_np, "hi1102,gpio_wlan_wakeup_host", 0);
		of_node_put(wifi_np);
		if (wake_gpio == -EPROBE_DEFER)
			return -EPROBE_DEFER;
	}

	ret = devm_gpio_request_one(dev, power_gpio, GPIOF_OUT_INIT_LOW,
				    "hi1102_power_on");
	if (ret)
		return dev_err_probe(dev, ret,
				     "MARIE_WIFI: failed to request power_gpio %d\n",
				     power_gpio);
	plat->power_gpio = gpio_to_desc(power_gpio);

	if (wake_gpio >= 0) {
		ret = devm_gpio_request_one(dev, wake_gpio, GPIOF_IN,
					    "hi1102_wlan_wake");
		if (!ret)
			plat->wakeup_gpio = gpio_to_desc(wake_gpio);
		else
			dev_warn(dev, "MARIE_WIFI: wakeup gpio %d request failed: %d\n",
				 wake_gpio, ret);
	}

	if (plat->clk_32k) {
		ret = clk_prepare_enable(plat->clk_32k);
		if (ret)
			dev_warn(dev, "MARIE_WIFI: clk_pmu32kb enable failed: %d\n", ret);
		else
			dev_info(dev, "MARIE_WIFI: clk_pmu32kb enabled\n");
	}

	g_hi1102_plat = plat;

	/* Hi1102 hardware power-on pulse (matches board.c: LOW 10ms -> HIGH 30ms) */
	gpiod_set_value_cansleep(plat->power_gpio, 0);
	msleep(10);
	gpiod_set_value_cansleep(plat->power_gpio, 1);
	msleep(30);

	dev_info(dev, "MARIE_WIFI: Hi1102 powered ON (power_gpio=%d, wake_gpio=%d), triggering SDIO scan\n",
		 power_gpio, wake_gpio);
	kirin710_sdio_set_card_present(true);
	return 0;
}

static const struct of_device_id hi1102_plat_match[] = {
	{ .compatible = "hisilicon,hi1102" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi1102_plat_match);

static struct platform_driver hi1102_plat_driver = {
	.probe = hi1102_plat_probe,
	.driver = {
		.name = "hi1102_platform",
		.of_match_table = hi1102_plat_match,
	},
};

static int __init hi1102_init(void)
{
	int ret;

	ret = sdio_register_driver(&hi1102_sdio_driver);
	if (ret)
		return ret;
	ret = platform_driver_register(&hi1102_plat_driver);
	if (ret)
		sdio_unregister_driver(&hi1102_sdio_driver);
	return ret;
}

static void __exit hi1102_exit(void)
{
	platform_driver_unregister(&hi1102_plat_driver);
	sdio_unregister_driver(&hi1102_sdio_driver);
}

late_initcall(hi1102_init);
module_exit(hi1102_exit);

MODULE_AUTHOR("apkreader");
MODULE_DESCRIPTION("HiSilicon Hi1102 SDIO Wi-Fi driver for Kirin 710");
MODULE_LICENSE("GPL");

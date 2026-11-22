// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/hwspinlock.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox/kirin710.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define KIRIN710_VOLT_LOCK	19
#define KIRIN710_LOW_TEMP	GENMASK(27, 26)
#define KIRIN710_VOLT_CURRENT	GENMASK(29, 28)
#define KIRIN710_VOLT_AVS_OFF	GENMASK(31, 30)
#define KIRIN710_FAST_AVS_CMD	0x000d0002
#define KIRIN710_FAST_AVS_ID	13

struct kirin710_dvfs {
	struct device *dev;
	struct hwspinlock *lock;
	struct mbox_client client;
	struct mbox_chan *channel;
	void __iomem *base;
	resource_size_t size;
};

struct kirin710_dvfs_clk {
	struct clk_hw hw;
	struct kirin710_dvfs *dvfs;
	void __iomem *request;
	void __iomem *status;
	u32 mask;
	u32 volt[2];
	unsigned long threshold_hz;
	unsigned long low_temp_hz;
};

static int kirin710_dvfs_vote(struct kirin710_dvfs_clk *clk, u32 level)
{
	unsigned long flags;
	u32 request;
	int ret;

	ret = hwspin_lock_timeout_irqsave(clk->dvfs->lock, 10, &flags);
	if (ret)
		return ret;

	request = readl(clk->request);
	request &= ~clk->mask;
	request |= level << __ffs(clk->mask);
	writel(request, clk->request);
	hwspin_unlock_irqrestore(clk->dvfs->lock, &flags);
	return 0;
}

static void kirin710_dvfs_unprepare(struct clk_hw *hw)
{
	struct kirin710_dvfs_clk *clk = container_of(hw, struct kirin710_dvfs_clk, hw);

	if (kirin710_dvfs_vote(clk, 0))
		dev_warn(clk->dvfs->dev, "could not clear %s voltage vote\n",
			 clk_hw_get_name(hw));
}

static bool kirin710_dvfs_can_run(struct kirin710_dvfs_clk *clk, unsigned long rate)
{
	return rate <= clk->low_temp_hz || !(readl(clk->status) & KIRIN710_LOW_TEMP);
}

static int kirin710_dvfs_prepare(struct clk_hw *hw)
{
	struct kirin710_dvfs_clk *clk = container_of(hw, struct kirin710_dvfs_clk, hw);
	u32 message[8] = { KIRIN710_FAST_AVS_CMD, KIRIN710_FAST_AVS_ID };
	unsigned long rate = 0;
	u32 status, level = clk->volt[1];
	int ret;

	if (clk->threshold_hz) {
		rate = clk_hw_get_rate(hw);
		if (!rate || !kirin710_dvfs_can_run(clk, rate))
			return -ERANGE;
		level = clk->volt[rate > clk->threshold_hz];
	}

	ret = kirin710_dvfs_vote(clk, level);
	if (ret)
		return ret;

	status = readl(clk->status);
	if (!(status & KIRIN710_VOLT_AVS_OFF) &&
	    FIELD_GET(KIRIN710_VOLT_CURRENT, status) < level) {
		ret = mbox_send_message(clk->dvfs->channel, message);
		if (ret < 0)
			goto clear_vote;
	}

	ret = readl_poll_timeout(clk->status, status,
				 FIELD_GET(KIRIN710_VOLT_CURRENT, status) >= level,
				 150, 120000);
	if (!ret && clk->threshold_hz && !kirin710_dvfs_can_run(clk, rate))
		ret = -ERANGE;
	if (!ret)
		return 0;

clear_vote:
	kirin710_dvfs_unprepare(hw);
	return ret;
}

static int kirin710_secs_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	struct kirin710_dvfs_clk *clk = container_of(hw, struct kirin710_dvfs_clk, hw);
	int ret;

	if (!kirin710_dvfs_can_run(clk, req->rate))
		return -ERANGE;

	ret = clk_hw_determine_rate_no_reparent(hw, req);
	if (ret)
		return ret;

	return kirin710_dvfs_can_run(clk, req->rate) ? 0 : -ERANGE;
}

static const struct clk_ops kirin710_usb_ops = {
	.prepare = kirin710_dvfs_prepare,
	.unprepare = kirin710_dvfs_unprepare,
};

static const struct clk_ops kirin710_secs_ops = {
	.prepare = kirin710_dvfs_prepare,
	.unprepare = kirin710_dvfs_unprepare,
	.determine_rate = kirin710_secs_determine_rate,
};

static void kirin710_dvfs_free_channel(void *channel)
{
	mbox_free_channel(channel);
}

static void kirin710_dvfs_remove_provider(void *node)
{
	of_clk_del_provider(node);
	of_node_put(node);
}

static int kirin710_dvfs_register(struct kirin710_dvfs *dvfs, const char *vote_name,
				 const char *clock_name, u32 request_mask, bool rate_control)
{
	struct device *dev = dvfs->dev;
	struct device_node *vote, *clock;
	struct clk_init_data init = {};
	struct kirin710_dvfs_clk *clk;
	const char *parent;
	u32 request[2], status, id, clock_id, levels, freq, cap = 0, volt[2];
	int ret;

	vote = of_get_child_by_name(dev->of_node, vote_name);
	if (!vote)
		return -ENOENT;

	ret = of_property_read_u32_array(vote, "perivolt-poll-reg", request, 2) ||
	      of_property_read_u32(vote, "perivolt-poll-lpmcu", &status) ||
	      of_property_read_u32(vote, "perivolt-poll-id", &id);
	of_node_put(vote);
	if (ret || request[1] != request_mask || (request[0] & 3) || (status & 3) ||
	    (u64)max(request[0], status) + sizeof(u32) > dvfs->size)
		return -EINVAL;

	clock = of_find_node_by_name(NULL, clock_name);
	if (!clock)
		return -ENOENT;

	ret = -EINVAL;
	if (!of_device_is_compatible(clock, "hisilicon,clkdev-dvfs") ||
	    of_property_read_u32(clock, "hisilicon,clk-devfreq-id", &clock_id) ||
	    id != clock_id ||
	    of_property_read_u32(clock, "hisilicon,clk-dvfs-level", &levels) ||
	    levels != 1 ||
	    of_property_read_u32(clock, "hisilicon,sensitive-freq", &freq) ||
	    of_property_read_u32_array(clock, "hisilicon,sensitive-volt", volt, 2) ||
	    volt[1] > 3 ||
	    of_property_read_string(clock, "clock-output-names", &init.name) ||
	    of_property_read_string(clock, "clock-friend-names", &parent))
		goto put_clock;

	if (rate_control) {
		if (!freq || volt[0] > volt[1] ||
		    !of_property_read_bool(clock, "low_temperature_property") ||
		    of_property_read_u32(clock, "hisilicon,low-temperature-freq", &cap) ||
		    !cap || freq > ULONG_MAX / 1000 || cap > ULONG_MAX / 1000)
			goto put_clock;
		init.ops = &kirin710_secs_ops;
		/* Keep the clock rate stable until its voltage vote is released. */
		init.flags = CLK_SET_RATE_PARENT | CLK_SET_RATE_GATE;
	} else {
		if (freq)
			goto put_clock;
		init.ops = &kirin710_usb_ops;
	}

	ret = -ENOMEM;
	clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		goto put_clock;

	clk->dvfs = dvfs;
	clk->request = dvfs->base + request[0];
	clk->status = dvfs->base + status;
	clk->mask = request[1];
	clk->volt[0] = volt[0];
	clk->volt[1] = volt[1];
	clk->threshold_hz = (unsigned long)freq * 1000;
	clk->low_temp_hz = (unsigned long)cap * 1000;

	init.parent_names = &parent;
	init.num_parents = 1;
	clk->hw.init = &init;
	ret = devm_clk_hw_register(dev, &clk->hw);
	if (ret)
		goto put_clock;

	/* The stock secure-power device has a name lookup and no clocks property. */
	if (rate_control) {
		ret = devm_clk_hw_register_clkdev(dev, &clk->hw, init.name, NULL);
		if (ret)
			goto put_clock;
	}

	ret = of_clk_add_hw_provider(clock, of_clk_hw_simple_get, &clk->hw);
	if (!ret)
		return devm_add_action_or_reset(dev, kirin710_dvfs_remove_provider, clock);

put_clock:
	of_node_put(clock);
	return ret;
}

static int kirin710_dvfs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct kirin710_dvfs *dvfs;
	struct resource *res;
	void *group;
	int ret;

	if (!of_machine_is_compatible("hisilicon,kirin710"))
		return -ENODEV;

	dvfs = devm_kzalloc(dev, sizeof(*dvfs), GFP_KERNEL);
	if (!dvfs)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	dvfs->dev = dev;
	dvfs->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!dvfs->base)
		return -ENOMEM;
	dvfs->size = resource_size(res);
	dvfs->lock = devm_hwspin_lock_request_specific(dev, KIRIN710_VOLT_LOCK);
	if (!dvfs->lock)
		return -EPROBE_DEFER;

	dvfs->client.dev = dev;
	dvfs->client.tx_block = true;
	dvfs->client.tx_tout = 300;
	dvfs->channel = kirin710_mbox_request_clk_channel(&dvfs->client);
	if (IS_ERR(dvfs->channel))
		return PTR_ERR(dvfs->channel);
	ret = devm_add_action_or_reset(dev, kirin710_dvfs_free_channel, dvfs->channel);
	if (ret)
		return ret;

	ret = kirin710_dvfs_register(dvfs, "pvp_usb2otg", "hclk_usb2otg",
				     GENMASK(13, 12), false);
	if (ret)
		return ret;

	/* Leave USB clock support available on boards without the secure clock. */
	group = devres_open_group(dev, NULL, GFP_KERNEL);
	if (group) {
		ret = kirin710_dvfs_register(dvfs, "pvp_secs", "clk_secs",
					     GENMASK(17, 16), true);
		if (ret)
			devres_release_group(dev, group);
		else
			devres_remove_group(dev, group);
	} else {
		ret = -ENOMEM;
	}
	if (ret && ret != -ENOENT)
		dev_warn(dev, "could not register the secure processor clock: %d\n", ret);

	return 0;
}

static const struct of_device_id kirin710_usb_clk_match[] = {
	{ .compatible = "hisilicon,soc-peri-dvfs" },
	{ }
};
MODULE_DEVICE_TABLE(of, kirin710_usb_clk_match);

static struct platform_driver kirin710_usb_clk_driver = {
	.probe = kirin710_dvfs_probe,
	.driver = {
		.name = "kirin710-usb-clk",
		.of_match_table = kirin710_usb_clk_match,
	},
};
module_platform_driver(kirin710_usb_clk_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kirin 710 USB and secure processor DVFS clocks");

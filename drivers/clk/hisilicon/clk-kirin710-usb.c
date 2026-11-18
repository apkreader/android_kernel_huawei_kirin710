// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/hwspinlock.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox/kirin710.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define USB_VOLT_LOCK		19
#define USB_VOLT_CURRENT	GENMASK(29, 28)
#define USB_VOLT_AVS_OFF	GENMASK(31, 30)
#define USB_FAST_AVS_COMMAND	0x000d0002
#define USB_FAST_AVS_ID		13

struct kirin710_usb_clk {
	struct clk_hw hw;
	struct device *dev;
	struct hwspinlock *lock;
	struct mbox_client client;
	struct mbox_chan *channel;
	void __iomem *request;
	void __iomem *status;
	u32 mask;
	u32 level;
};

static int kirin710_usb_vote(struct kirin710_usb_clk *usb, u32 level)
{
	unsigned long flags;
	u32 request;
	int ret;

	ret = hwspin_lock_timeout_irqsave(usb->lock, 10, &flags);
	if (ret)
		return ret;

	request = readl(usb->request);
	request &= ~usb->mask;
	request |= level << __ffs(usb->mask);
	writel(request, usb->request);
	hwspin_unlock_irqrestore(usb->lock, &flags);
	return 0;
}

static int kirin710_usb_prepare(struct clk_hw *hw)
{
	struct kirin710_usb_clk *usb = container_of(hw, struct kirin710_usb_clk, hw);
	u32 message[8] = { USB_FAST_AVS_COMMAND, USB_FAST_AVS_ID };
	u32 status;
	int ret;

	ret = kirin710_usb_vote(usb, usb->level);
	if (ret)
		return ret;

	status = readl(usb->status);
	if (!(status & USB_VOLT_AVS_OFF) &&
	    FIELD_GET(USB_VOLT_CURRENT, status) < usb->level) {
		ret = mbox_send_message(usb->channel, message);
		if (ret < 0)
			goto clear_vote;
	}

	ret = readl_poll_timeout(usb->status, status,
				 FIELD_GET(USB_VOLT_CURRENT, status) >= usb->level,
				 150, 120000);
	if (!ret)
		return 0;

clear_vote:
	if (kirin710_usb_vote(usb, 0))
		dev_warn(usb->dev, "could not clear USB voltage vote\n");
	return ret;
}

static void kirin710_usb_unprepare(struct clk_hw *hw)
{
	struct kirin710_usb_clk *usb = container_of(hw, struct kirin710_usb_clk, hw);

	if (kirin710_usb_vote(usb, 0))
		dev_warn(usb->dev, "could not clear USB voltage vote\n");
}

static const struct clk_ops kirin710_usb_ops = {
	.prepare = kirin710_usb_prepare,
	.unprepare = kirin710_usb_unprepare,
};

static void kirin710_usb_free_channel(void *channel)
{
	mbox_free_channel(channel);
}

static void kirin710_usb_remove_provider(void *node)
{
	of_clk_del_provider(node);
	of_node_put(node);
}

static int kirin710_usb_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *vote, *clock;
	struct clk_init_data init = { .ops = &kirin710_usb_ops };
	struct kirin710_usb_clk *usb;
	struct resource *res;
	void __iomem *base;
	const char *parent;
	u32 request[2], status, id, clock_id, levels, freq, volt[2];
	int ret;

	if (!of_machine_is_compatible("hisilicon,kirin710"))
		return -ENODEV;

	vote = of_get_child_by_name(dev->of_node, "pvp_usb2otg");
	if (!vote)
		return -ENODEV;

	ret = of_property_read_u32_array(vote, "perivolt-poll-reg", request, 2) ||
	      of_property_read_u32(vote, "perivolt-poll-lpmcu", &status) ||
	      of_property_read_u32(vote, "perivolt-poll-id", &id);
	of_node_put(vote);
	if (ret || request[1] != GENMASK(13, 12) || (request[0] & 3) || (status & 3))
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || (u64)max(request[0], status) + sizeof(u32) > resource_size(res))
		return -EINVAL;
	base = devm_ioremap(dev, res->start, resource_size(res));
	if (!base)
		return -ENOMEM;

	clock = of_find_node_by_name(NULL, "hclk_usb2otg");
	if (!clock)
		return -ENODEV;

	ret = -EINVAL;
	if (!of_device_is_compatible(clock, "hisilicon,clkdev-dvfs") ||
	    of_property_read_u32(clock, "hisilicon,clk-devfreq-id", &clock_id) ||
	    id != clock_id ||
	    of_property_read_u32(clock, "hisilicon,clk-dvfs-level", &levels) ||
	    levels != 1 ||
	    of_property_read_u32(clock, "hisilicon,sensitive-freq", &freq) || freq ||
	    of_property_read_u32_array(clock, "hisilicon,sensitive-volt", volt, 2) ||
	    volt[1] > 3 ||
	    of_property_read_string(clock, "clock-output-names", &init.name) ||
	    of_property_read_string(clock, "clock-friend-names", &parent))
		goto put_clock;

	ret = -ENOMEM;
	usb = devm_kzalloc(dev, sizeof(*usb), GFP_KERNEL);
	if (!usb)
		goto put_clock;

	usb->dev = dev;
	usb->request = base + request[0];
	usb->status = base + status;
	usb->mask = request[1];
	usb->level = volt[1];
	usb->lock = devm_hwspin_lock_request_specific(dev, USB_VOLT_LOCK);
	if (!usb->lock) {
		ret = -EPROBE_DEFER;
		goto put_clock;
	}

	usb->client.dev = dev;
	usb->client.tx_block = true;
	usb->client.tx_tout = 300;
	usb->channel = kirin710_mbox_request_clk_channel(&usb->client);
	if (IS_ERR(usb->channel)) {
		ret = PTR_ERR(usb->channel);
		goto put_clock;
	}
	ret = devm_add_action_or_reset(dev, kirin710_usb_free_channel, usb->channel);
	if (ret)
		goto put_clock;

	init.parent_names = &parent;
	init.num_parents = 1;
	usb->hw.init = &init;
	ret = devm_clk_hw_register(dev, &usb->hw);
	if (ret)
		goto put_clock;
	ret = of_clk_add_hw_provider(clock, of_clk_hw_simple_get, &usb->hw);
	if (!ret)
		return devm_add_action_or_reset(dev, kirin710_usb_remove_provider, clock);

put_clock:
	of_node_put(clock);
	return ret;
}

static const struct of_device_id kirin710_usb_clk_match[] = {
	{ .compatible = "hisilicon,soc-peri-dvfs" },
	{ }
};
MODULE_DEVICE_TABLE(of, kirin710_usb_clk_match);

static struct platform_driver kirin710_usb_clk_driver = {
	.probe = kirin710_usb_clk_probe,
	.driver = {
		.name = "kirin710-usb-clk",
		.of_match_table = kirin710_usb_clk_match,
	},
};
module_platform_driver(kirin710_usb_clk_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kirin 710 USB bus clock and voltage vote");

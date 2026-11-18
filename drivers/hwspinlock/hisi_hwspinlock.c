// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon peripheral-controller hardware spinlocks.
 * Based on Huawei's hisi_hwspinlock driver.
 */
#include <linux/io.h>
#include <linux/hwspinlock.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#include "hwspinlock_internal.h"

#define HISI_LOCKS_PER_GROUP	8
#define HISI_LOCK_BITS		4
#define HISI_LOCK_CMD		3
#define HISI_UNLOCK		0x4
#define HISI_LOCK_STATUS	0x8

struct hisi_hwlock {
	void __iomem *reg;
	unsigned int shift;
};

static int hisi_hwlock_trylock(struct hwspinlock *lock)
{
	struct hisi_hwlock *slot = lock->priv;
	u32 status;

	writel(HISI_LOCK_CMD << slot->shift, slot->reg);
	status = readl(slot->reg + HISI_LOCK_STATUS);
	return ((status >> slot->shift) & 0xf) == HISI_LOCK_CMD;
}

static void hisi_hwlock_unlock(struct hwspinlock *lock)
{
	struct hisi_hwlock *slot = lock->priv;

	writel(HISI_LOCK_CMD << slot->shift, slot->reg + HISI_UNLOCK);
}

static const struct hwspinlock_ops hisi_hwlock_ops = {
	.trylock = hisi_hwlock_trylock,
	.unlock = hisi_hwlock_unlock,
};

static int hisi_hwlock_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node, *pctrl;
	struct hwspinlock_device *bank;
	struct hisi_hwlock *slots;
	struct resource res;
	void __iomem *base;
	u32 groups, group, i, offset, width, bits;
	unsigned int count;
	int ret;

	if (!of_machine_is_compatible("hisilicon,kirin710"))
		return -ENODEV;

	if (of_property_read_u32(np, "hwlock,register-width", &width) ||
	    of_property_read_u32(np, "hwlock,bits-per-single", &bits) ||
	    of_property_read_u32(np, "hwlock,groups", &groups) ||
	    width != 32 || bits != HISI_LOCK_BITS || !groups || groups > 8)
		return -EINVAL;

	pctrl = of_find_compatible_node(NULL, NULL, "hisilicon,pctrl");
	if (!pctrl)
		return -ENODEV;

	ret = of_address_to_resource(pctrl, 0, &res);
	of_node_put(pctrl);
	if (ret)
		return ret;

	base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!base)
		return -ENOMEM;

	count = groups * HISI_LOCKS_PER_GROUP;
	bank = devm_kzalloc(dev, struct_size(bank, lock, count), GFP_KERNEL);
	slots = devm_kcalloc(dev, count, sizeof(*slots), GFP_KERNEL);
	if (!bank || !slots)
		return -ENOMEM;

	for (group = 0; group < groups; group++) {
		ret = of_property_read_u32_index(np, "hwlock,offset", group,
						 &offset);
		if (ret || (offset & 3) ||
		    (u64)offset + HISI_LOCK_STATUS + sizeof(u32) > resource_size(&res))
			return -EINVAL;

		for (i = 0; i < HISI_LOCKS_PER_GROUP; i++) {
			unsigned int id = group * HISI_LOCKS_PER_GROUP + i;

			slots[id].reg = base + offset;
			slots[id].shift = i * HISI_LOCK_BITS;
			bank->lock[id].priv = &slots[id];
		}
	}

	return devm_hwspin_lock_register(dev, bank, &hisi_hwlock_ops, 0, count);
}

static const struct of_device_id hisi_hwlock_match[] = {
	{ .compatible = "hisilicon,hwspinlock" },
	{ }
};
MODULE_DEVICE_TABLE(of, hisi_hwlock_match);

static struct platform_driver hisi_hwlock_driver = {
	.probe = hisi_hwlock_probe,
	.driver = {
		.name = "hisi-hwspinlock",
		.of_match_table = hisi_hwlock_match,
	},
};
module_platform_driver(hisi_hwlock_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HiSilicon Kirin 710 hardware spinlocks");

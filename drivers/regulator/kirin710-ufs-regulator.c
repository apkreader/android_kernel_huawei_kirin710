// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hwspinlock.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>
#include <linux/spmi.h>

static const struct regulator_ops kirin710_ufs_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
};

static const struct regmap_config kirin710_pmic_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xffff,
};

#define ABB_REQUEST_REG	0x43c
#define ABB_AP_REQUEST	BIT(0)
#define ABB_LPM_REQUEST	BIT(1)

struct kirin710_abb_clock {
	struct clk_hw hw;
	struct spmi_device *sdev;
	struct hwspinlock *lock;
	void __iomem *request;
	u32 lock_id;
	u16 pmic_reg;
	u8 pmic_mask;
};

static int kirin710_abb_update(struct kirin710_abb_clock *abb, bool enable)
{
	unsigned long flags;
	u32 request;
	u8 value;
	int ret;

	if (!abb->lock) {
		abb->lock = devm_hwspin_lock_request_specific(&abb->sdev->dev,
							     abb->lock_id);
		if (!abb->lock)
			return -EPROBE_DEFER;
	}

	ret = hwspin_lock_timeout_irqsave(abb->lock, 10, &flags);
	if (ret)
		return ret;

	request = readl(abb->request);
	if (enable == !!(request & ABB_AP_REQUEST))
		goto unlock;

	/* The low-power MCU can hold the clock independently of Linux. */
	if (!(request & ABB_LPM_REQUEST)) {
		ret = spmi_ext_register_readl(abb->sdev, abb->pmic_reg, &value, 1);
		if (ret)
			goto unlock;
		if (enable)
			value |= abb->pmic_mask;
		else
			value &= ~abb->pmic_mask;
		ret = spmi_ext_register_writel(abb->sdev, abb->pmic_reg, &value, 1);
		if (ret)
			goto unlock;
	}

	writel(enable ? request | ABB_AP_REQUEST : request & ~ABB_AP_REQUEST,
	       abb->request);

unlock:
	hwspin_unlock_irqrestore(abb->lock, &flags);
	return ret;
}

static int kirin710_abb_prepare(struct clk_hw *hw)
{
	struct kirin710_abb_clock *abb =
		container_of(hw, struct kirin710_abb_clock, hw);
	int ret;

	ret = kirin710_abb_update(abb, true);
	if (!ret)
		usleep_range(1000, 1500);
	return ret;
}

static void kirin710_abb_unprepare(struct clk_hw *hw)
{
	struct kirin710_abb_clock *abb =
		container_of(hw, struct kirin710_abb_clock, hw);
	int ret;

	ret = kirin710_abb_update(abb, false);
	if (ret)
		dev_warn(&abb->sdev->dev, "failed to release ABB clock: %d\n", ret);
}

static const struct clk_ops kirin710_abb_ops = {
	.prepare = kirin710_abb_prepare,
	.unprepare = kirin710_abb_unprepare,
};

static void kirin710_abb_remove_provider(void *data)
{
	struct device_node *node = data;

	of_clk_del_provider(node);
	of_node_put(node);
}

static int kirin710_register_abb(struct spmi_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct device_node *node, *sysctrl;
	struct kirin710_abb_clock *abb;
	struct resource res;
	struct clk_init_data init = { .ops = &kirin710_abb_ops };
	const char *parent;
	u32 gate[2];
	void __iomem *base;
	int ret = 0;

	node = of_find_node_by_name(NULL, "clk_abb_192");
	if (!node)
		return 0;

	if (!of_device_is_compatible(node, "hisilicon,clk-pmu-gate") ||
	    of_property_read_string(node, "clock-output-names", &init.name) ||
	    of_property_read_u32_array(node, "hisilicon,clkgate", gate, 2) ||
	    gate[0] > 0xffff || gate[1] >= 8) {
		ret = -EINVAL;
		goto put_node;
	}

	parent = of_clk_get_parent_name(node, 0);
	if (!parent) {
		ret = -EINVAL;
		goto put_node;
	}
	init.parent_names = &parent;
	init.num_parents = 1;

	sysctrl = of_find_compatible_node(NULL, NULL, "hisilicon,sysctrl");
	if (!sysctrl) {
		ret = -ENODEV;
		goto put_node;
	}
	ret = of_address_to_resource(sysctrl, 0, &res);
	of_node_put(sysctrl);
	if (ret || resource_size(&res) < ABB_REQUEST_REG + sizeof(u32)) {
		ret = -EINVAL;
		goto put_node;
	}

	ret = -ENOMEM;
	abb = devm_kzalloc(dev, sizeof(*abb), GFP_KERNEL);
	if (!abb)
		goto put_node;
	base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!base)
		goto put_node;

	ret = of_property_read_u32(node, "hwspinlock-id", &abb->lock_id);
	if (ret)
		goto put_node;

	abb->sdev = sdev;
	abb->request = base + ABB_REQUEST_REG;
	abb->pmic_reg = gate[0];
	abb->pmic_mask = BIT(gate[1]);
	abb->hw.init = &init;

	ret = devm_clk_hw_register(dev, &abb->hw);
	if (ret)
		goto put_node;
	ret = of_clk_add_hw_provider(node, of_clk_hw_simple_get, &abb->hw);
	if (!ret)
		return devm_add_action_or_reset(dev, kirin710_abb_remove_provider, node);

put_node:
	of_node_put(node);
	return ret;
}


struct kirin710_pmu_gate {
	struct clk_hw hw;
	struct spmi_device *sdev;
	u16 reg;
	u8 mask;
};

static int kirin710_pmu_gate_prepare(struct clk_hw *hw)
{
	struct kirin710_pmu_gate *g = container_of(hw, struct kirin710_pmu_gate, hw);
	u8 val;
	int ret;

	ret = spmi_ext_register_readl(g->sdev, g->reg, &val, 1);
	if (ret)
		return ret;
	val |= g->mask;
	return spmi_ext_register_writel(g->sdev, g->reg, &val, 1);
}

static void kirin710_pmu_gate_unprepare(struct clk_hw *hw)
{
	struct kirin710_pmu_gate *g = container_of(hw, struct kirin710_pmu_gate, hw);
	u8 val;

	if (!spmi_ext_register_readl(g->sdev, g->reg, &val, 1)) {
		val &= ~g->mask;
		spmi_ext_register_writel(g->sdev, g->reg, &val, 1);
	}
}

static int kirin710_pmu_gate_is_prepared(struct clk_hw *hw)
{
	struct kirin710_pmu_gate *g = container_of(hw, struct kirin710_pmu_gate, hw);
	u8 val = 0;

	if (spmi_ext_register_readl(g->sdev, g->reg, &val, 1))
		return 0;
	return !!(val & g->mask);
}

static const struct clk_ops kirin710_pmu_gate_ops = {
	.prepare = kirin710_pmu_gate_prepare,
	.unprepare = kirin710_pmu_gate_unprepare,
	.is_prepared = kirin710_pmu_gate_is_prepared,
};

static int kirin710_register_pmu32kb(struct spmi_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct clk_init_data init = {
		.name = "clk_pmu32kb",
		.ops = &kirin710_pmu_gate_ops,
	};
	struct device_node *node;
	struct kirin710_pmu_gate *g;
	const char *parent;
	u32 gate[2];
	int ret;

	for_each_compatible_node(node, NULL, "hisilicon,clk-pmu-gate") {
		const char *name = NULL;

		if (!of_property_read_string(node, "clock-output-names", &name) &&
		    !strcmp(name, "clk_pmu32kb"))
			break;
	}
	if (!node)
		return -ENOENT;

	if (of_property_read_u32_array(node, "hisilicon,clkgate", gate, 2) ||
	    gate[0] > 0xffff || gate[1] >= 8) {
		of_node_put(node);
		return -EINVAL;
	}

	parent = of_clk_get_parent_name(node, 0);
	if (parent) {
		init.parent_names = &parent;
		init.num_parents = 1;
	}

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g) {
		of_node_put(node);
		return -ENOMEM;
	}

	g->sdev = sdev;
	g->reg = gate[0];
	g->mask = BIT(gate[1]);
	g->hw.init = &init;

	ret = devm_clk_hw_register(dev, &g->hw);
	if (ret) {
		of_node_put(node);
		return ret;
	}
	ret = of_clk_add_hw_provider(node, of_clk_hw_simple_get, &g->hw);
	if (ret) {
		of_node_put(node);
		return ret;
	}
	return devm_add_action_or_reset(dev, kirin710_abb_remove_provider, node);
}

static struct spmi_device *kirin710_pmic_sdev;

struct spmi_device *kirin710_pmic_get_sdev(void)
{
	return kirin710_pmic_sdev;
}
EXPORT_SYMBOL_GPL(kirin710_pmic_get_sdev);

static int kirin710_ufs_regulator_probe(struct spmi_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct regulator_config config = { .dev = dev };
	struct regulator_desc *desc;
	struct device_node *ufs;
	struct device_node *node;
	u32 ctrl[3], vset[2], count;
	unsigned int *table;
	int ret;

	if (!of_machine_is_compatible("hisilicon,kirin710"))
		return -ENODEV;

	ufs = of_find_compatible_node(NULL, NULL, "hisilicon,kirin-ufs");
	if (!ufs)
		return -ENODEV;
	node = of_parse_phandle(ufs, "vcc-supply", 0);
	of_node_put(ufs);
	if (!node)
		return -ENODEV;

	if (node->parent != dev->of_node ||
	    !of_device_is_compatible(node, "hisilicon-hisi-ldo")) {
		ret = -ENODEV;
		goto put_node;
	}

	ret = -EINVAL;
	if (of_property_read_u32_array(node, "hisilicon,hisi-ctrl", ctrl, 3) ||
	    of_property_read_u32_array(node, "hisilicon,hisi-vset", vset, 2) ||
	    of_property_read_u32(node, "hisilicon,hisi-n-voltages", &count) ||
	    ctrl[0] > 0xffff || vset[0] > 0xffff ||
	    !ctrl[1] || ctrl[1] > 0xff || !vset[1] || vset[1] > 0xff)
		goto put_node;

	if (!count || count > (vset[1] >> __ffs(vset[1])) + 1 ||
	    of_property_count_u32_elems(node, "hisilicon,hisi-vset-table") != count)
		goto put_node;

	ret = -ENOMEM;
	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	table = devm_kcalloc(dev, count, sizeof(*table), GFP_KERNEL);
	if (!desc || !table)
		goto put_node;

	ret = of_property_read_u32_array(node, "hisilicon,hisi-vset-table", table, count);
	if (ret)
		goto put_node;

	desc->name = "kirin710-ufs-vcc";
	desc->type = REGULATOR_VOLTAGE;
	desc->owner = THIS_MODULE;
	desc->ops = &kirin710_ufs_ops;
	desc->n_voltages = count;
	desc->volt_table = table;
	desc->enable_reg = ctrl[0];
	desc->enable_mask = ctrl[1];
	desc->vsel_reg = vset[0];
	desc->vsel_mask = vset[1];
	of_property_read_u32(node, "hisilicon,hisi-enable-time-us", &desc->enable_time);
	of_property_read_u32(node, "hisilicon,hisi-off-on-delay-us", &desc->off_on_delay);

	config.regmap = devm_regmap_init_spmi_ext(sdev, &kirin710_pmic_regmap);
	if (IS_ERR(config.regmap)) {
		ret = PTR_ERR(config.regmap);
		goto put_node;
	}
	config.of_node = node;
	config.init_data = of_get_regulator_init_data(dev, node, desc);
	if (!config.init_data) {
		ret = -ENOMEM;
		goto put_node;
	}
	ret = PTR_ERR_OR_ZERO(devm_regulator_register(dev, desc, &config));
	if (!ret && sdev->usid == 0) {
		kirin710_pmic_sdev = sdev;
		int clk_ret = kirin710_register_abb(sdev);

		if (clk_ret)
			dev_warn(dev, "failed to register ABB clock: %d\n", clk_ret);
		clk_ret = kirin710_register_pmu32kb(sdev);
		if (clk_ret && clk_ret != -ENOENT)
			dev_warn(dev, "failed to register clk_pmu32kb: %d\n", clk_ret);
	}

put_node:
	of_node_put(node);
	return ret;
}

static const struct of_device_id kirin710_pmic_ids[] = {
	{ .compatible = "hisilicon-hisi-pmic-spmi" },
	{ }
};
MODULE_DEVICE_TABLE(of, kirin710_pmic_ids);

static struct spmi_driver kirin710_ufs_regulator_driver = {
	.driver = {
		.name = "kirin710-ufs-regulator",
		.of_match_table = kirin710_pmic_ids,
	},
	.probe = kirin710_ufs_regulator_probe,
};
module_spmi_driver(kirin710_ufs_regulator_driver);

MODULE_DESCRIPTION("Kirin 710 UFS supply regulator");
MODULE_LICENSE("GPL");

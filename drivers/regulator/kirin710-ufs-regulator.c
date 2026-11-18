// SPDX-License-Identifier: GPL-2.0-only
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
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

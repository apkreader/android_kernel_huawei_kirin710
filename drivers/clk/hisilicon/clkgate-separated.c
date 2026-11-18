// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hisilicon clock separated gate driver
 *
 * Copyright (c) 2012-2013 Hisilicon Limited.
 * Copyright (c) 2012-2013 Linaro Limited.
 *
 * Author: Haojian Zhuang <haojian.zhuang@linaro.org>
 *	   Xin Li <li.xin@linaro.org>
 */

#include <linux/kernel.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#include "clk.h"

/* clock separated gate register offset */
#define CLKGATE_SEPERATED_ENABLE		0x0
#define CLKGATE_SEPERATED_DISABLE		0x4
#define CLKGATE_SEPERATED_STATUS		0x8

struct clkgate_separated {
	struct clk_hw	hw;
	void __iomem	*enable;	/* enable register */
	u8		bit_idx;	/* bits in enable/disable register */
	u8		flags;
	spinlock_t	*lock;
};

static int clkgate_separated_enable(struct clk_hw *hw)
{
	struct clkgate_separated *sclk;
	unsigned long flags = 0;
	u32 reg;

	sclk = container_of(hw, struct clkgate_separated, hw);
	if (sclk->lock)
		spin_lock_irqsave(sclk->lock, flags);
	reg = BIT(sclk->bit_idx);
	writel_relaxed(reg, sclk->enable);
	readl_relaxed(sclk->enable + CLKGATE_SEPERATED_STATUS);
	if (sclk->lock)
		spin_unlock_irqrestore(sclk->lock, flags);
	return 0;
}

static void clkgate_separated_disable(struct clk_hw *hw)
{
	struct clkgate_separated *sclk;
	unsigned long flags = 0;
	u32 reg;

	sclk = container_of(hw, struct clkgate_separated, hw);
	if (sclk->lock)
		spin_lock_irqsave(sclk->lock, flags);
	reg = BIT(sclk->bit_idx);
	writel_relaxed(reg, sclk->enable + CLKGATE_SEPERATED_DISABLE);
	readl_relaxed(sclk->enable + CLKGATE_SEPERATED_STATUS);
	if (sclk->lock)
		spin_unlock_irqrestore(sclk->lock, flags);
}

static int clkgate_separated_is_enabled(struct clk_hw *hw)
{
	struct clkgate_separated *sclk;
	u32 reg;

	sclk = container_of(hw, struct clkgate_separated, hw);
	reg = readl_relaxed(sclk->enable + CLKGATE_SEPERATED_STATUS);
	reg &= BIT(sclk->bit_idx);

	return reg ? 1 : 0;
}

static const struct clk_ops clkgate_separated_ops = {
	.enable		= clkgate_separated_enable,
	.disable	= clkgate_separated_disable,
	.is_enabled	= clkgate_separated_is_enabled,
};

struct clk *hisi_register_clkgate_sep(struct device *dev, const char *name,
				      const char *parent_name,
				      unsigned long flags,
				      void __iomem *reg, u8 bit_idx,
				      u8 clk_gate_flags, spinlock_t *lock)
{
	struct clkgate_separated *sclk;
	struct clk *clk;
	struct clk_init_data init;

	sclk = kzalloc(sizeof(*sclk), GFP_KERNEL);
	if (!sclk)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &clkgate_separated_ops;
	init.flags = flags;
	init.parent_names = (parent_name ? &parent_name : NULL);
	init.num_parents = (parent_name ? 1 : 0);

	sclk->enable = reg + CLKGATE_SEPERATED_ENABLE;
	sclk->bit_idx = bit_idx;
	sclk->flags = clk_gate_flags;
	sclk->hw.init = &init;
	sclk->lock = lock;

	clk = clk_register(dev, &sclk->hw);
	if (IS_ERR(clk))
		kfree(sclk);
	return clk;
}

#ifdef CONFIG_OF
static void __init kirin710_gate_setup(struct device_node *np)
{
	struct device_node *parent;
	struct resource res;
	void __iomem *base;
	const char *name, *parent_name, *parent_compatible;
	struct clk_hw *hw;
	struct clk *clk;
	u32 gate[2];
	int ret;

	if (!of_machine_is_compatible("hisilicon,kirin710") ||
	    of_property_read_string(np, "clock-output-names", &name))
		return;

	if (!strcmp(name, "clk_ufsio_ref"))
		parent_compatible = "hisilicon,clk-sctrl";
	else if (!strcmp(name, "clk_usb2otg_ref") || !strcmp(name, "clk_usb2otg") ||
		 !strcmp(name, "hclk_usb2otgfreq"))
		parent_compatible = "hisilicon,clk-crgctrl";
	else
		return;

	parent_name = of_clk_get_parent_name(np, 0);
	if (!parent_name ||
	    of_property_read_u32_array(np, "hisilicon,hi3xxx-clkgate", gate, 2) ||
	    (gate[1] & (gate[1] - 1)) ||
	    ((gate[1] == 0) != (strcmp(name, "hclk_usb2otgfreq") == 0)) || (gate[0] & 3))
		return;

	parent = of_get_parent(np);
	if (!parent)
		return;

	ret = of_device_is_compatible(parent, parent_compatible) ?
		of_address_to_resource(parent, 0, &res) : -EINVAL;
	of_node_put(parent);
	if (ret || (u64)gate[0] + CLKGATE_SEPERATED_STATUS + sizeof(u32) >
	    resource_size(&res))
		return;

	/* The firmware uses a zero mask for the bus clock's pass-through. */
	if (!gate[1]) {
		hw = clk_hw_register_fixed_factor(NULL, name, parent_name, 0, 1, 1);
		if (IS_ERR(hw)) {
			pr_err("%pOF: failed to register clock: %ld\n", np, PTR_ERR(hw));
			return;
		}
		ret = of_clk_add_hw_provider(np, of_clk_hw_simple_get, hw);
		if (ret) {
			clk_hw_unregister_fixed_factor(hw);
			pr_err("%pOF: failed to register clock provider: %d\n", np, ret);
		}
		return;
	}

	base = ioremap(res.start, resource_size(&res));
	if (!base)
		return;

	clk = hisi_register_clkgate_sep(NULL, name, parent_name,
				       CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED,
				       base + gate[0], __ffs(gate[1]), 0, NULL);
	if (!IS_ERR(clk)) {
		hw = __clk_get_hw(clk);
		ret = of_clk_add_hw_provider(np, of_clk_hw_simple_get, hw);
		if (!ret)
			return;

		clk_unregister(clk);
		kfree(container_of(hw, struct clkgate_separated, hw));
	} else {
		ret = PTR_ERR(clk);
	}

	iounmap(base);
	pr_err("%pOF: failed to register clock: %d\n", np, ret);
}
CLK_OF_DECLARE(kirin710_gate, "hisilicon,hi3xxx-clk-gate",
	       kirin710_gate_setup);

static void __init kirin710_hiword_gate_setup(struct device_node *np)
{
	struct device_node *parent;
	struct resource res;
	struct clk_hw *hw;
	void __iomem *base;
	const char *name, *parent_name, *parent_compatible;
	u32 gate[2];
	int ret;

	if (!of_machine_is_compatible("hisilicon,kirin710") ||
	    of_property_read_string(np, "clock-output-names", &name) ||
	    !of_property_read_bool(np, "hiword") ||
	    of_property_read_u32_array(np, "hisilicon,clkgate", gate, 2) ||
	    (gate[0] & 3) || gate[1] >= 16)
		return;

	if (!strcmp(name, "clk_usb_tcxo_en"))
		parent_compatible = "hisilicon,clk-pctrl";
	else if (!strcmp(name, "autodiv_sysbus"))
		parent_compatible = "hisilicon,clk-crgctrl";
	else
		return;

	parent_name = of_clk_get_parent_name(np, 0);
	if (!parent_name)
		return;

	parent = of_get_parent(np);
	if (!parent)
		return;
	ret = of_device_is_compatible(parent, parent_compatible) ?
		of_address_to_resource(parent, 0, &res) : -EINVAL;
	of_node_put(parent);
	if (ret || (u64)gate[0] + sizeof(u32) > resource_size(&res))
		return;

	base = ioremap(res.start, resource_size(&res));
	if (!base)
		return;

	hw = clk_hw_register_gate(NULL, name, parent_name, CLK_IGNORE_UNUSED,
				  base + gate[0], gate[1], CLK_GATE_HIWORD_MASK, NULL);
	if (!IS_ERR(hw)) {
		ret = of_clk_add_hw_provider(np, of_clk_hw_simple_get, hw);
		if (!ret)
			return;
		clk_hw_unregister_gate(hw);
	} else {
		ret = PTR_ERR(hw);
	}

	iounmap(base);
	pr_err("%pOF: failed to register clock: %d\n", np, ret);
}
CLK_OF_DECLARE(kirin710_hiword_gate, "hisilicon,clk-gate",
	       kirin710_hiword_gate_setup);
#endif

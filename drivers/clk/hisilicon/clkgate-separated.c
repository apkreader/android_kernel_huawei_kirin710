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
#include <linux/iopoll.h>
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
static void __init kirin710_register_passthrough(struct device_node *np,
					       const char *name,
					       const char *parent)
{
	struct clk_hw *hw;
	int ret;

	hw = clk_hw_register_fixed_factor(NULL, name, parent, 0, 1, 1);
	if (IS_ERR(hw)) {
		pr_err("%pOF: failed to register clock: %ld\n", np, PTR_ERR(hw));
		return;
	}

	ret = of_clk_add_hw_provider(np, of_clk_hw_simple_get, hw);
	if (ret) {
		clk_hw_unregister_fixed_factor(hw);
		pr_err("%pOF: failed to register clock provider: %d\n", np, ret);
	}
}

#define KIRIN710_PPLL2_EN		0x000
#define KIRIN710_PPLL2_DIS		0x004
#define KIRIN710_PPLL2_ENABLE_VOTES	0x008
#define KIRIN710_PPLL2_GT_EN		0x460
#define KIRIN710_PPLL2_GT_DIS		0x464
#define KIRIN710_PPLL2_GATE_VOTES	0x468
#define KIRIN710_PPLL2_CTRL0		0x040
#define KIRIN710_PPLL2_ENABLE_CPU	BIT(3)
#define KIRIN710_PPLL2_GATE_CPU		BIT(18)
#define KIRIN710_PPLL2_LOCKED		BIT(26)

struct kirin710_ppll2 {
	struct clk_hw hw;
	void __iomem *crg;
	void __iomem *pmctrl;
	bool owns_enable;
	bool owns_gate;
};

static int kirin710_ppll2_prepare(struct clk_hw *hw)
{
	struct kirin710_ppll2 *pll = container_of(hw, struct kirin710_ppll2, hw);
	u32 status;
	int ret;

	status = readl(pll->crg + KIRIN710_PPLL2_ENABLE_VOTES);
	pll->owns_enable = !(status & KIRIN710_PPLL2_ENABLE_CPU);
	if (pll->owns_enable)
		writel(KIRIN710_PPLL2_ENABLE_CPU, pll->crg + KIRIN710_PPLL2_EN);

	ret = readl_poll_timeout(pll->pmctrl + KIRIN710_PPLL2_CTRL0, status,
				 status & KIRIN710_PPLL2_LOCKED, 2, 1000);
	if (ret) {
		if (pll->owns_enable)
			writel(KIRIN710_PPLL2_ENABLE_CPU, pll->crg + KIRIN710_PPLL2_DIS);
		return ret;
	}

	status = readl(pll->crg + KIRIN710_PPLL2_GATE_VOTES);
	pll->owns_gate = !(status & KIRIN710_PPLL2_GATE_CPU);
	if (pll->owns_gate)
		writel(KIRIN710_PPLL2_GATE_CPU, pll->crg + KIRIN710_PPLL2_GT_EN);

	return 0;
}

static void kirin710_ppll2_unprepare(struct clk_hw *hw)
{
	struct kirin710_ppll2 *pll = container_of(hw, struct kirin710_ppll2, hw);

	if (pll->owns_gate)
		writel(KIRIN710_PPLL2_GATE_CPU, pll->crg + KIRIN710_PPLL2_GT_DIS);
	if (pll->owns_enable)
		writel(KIRIN710_PPLL2_ENABLE_CPU, pll->crg + KIRIN710_PPLL2_DIS);
}

static const struct clk_ops kirin710_ppll2_ops = {
	.prepare = kirin710_ppll2_prepare,
	.unprepare = kirin710_ppll2_unprepare,
};

static void __init kirin710_ppll2_setup(struct device_node *np,
				       const char *name, const char *parent)
{
	struct clk_init_data init = {
		.name = name,
		.ops = &kirin710_ppll2_ops,
		.flags = CLK_IGNORE_UNUSED,
		.parent_names = &parent,
		.num_parents = 1,
	};
	struct device_node *crg, *pmctrl;
	struct resource crg_res, pmctrl_res;
	struct kirin710_ppll2 *pll;
	int ret = -EINVAL;

	pmctrl = of_get_parent(np);
	crg = of_find_compatible_node(NULL, NULL, "hisilicon,clk-crgctrl");
	if (crg && of_device_is_compatible(pmctrl, "hisilicon,clk-pmctrl") &&
	    !of_address_to_resource(crg, 0, &crg_res) &&
	    !of_address_to_resource(pmctrl, 0, &pmctrl_res) &&
	    resource_size(&crg_res) >= KIRIN710_PPLL2_GATE_VOTES + sizeof(u32) &&
	    resource_size(&pmctrl_res) >= KIRIN710_PPLL2_CTRL0 + sizeof(u32))
		ret = 0;
	of_node_put(crg);
	of_node_put(pmctrl);
	if (ret)
		goto error;

	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll) {
		ret = -ENOMEM;
		goto error;
	}
	pll->crg = ioremap(crg_res.start, resource_size(&crg_res));
	pll->pmctrl = ioremap(pmctrl_res.start, resource_size(&pmctrl_res));
	if (!pll->crg || !pll->pmctrl) {
		ret = -ENOMEM;
		goto free_pll;
	}

	pll->hw.init = &init;
	ret = clk_hw_register(NULL, &pll->hw);
	if (ret)
		goto free_pll;

	ret = of_clk_add_hw_provider(np, of_clk_hw_simple_get, &pll->hw);
	if (!ret)
		return;
	clk_hw_unregister(&pll->hw);
free_pll:
	if (pll->pmctrl)
		iounmap(pll->pmctrl);
	if (pll->crg)
		iounmap(pll->crg);
	kfree(pll);
error:
	pr_err("%pOF: failed to register PPLL2: %d\n", np, ret);
}

static void __init kirin710_ppll_setup(struct device_node *np)
{
	const char *name, *parent;
	u32 enable[2], disable[2];

	if (!of_machine_is_compatible("hisilicon,kirin710") ||
	    of_property_read_string(np, "clock-output-names", &name) ||
	    of_property_read_u32_array(np, "hisilicon,ipc-lpm3-cmd-en", enable, 2) ||
	    of_property_read_u32_array(np, "hisilicon,ipc-lpm3-cmd-dis", disable, 2))
		return;

	parent = of_clk_get_parent_name(np, 0);
	if (!parent)
		return;

	if (!strcmp(name, "clk_ap_ppll0") && !enable[1] && !disable[1]) {
		/* Firmware leaves PPLL0 running. */
		kirin710_register_passthrough(np, name, parent);
	} else if (!strcmp(name, "clk_ap_ppll2") && enable[1] == 2 && disable[1] == 2) {
		kirin710_ppll2_setup(np, name, parent);
	}
}
CLK_OF_DECLARE(kirin710_ppll, "hisilicon,ppll-ctrl", kirin710_ppll_setup);

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

	if (!strcmp(name, "clk_ufsio_ref") || !strcmp(name, "pclk_ao_gpio6"))
		parent_compatible = "hisilicon,clk-sctrl";
	else if (!strcmp(name, "clk_usb2otg_ref") || !strcmp(name, "clk_usb2otg") ||
		 !strcmp(name, "hclk_usb2otgfreq") || !strcmp(name, "pclk_wd0") ||
		 !strcmp(name, "pclk_wd0_high"))
		parent_compatible = "hisilicon,clk-crgctrl";
	else
		return;

	parent_name = of_clk_get_parent_name(np, 0);
	if (!parent_name ||
	    of_property_read_u32_array(np, "hisilicon,hi3xxx-clkgate", gate, 2) ||
	    (gate[1] & (gate[1] - 1)) ||
	    ((gate[1] == 0) != (!strcmp(name, "hclk_usb2otgfreq") ||
			 !strcmp(name, "pclk_wd0"))) || (gate[0] & 3))
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

	/* A zero gate mask denotes a clock with no hardware gate. */
	if (!gate[1]) {
		kirin710_register_passthrough(np, name, parent_name);
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

static void __iomem *__init kirin710_crg_clock_reg(struct device_node *np,
						   u32 offset)
{
	struct device_node *parent = of_get_parent(np);
	struct resource res;
	int ret;

	if (!parent)
		return NULL;
	ret = of_device_is_compatible(parent, "hisilicon,clk-crgctrl") ?
		of_address_to_resource(parent, 0, &res) : -EINVAL;
	of_node_put(parent);
	if (ret || (offset & 3) || (u64)offset + sizeof(u32) > resource_size(&res))
		return NULL;

	return ioremap(res.start + offset, sizeof(u32));
}

static void __init kirin710_mux_setup(struct device_node *np)
{
	const char *name, *parents[2];
	struct clk_hw *hw;
	void __iomem *reg;
	u32 mux[2];
	u8 flags;
	int ret;

	if (!of_machine_is_compatible("hisilicon,kirin710") ||
	    of_property_read_string(np, "clock-output-names", &name) ||
	    of_property_read_u32_array(np, "hisilicon,clkmux-reg", mux, 2))
		return;

	if (!strcmp(name, "clk_wd0_mux") && mux[1] == BIT(17)) {
		flags = CLK_MUX_READ_ONLY;
	} else if (!strcmp(name, "clk_sec_pll_mux") && mux[1] == BIT(5) &&
		   of_property_read_bool(np, "hiword")) {
		flags = CLK_MUX_HIWORD_MASK;
	} else {
		return;
	}

	parents[0] = of_clk_get_parent_name(np, 0);
	parents[1] = of_clk_get_parent_name(np, 1);
	if (!parents[0] || !parents[1])
		return;

	reg = kirin710_crg_clock_reg(np, mux[0]);
	if (!reg)
		return;

	hw = clk_hw_register_mux(NULL, name, parents, ARRAY_SIZE(parents), 0,
				 reg, __ffs(mux[1]), 1, flags, NULL);
	if (!IS_ERR(hw)) {
		ret = of_clk_add_hw_provider(np, of_clk_hw_simple_get, hw);
		if (!ret)
			return;
		clk_hw_unregister_mux(hw);
	}
	iounmap(reg);
}
CLK_OF_DECLARE(kirin710_mux, "hisilicon,hi3xxx-clk-mux", kirin710_mux_setup);

static void __init kirin710_div_setup(struct device_node *np)
{
	const char *name, *parent;
	struct clk_hw *hw;
	void __iomem *reg;
	u32 div[2], range[2];
	u8 flags;
	int ret;

	if (!of_machine_is_compatible("hisilicon,kirin710") ||
	    of_property_read_string(np, "clock-output-names", &name) ||
	    of_property_read_u32_array(np, "hisilicon,clkdiv", div, 2) ||
	    of_property_read_u32_array(np, "hisilicon,clkdiv-table", range, 2) ||
	    range[1] != 1 || of_property_read_bool(np, "double_div"))
		return;

	if (!strcmp(name, "sc_div_cfgbus") && div[1] == GENMASK(1, 0) && range[0] == 4) {
		flags = CLK_DIVIDER_READ_ONLY;
	} else if (!strcmp(name, "clk_sec_div") && div[1] == GENMASK(4, 0) &&
		   range[0] == 32 && of_property_read_bool(np, "hiword")) {
		flags = CLK_DIVIDER_HIWORD_MASK;
	} else {
		return;
	}

	parent = of_clk_get_parent_name(np, 0);
	if (!parent)
		return;

	reg = kirin710_crg_clock_reg(np, div[0]);
	if (!reg)
		return;

	hw = clk_hw_register_divider(NULL, name, parent, 0, reg, 0, hweight32(div[1]),
				     flags, NULL);
	if (!IS_ERR(hw)) {
		ret = of_clk_add_hw_provider(np, of_clk_hw_simple_get, hw);
		if (!ret)
			return;
		clk_hw_unregister_divider(hw);
	}
	iounmap(reg);
}
CLK_OF_DECLARE(kirin710_div, "hisilicon,hi3xxx-clk-div", kirin710_div_setup);

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
	else if (!strcmp(name, "autodiv_sysbus") || !strcmp(name, "clk_gt_sec"))
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

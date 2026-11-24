// SPDX-License-Identifier: GPL-2.0-only
/* Based on Huawei's Kirin 710 DWC3 PHY initialization. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/usb/gadget.h>

#include "core.h"

#define CRG_BACKUP_ENABLE	0x50
#define CRG_BACKUP_DISABLE	0x54
#define CRG_BACKUP_SELECT	0xfc
#define CRG_BACKUP_DIVIDER	0x108
#define USB_BACKUP_GATE		BIT(22)
#define USB_BACKUP_SYS		BIT(6)
#define USB_BACKUP_PLL		BIT(7)
#define USB_BACKUP_SELECT	BIT(8)
#define USB_BACKUP_DIVIDER	GENMASK(15, 10)
#define CRG_HIWORD(value, mask)	((value) | ((mask) << 16))
#define CRG_RST_ENABLE		0x60
#define CRG_RST_DISABLE		0x64
#define CRG_ISO_DISABLE		0x148
#define USB_REFCLK_ISO		BIT(25)
#define USB_RST_CORE		BIT(4)
#define USB_RST_BUS		(BIT(5) | BIT(6) | BIT(25))
#define USB_RST_PHY		BIT(27)
#define USB_RST_PHY_POR		BIT(28)
#define SYS_SLEEP_STATUS	0x08
#define USB_BACKUP_CLOCK	BIT(20)
#define PCTRL_PHY_MUX		0x64
#define USB_PHY_MUX_BACKUP	BIT(25)
#define OTG_CTRL0		0x00
#define OTG_CTRL2		0x08
#define OTG_PHY_TUNING		0x0c

struct kirin710_dwc3 {
	struct clk_bulk_data clocks[2];
	struct clk *tcxo;
	void __iomem *crg;
	bool backup_clock;
};

static struct device *kirin710_dwc3_dev;

static int kirin710_dwc3_match_child(struct device *dev, void *data)
{
	return of_device_is_compatible(dev->of_node, "snps,dwc3");
}

void kirin710_dwc3_vbus_notify(bool vbus_online)
{
	struct device *child;
	struct dwc3 *dwc;

	if (!kirin710_dwc3_dev)
		return;

	child = device_find_child(kirin710_dwc3_dev, NULL,
				  kirin710_dwc3_match_child);
	if (!child)
		return;

	dwc = platform_get_drvdata(to_platform_device(child));
	if (dwc && dwc->gadget) {
		dev_info(child, "Kirin 710 VBUS %s -> notifying UDC\n",
			 vbus_online ? "connected" : "disconnected");
		usb_udc_vbus_handler(dwc->gadget, vbus_online);
	}
	put_device(child);
}
EXPORT_SYMBOL_GPL(kirin710_dwc3_vbus_notify);

static void __iomem *kirin710_dwc3_map(struct device *dev, const char *compatible,
				       unsigned int last_reg)
{
	struct device_node *np;
	struct resource res;
	void __iomem *base;
	int ret;

	np = of_find_compatible_node(NULL, NULL, compatible);
	if (!np)
		return ERR_PTR(-ENODEV);

	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret)
		return ERR_PTR(ret);
	if (resource_size(&res) < last_reg + sizeof(u32))
		return ERR_PTR(-EINVAL);

	base = devm_ioremap(dev, res.start, resource_size(&res));
	return base ?: ERR_PTR(-ENOMEM);
}

static int kirin710_dwc3_refclk_enable(struct kirin710_dwc3 *usb,
				     void __iomem *pctrl)
{
	u32 mux = readl(pctrl + PCTRL_PHY_MUX);
	int ret;

	if (usb->backup_clock) {
		writel(USB_BACKUP_GATE, usb->crg + CRG_BACKUP_DISABLE);
		writel(CRG_HIWORD(USB_BACKUP_SELECT, USB_BACKUP_SELECT),
		       usb->crg + CRG_BACKUP_SELECT);
		writel(CRG_HIWORD(USB_BACKUP_DIVIDER, USB_BACKUP_DIVIDER),
		       usb->crg + CRG_BACKUP_DIVIDER);
		writel(CRG_HIWORD(USB_BACKUP_PLL, USB_BACKUP_PLL),
		       usb->crg + CRG_BACKUP_SELECT);
		writel(mux | USB_PHY_MUX_BACKUP, pctrl + PCTRL_PHY_MUX);
		writel(USB_BACKUP_GATE, usb->crg + CRG_BACKUP_ENABLE);
		return 0;
	}

	writel(USB_REFCLK_ISO, usb->crg + CRG_ISO_DISABLE);
	ret = clk_prepare_enable(usb->tcxo);
	if (ret)
		return ret;
	usleep_range(1000, 1500);
	writel(mux & ~USB_PHY_MUX_BACKUP, pctrl + PCTRL_PHY_MUX);
	return 0;
}

static void kirin710_dwc3_power_off(void *data)
{
	struct kirin710_dwc3 *usb = data;

	kirin710_dwc3_dev = NULL;

	writel(USB_RST_CORE, usb->crg + CRG_RST_ENABLE);
	writel(USB_RST_PHY, usb->crg + CRG_RST_ENABLE);
	writel(USB_RST_PHY_POR, usb->crg + CRG_RST_ENABLE);
	if (usb->backup_clock) {
		writel(USB_BACKUP_GATE, usb->crg + CRG_BACKUP_DISABLE);
		writel(CRG_HIWORD(0, USB_BACKUP_PLL | USB_BACKUP_SYS),
		       usb->crg + CRG_BACKUP_SELECT);
	} else {
		clk_disable_unprepare(usb->tcxo);
	}
	writel(USB_RST_BUS, usb->crg + CRG_RST_ENABLE);
	clk_bulk_disable_unprepare(ARRAY_SIZE(usb->clocks), usb->clocks);
}

static int kirin710_dwc3_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct kirin710_dwc3 *usb;
	void __iomem *sysctrl, *pctrl, *phy;
	u32 tuning;
	int ret;

	if (!of_machine_is_compatible("hisilicon,kirin710"))
		return -ENODEV;
	ret = of_property_read_u32(dev->of_node, "eye_diagram_param", &tuning);
	if (ret)
		return ret;

	usb = devm_kzalloc(dev, sizeof(*usb), GFP_KERNEL);
	if (!usb)
		return -ENOMEM;

	usb->crg = kirin710_dwc3_map(dev, "hisilicon,crgctrl", CRG_ISO_DISABLE);
	if (IS_ERR(usb->crg))
		return PTR_ERR(usb->crg);
	sysctrl = kirin710_dwc3_map(dev, "hisilicon,sysctrl", SYS_SLEEP_STATUS);
	if (IS_ERR(sysctrl))
		return PTR_ERR(sysctrl);
	pctrl = kirin710_dwc3_map(dev, "hisilicon,pctrl", PCTRL_PHY_MUX);
	if (IS_ERR(pctrl))
		return PTR_ERR(pctrl);
	phy = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	usb->backup_clock = !!(readl(sysctrl + SYS_SLEEP_STATUS) & USB_BACKUP_CLOCK);

	usb->clocks[0].id = "hclk_usb2otg";
	usb->clocks[1].id = "clk_usb2phy_ref";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(usb->clocks), usb->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get USB clocks\n");
	if (!usb->backup_clock) {
		usb->tcxo = devm_clk_get(dev, "clk_usb2_tcxo_en");
		if (IS_ERR(usb->tcxo))
			return dev_err_probe(dev, PTR_ERR(usb->tcxo),
					     "failed to get TCXO\n");
	}

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(usb->clocks), usb->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable USB clocks\n");

	writel(USB_RST_BUS, usb->crg + CRG_RST_DISABLE);
	ret = kirin710_dwc3_refclk_enable(usb, pctrl);
	if (ret) {
		writel(USB_RST_BUS, usb->crg + CRG_RST_ENABLE);
		clk_bulk_disable_unprepare(ARRAY_SIZE(usb->clocks), usb->clocks);
		return dev_err_probe(dev, ret, "failed to enable PHY reference clock\n");
	}
	ret = devm_add_action_or_reset(dev, kirin710_dwc3_power_off, usb);
	if (ret)
		return ret;


	writel(USB_RST_PHY_POR, usb->crg + CRG_RST_DISABLE);
	udelay(100);
	writel(USB_RST_PHY, usb->crg + CRG_RST_DISABLE);
	udelay(100);
	writel(USB_RST_CORE, usb->crg + CRG_RST_DISABLE);
	udelay(100);

	writel(readl(phy + OTG_CTRL2) | BIT(3) | BIT(2), phy + OTG_CTRL2);
	writel(readl(phy + OTG_CTRL0) | GENMASK(11, 10), phy + OTG_CTRL0);
	usleep_range(1000, 1500);
	writel(tuning, phy + OTG_PHY_TUNING);

	kirin710_dwc3_dev = dev;
	return devm_of_platform_populate(dev);
}

static const struct of_device_id kirin710_dwc3_match[] = {
	{ .compatible = "hisilicon,nov-dwc3" },
	{ }
};
MODULE_DEVICE_TABLE(of, kirin710_dwc3_match);

static struct platform_driver kirin710_dwc3_driver = {
	.probe = kirin710_dwc3_probe,
	.driver = {
		.name = "kirin710-dwc3",
		.of_match_table = kirin710_dwc3_match,
	},
};
module_platform_driver(kirin710_dwc3_driver);

MODULE_DESCRIPTION("Kirin 710 DWC3 USB glue");
MODULE_LICENSE("GPL");

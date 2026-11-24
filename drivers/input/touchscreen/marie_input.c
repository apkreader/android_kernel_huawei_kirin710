// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 apkreader
 *
 * Huawei P30 lite (marie / Kirin 710) input drivers:
 *  1. Volume Up / Volume Down GPIO keys (hisilicon,gpio-key)
 *  2. SPMI PMIC Power Key (hisilicon-hisi-powerkey / pmic@0)
 *  3. Synaptics TD4320 ZeroFlash TouchComm SPI touchscreen (huawei,ts_kit)
 */

#include <linux/arch_topology.h>
#include <linux/clk-provider.h>
#include <linux/cpufreq.h>
#include <linux/cpuset.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spmi.h>
#include <linux/workqueue.h>

extern struct spmi_device *kirin710_pmic_get_sdev(void);
extern void kirin710_dwc3_vbus_notify(bool vbus_online);
extern void kirin710_dwc3_typec_set_connected(bool connected);
extern bool kirin710_dwc3_is_configured(void);
extern struct spi_controller *marie_get_spi1_ctlr(void);
extern int kirin710_xfreq_send_mhz(u32 cmd0, u32 mhz);

static struct gpio_desc *marie_get_dt_gpiod(struct device_node *np,
					    const char *prop,
					    enum gpiod_flags dflags)
{
	struct of_phandle_args args;
	struct gpio_device *gdev;
	struct gpio_desc *desc;
	int ret;

	ret = of_parse_phandle_with_args(np, prop, "#gpio-cells", 0, &args);
	if (ret)
		return ERR_PTR(ret);

	gdev = gpio_device_find_by_fwnode(of_fwnode_handle(args.np));
	of_node_put(args.np);
	if (!gdev)
		return ERR_PTR(-EPROBE_DEFER);

	desc = gpio_device_get_desc(gdev, args.args[0]);
	gpio_device_put(gdev);
	if (IS_ERR(desc))
		return desc;

	if (dflags == GPIOD_IN)
		ret = gpiod_direction_input(desc);
	else if (dflags == GPIOD_OUT_HIGH)
		ret = gpiod_direction_output(desc, 1);
	else if (dflags == GPIOD_OUT_LOW)
		ret = gpiod_direction_output(desc, 0);
	else
		ret = 0;

	return ret ? ERR_PTR(ret) : desc;
}

/* =========================================================================
 * 1. Volume Up / Volume Down GPIO Keys (hisilicon,gpio-key)
 * ========================================================================= */

struct marie_gpio_keys {
	struct input_dev *input;
	struct gpio_desc *vol_up;
	struct gpio_desc *vol_down;
	int irq_up;
	int irq_down;
	struct delayed_work work_up;
	struct delayed_work work_down;
};

static void marie_vol_up_work(struct work_struct *work)
{
	struct marie_gpio_keys *gk =
		container_of(to_delayed_work(work), struct marie_gpio_keys, work_up);
	int pressed = (gpiod_get_raw_value_cansleep(gk->vol_up) == 0);

	pr_info("MARIE_KEY: KEY_VOLUMEUP %s\n", pressed ? "DOWN" : "UP");
	input_report_key(gk->input, KEY_VOLUMEUP, pressed);
	input_sync(gk->input);
}

static void marie_vol_down_work(struct work_struct *work)
{
	struct marie_gpio_keys *gk =
		container_of(to_delayed_work(work), struct marie_gpio_keys, work_down);
	int pressed = (gpiod_get_raw_value_cansleep(gk->vol_down) == 0);

	pr_info("MARIE_KEY: KEY_VOLUMEDOWN %s\n", pressed ? "DOWN" : "UP");
	input_report_key(gk->input, KEY_VOLUMEDOWN, pressed);
	input_sync(gk->input);
}

static irqreturn_t marie_gpio_key_irq(int irq, void *data)
{
	struct marie_gpio_keys *gk = data;

	if (irq == gk->irq_up)
		mod_delayed_work(system_wq, &gk->work_up, msecs_to_jiffies(20));
	else if (irq == gk->irq_down)
		mod_delayed_work(system_wq, &gk->work_down, msecs_to_jiffies(20));

	return IRQ_HANDLED;
}

static int marie_gpio_keys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct marie_gpio_keys *gk;
	int ret;

	gk = devm_kzalloc(dev, sizeof(*gk), GFP_KERNEL);
	if (!gk)
		return -ENOMEM;

	gk->vol_up = marie_get_dt_gpiod(dev->of_node, "gpio-keyup,gpio-irq", GPIOD_IN);
	if (IS_ERR(gk->vol_up))
		return PTR_ERR(gk->vol_up);

	gk->vol_down = marie_get_dt_gpiod(dev->of_node, "gpio-keydown,gpio-irq", GPIOD_IN);
	if (IS_ERR(gk->vol_down))
		return PTR_ERR(gk->vol_down);

	gk->input = devm_input_allocate_device(dev);
	if (!gk->input)
		return -ENOMEM;

	gk->input->name = "hisi_gpio_key";
	gk->input->phys = "hisi_gpio_key/input0";
	gk->input->id.bustype = BUS_HOST;
	input_set_capability(gk->input, EV_KEY, KEY_VOLUMEUP);
	input_set_capability(gk->input, EV_KEY, KEY_VOLUMEDOWN);

	ret = input_register_device(gk->input);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&gk->work_up, marie_vol_up_work);
	INIT_DELAYED_WORK(&gk->work_down, marie_vol_down_work);

	gk->irq_up = gpiod_to_irq(gk->vol_up);
	gk->irq_down = gpiod_to_irq(gk->vol_down);
	if (gk->irq_up < 0 || gk->irq_down < 0) {
		dev_err(dev, "failed to map gpio key irqs (%d, %d)\n",
			gk->irq_up, gk->irq_down);
		return -EINVAL;
	}

	ret = devm_request_irq(dev, gk->irq_up, marie_gpio_key_irq,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_NO_SUSPEND,
			       "marie-vol-up", gk);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, gk->irq_down, marie_gpio_key_irq,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_NO_SUSPEND,
			       "marie-vol-down", gk);
	if (ret)
		return ret;

	pr_info("MARIE_KEY: gpio keys registered (up_irq=%d down_irq=%d)\n",
		gk->irq_up, gk->irq_down);
	return 0;
}

static const struct of_device_id marie_gpio_keys_of_match[] = {
	{ .compatible = "hisilicon,gpio-key" },
	{ }
};

static struct platform_driver marie_gpio_keys_driver = {
	.probe = marie_gpio_keys_probe,
	.driver = {
		.name = "marie-gpio-keys",
		.of_match_table = marie_gpio_keys_of_match,
	},
};

/* =========================================================================
 * 2. SPMI PMIC Power Key (pmic@0 / ponkey@b1)
 * ========================================================================= */

#define PMIC_IRQ_MASK0_REG	0x18c
#define PMIC_IRQ_MASK1_REG	0x18d
#define PMIC_IRQ_STAT0_REG	0x19b
#define PMIC_IRQ_STAT1_REG	0x19c
#define PMIC_PONKEY_DOWN_BIT	BIT(7)
#define PMIC_PONKEY_UP_BIT	BIT(6)
#define PMIC_VBUS_DISCONNECT_BIT BIT(2)
#define PMIC_VBUS_CONNECT_BIT	BIT(1)

static void marie_bat_vbus_irq_notify(void);

struct marie_ponkey {
	struct spmi_device *sdev;
	struct input_dev *input;
	struct gpio_desc *irq_gpiod;
	int irq;
	struct delayed_work init_work;
	int retries;
};

static struct marie_ponkey marie_pk;

static irqreturn_t marie_ponkey_irq_thread(int irq, void *data)
{
	struct marie_ponkey *pk = data;
	u8 st0 = 0, st1 = 0;

	if (spmi_ext_register_readl(pk->sdev, PMIC_IRQ_STAT0_REG, &st0, 1) == 0 && st0)
		spmi_ext_register_writel(pk->sdev, PMIC_IRQ_STAT0_REG, &st0, 1);
	if (spmi_ext_register_readl(pk->sdev, PMIC_IRQ_STAT1_REG, &st1, 1) == 0 && st1)
		spmi_ext_register_writel(pk->sdev, PMIC_IRQ_STAT1_REG, &st1, 1);

	/* Report DOWN before UP if both arrived together */
	if (st0 & PMIC_PONKEY_DOWN_BIT) {
		pr_info("MARIE_KEY: KEY_POWER DOWN (st0=0x%02x)\n", st0);
		input_report_key(pk->input, KEY_POWER, 1);
		input_sync(pk->input);
	}
	if (st0 & PMIC_PONKEY_UP_BIT) {
		pr_info("MARIE_KEY: KEY_POWER UP (st0=0x%02x)\n", st0);
		input_report_key(pk->input, KEY_POWER, 0);
		input_sync(pk->input);
	}
	if (st0 & (PMIC_VBUS_CONNECT_BIT | PMIC_VBUS_DISCONNECT_BIT)) {
		pr_info("MARIE_BAT: PMIC VBUS IRQ (st0=0x%02x)\n", st0);
		marie_bat_vbus_irq_notify();
	}

	return IRQ_HANDLED;
}

static void marie_ponkey_init_work(struct work_struct *work)
{
	struct marie_ponkey *pk =
		container_of(to_delayed_work(work), struct marie_ponkey, init_work);
	struct device_node *np;
	u8 mask0 = 0xff, mask1 = 0xff, clr = 0xff;
	int ret;

	pk->sdev = kirin710_pmic_get_sdev();
	if (!pk->sdev) {
		if (++pk->retries < 40)
			schedule_delayed_work(&pk->init_work, msecs_to_jiffies(250));
		return;
	}

	np = pk->sdev->dev.of_node;
	pk->irq_gpiod = marie_get_dt_gpiod(np, "gpios", GPIOD_IN);
	if (IS_ERR(pk->irq_gpiod)) {
		if (PTR_ERR(pk->irq_gpiod) == -EPROBE_DEFER && ++pk->retries < 40)
			schedule_delayed_work(&pk->init_work, msecs_to_jiffies(250));
		else
			pr_err("MARIE_PONKEY: gpios failed: %ld\n", PTR_ERR(pk->irq_gpiod));
		return;
	}

	pk->input = input_allocate_device();
	if (!pk->input)
		return;
	pk->input->name = "hisi_on";
	pk->input->phys = "hisi_on/input0";
	pk->input->id.bustype = BUS_HOST;
	input_set_capability(pk->input, EV_KEY, KEY_POWER);

	ret = input_register_device(pk->input);
	if (ret) {
		input_free_device(pk->input);
		pk->input = NULL;
		return;
	}

	/* Unmask powerkey (bits 7,6) and VBUS insert/remove (bits 2,1) */
	mask0 = (u8)(~(PMIC_PONKEY_DOWN_BIT | PMIC_PONKEY_UP_BIT |
		       PMIC_VBUS_CONNECT_BIT | PMIC_VBUS_DISCONNECT_BIT) & 0xff);
	mask1 = 0xff;
	spmi_ext_register_writel(pk->sdev, PMIC_IRQ_MASK0_REG, &mask0, 1);
	spmi_ext_register_writel(pk->sdev, PMIC_IRQ_MASK1_REG, &mask1, 1);
	spmi_ext_register_writel(pk->sdev, PMIC_IRQ_STAT0_REG, &clr, 1);
	spmi_ext_register_writel(pk->sdev, PMIC_IRQ_STAT1_REG, &clr, 1);

	pk->irq = gpiod_to_irq(pk->irq_gpiod);
	ret = request_threaded_irq(pk->irq, NULL, marie_ponkey_irq_thread,
				   IRQF_TRIGGER_LOW | IRQF_ONESHOT | IRQF_NO_SUSPEND,
				   "marie-ponkey", pk);
	if (ret) {
		pr_err("MARIE_PONKEY: request_threaded_irq(%d) failed: %d\n", pk->irq, ret);
		return;
	}

	enable_irq_wake(pk->irq);
	pr_info("MARIE_KEY: PMIC powerkey+VBUS IRQ registered (irq=%d, mask0=0x%02x)\n", pk->irq, mask0);
}

/* =========================================================================
 * 3. Synaptics TD4320 ZeroFlash TouchComm SPI Touchscreen (huawei,ts_kit)
 * ========================================================================= */

#define TCM_MARKER			0xa5
#define TCM_PADDING			0x5a
#define TCM_HDR_SIZE			4

#define TCM_PDT_START_ADDR		0x00e9
#define TCM_PDT_END_ADDR		0x00ee
#define TCM_UBL_FN_NUMBER		0x35
#define TCM_F35_CTRL3_OFFSET		18
#define TCM_F35_CTRL7_OFFSET		22
#define TCM_F35_WRITE_FW_TO_PMEM	4

#define TCM_IMG_MAGIC			0x4818472bU
#define TCM_AREA_MAGIC			0x7c05e516U

#define TCM_CMD_CONTINUE_WRITE		0x01
#define TCM_CMD_IDENTIFY		0x02
#define TCM_CMD_ENABLE_REPORT		0x05
#define TCM_CMD_DISABLE_REPORT		0x06
#define TCM_CMD_GET_APP_INFO		0x20
#define TCM_CMD_GET_TOUCH_CFG		0x25
#define TCM_CMD_DOWNLOAD_CONFIG		0x30

#define TCM_STATUS_OK			0x01
#define TCM_STATUS_CONTINUED_READ	0x03
#define TCM_REPORT_IDENTIFY		0x10
#define TCM_REPORT_TOUCH		0x11
#define TCM_REPORT_STATUS		0x1b

#define TCM_HDL_TOUCH_CFG_PMEM		1
#define TCM_HDL_DISP_CFG_PMEM		2

#define TCM_MAX_OBJECTS			10
#define TCM_TOUCH_CFG_MAX		128
#define TCM_SPI_SPEED_HZ		4000000

enum tcm_touch_code {
	TOUCH_END = 0,
	TOUCH_FOREACH_ACTIVE_OBJECT,
	TOUCH_FOREACH_OBJECT,
	TOUCH_FOREACH_END,
	TOUCH_PAD_TO_NEXT_BYTE,
	TOUCH_TIMESTAMP,
	TOUCH_OBJECT_N_INDEX,
	TOUCH_OBJECT_N_CLASSIFICATION,
	TOUCH_OBJECT_N_X_POSITION,
	TOUCH_OBJECT_N_Y_POSITION,
	TOUCH_OBJECT_N_Z,
	TOUCH_OBJECT_N_X_WIDTH,
	TOUCH_OBJECT_N_Y_WIDTH,
	TOUCH_OBJECT_N_TX_POSITION_TIXELS,
	TOUCH_OBJECT_N_RX_POSITION_TIXELS,
	TOUCH_0D_BUTTONS_STATE,
	TOUCH_GESTURE_DOUBLE_TAP,
	TOUCH_FRAME_RATE,
	TOUCH_POWER_IM,
	TOUCH_CID_IM,
	TOUCH_RAIL_IM,
	TOUCH_CID_VARIANCE_IM,
	TOUCH_NSM_FREQUENCY,
	TOUCH_NSM_STATE,
	TOUCH_NUM_OF_ACTIVE_OBJECTS,
	TOUCH_NUM_OF_CPU_CYCLES_USED_SINCE_LAST_FRAME,
	TOUCH_ROI_DATA = 0xCA,
	TOUCH_GRIP_DATA = 0xCB,
	TOUCH_ESD_DETECT = 0xCC,
};

struct tcm_area_desc {
	u8 magic[4];
	u8 id_str[16];
	u8 flags[4];
	u8 flash_addr_words[4];
	u8 length[4];
	u8 checksum[4];
} __packed;

struct tcm_img_hdr {
	u8 magic[4];
	u8 num_areas[4];
} __packed;

struct tcm_pdt_entry {
	u8 query_base;
	u8 cmd_base;
	u8 ctrl_base;
	u8 data_base;
	u8 intr_src_count:3;
	u8 reserved1:2;
	u8 fn_ver:2;
	u8 reserved2:1;
	u8 fn_number;
} __packed;

struct tcm_obj_data {
	u8 status;
	u32 x;
	u32 y;
	u32 z;
	u32 wx;
	u32 wy;
};

struct marie_tcm {
	struct device *dev;
	struct spi_device *spi;
	struct input_dev *input;
	struct gpio_desc *cs_gpiod;
	struct gpio_desc *reset_gpiod;
	struct gpio_desc *irq_gpiod;
	int irq;
	struct mutex io_lock;
	struct delayed_work init_work;
	int retries;
	char fw_name[128];

	u32 max_x;
	u32 max_y;
	u32 max_objects;
	u8 report_cfg[TCM_TOUCH_CFG_MAX];
	u32 report_cfg_len;

	u8 *xfer_tx;
	u8 *xfer_rx;
	size_t xfer_cap;
	u8 irq_pkt[512];
	u8 prev_status[TCM_MAX_OBJECTS];
	u32 touch_events;
	u32 bad_irqs;
};

static inline u32 tcm_le2(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8);
}

static inline u32 tcm_le4(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int tcm_ensure_buf(struct marie_tcm *ts, size_t need)
{
	if (need <= ts->xfer_cap)
		return 0;
	kfree(ts->xfer_tx);
	kfree(ts->xfer_rx);
	ts->xfer_tx = kmalloc(need, GFP_KERNEL);
	ts->xfer_rx = kmalloc(need, GFP_KERNEL);
	if (!ts->xfer_tx || !ts->xfer_rx) {
		ts->xfer_cap = 0;
		return -ENOMEM;
	}
	ts->xfer_cap = need;
	return 0;
}

static int tcm_spi_write(struct marie_tcm *ts, const u8 *data, size_t len)
{
	struct spi_transfer xfer = {
		.tx_buf = data,
		.len = len,
		.speed_hz = TCM_SPI_SPEED_HZ,
		.bits_per_word = 8,
	};
	int ret;

	gpiod_set_raw_value_cansleep(ts->cs_gpiod, 0);
	udelay(5);
	ret = spi_sync_transfer(ts->spi, &xfer, 1);
	udelay(5);
	gpiod_set_raw_value_cansleep(ts->cs_gpiod, 1);
	return ret;
}

static int tcm_spi_read(struct marie_tcm *ts, u8 *data, size_t len)
{
	struct spi_transfer xfer;
	int ret;

	ret = tcm_ensure_buf(ts, len);
	if (ret)
		return ret;
	memset(ts->xfer_tx, 0xff, len);
	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf = ts->xfer_tx;
	xfer.rx_buf = data;
	xfer.len = len;
	xfer.speed_hz = TCM_SPI_SPEED_HZ;
	xfer.bits_per_word = 8;

	gpiod_set_raw_value_cansleep(ts->cs_gpiod, 0);
	udelay(5);
	ret = spi_sync_transfer(ts->spi, &xfer, 1);
	udelay(5);
	gpiod_set_raw_value_cansleep(ts->cs_gpiod, 1);
	return ret;
}

static int tcm_rmi_read(struct marie_tcm *ts, u16 addr, u8 *data, size_t len)
{
	u8 hdr[2];
	struct spi_transfer xfers[2];
	int ret;

	ret = tcm_ensure_buf(ts, len);
	if (ret)
		return ret;
	hdr[0] = ((addr >> 8) & 0x7f) | 0x80;
	hdr[1] = addr & 0xff;
	memset(ts->xfer_tx, 0xff, len);
	memset(xfers, 0, sizeof(xfers));
	xfers[0].tx_buf = hdr;
	xfers[0].len = 2;
	xfers[0].speed_hz = 100000;
	xfers[0].bits_per_word = 8;
	xfers[1].tx_buf = ts->xfer_tx;
	xfers[1].rx_buf = data;
	xfers[1].len = len;
	xfers[1].speed_hz = 100000;
	xfers[1].bits_per_word = 8;

	ts->spi->max_speed_hz = 100000;
	spi_setup(ts->spi);

	gpiod_set_raw_value_cansleep(ts->cs_gpiod, 0);
	udelay(10);
	ret = spi_sync_transfer(ts->spi, xfers, 2);
	udelay(10);
	gpiod_set_raw_value_cansleep(ts->cs_gpiod, 1);

	ts->spi->max_speed_hz = TCM_SPI_SPEED_HZ;
	spi_setup(ts->spi);
	return ret;
}

static int tcm_rmi_write(struct marie_tcm *ts, u16 addr, const u8 *data, size_t len)
{
	u8 *buf;
	struct spi_transfer xfer;
	int ret;

	buf = kmalloc(len + 2, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = (addr >> 8) & 0x7f;
	buf[1] = addr & 0xff;
	memcpy(&buf[2], data, len);

	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf = buf;
	xfer.len = len + 2;
	xfer.speed_hz = TCM_SPI_SPEED_HZ;
	xfer.bits_per_word = 8;

	ts->spi->max_speed_hz = TCM_SPI_SPEED_HZ;
	spi_setup(ts->spi);

	gpiod_set_raw_value_cansleep(ts->cs_gpiod, 0);
	udelay(10);
	ret = spi_sync_transfer(ts->spi, &xfer, 1);
	udelay(10);
	gpiod_set_raw_value_cansleep(ts->cs_gpiod, 1);

	kfree(buf);
	return ret;
}

static int tcm_write_cmd(struct marie_tcm *ts, u8 cmd, const u8 *payload,
			 size_t len, size_t wr_chunk)
{
	size_t rem = len + 2;
	size_t chunk_space = wr_chunk ? (wr_chunk - 1) : rem;
	size_t chunks = DIV_ROUND_UP(rem, chunk_space);
	size_t idx, xfer_len;
	u8 *buf;
	int ret = 0;

	if (chunks == 0)
		chunks = 1;
	buf = kmalloc(chunk_space + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (idx = 0; idx < chunks; idx++) {
		xfer_len = min(rem, chunk_space);
		if (idx == 0) {
			buf[0] = cmd;
			buf[1] = len & 0xff;
			buf[2] = (len >> 8) & 0xff;
			if (xfer_len > 2 && payload)
				memcpy(&buf[3], payload, xfer_len - 2);
		} else {
			buf[0] = TCM_CMD_CONTINUE_WRITE;
			memcpy(&buf[1], &payload[idx * chunk_space - 2], xfer_len);
		}
		ret = tcm_spi_write(ts, buf, xfer_len + 1);
		if (ret)
			break;
		rem -= xfer_len;
		if (chunks > 1)
			usleep_range(500, 1000);
	}

	kfree(buf);
	return ret;
}

static int tcm_download_hdl_cfg(struct marie_tcm *ts, u8 area_type,
				const u8 *data, size_t size)
{
	u8 *buf;
	int ret;

	buf = kmalloc(size + 2, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	buf[0] = 1;
	buf[1] = area_type;
	memcpy(&buf[2], data, size);
	ret = tcm_write_cmd(ts, TCM_CMD_DOWNLOAD_CONFIG, buf, size + 2, 256);
	kfree(buf);
	return ret;
}

static int tcm_get_bits(const u8 *buf, u32 buf_len, u32 bit_off, u32 bits, u32 *out)
{
	u32 byte_off = bit_off / 8;
	u32 shift = bit_off % 8;
	u32 val = 0;
	u32 mask;

	if (bits == 0 || bits > 32)
		return -EINVAL;
	if (byte_off >= buf_len)
		return -EINVAL;

	if (byte_off < buf_len)
		val |= buf[byte_off];
	if (byte_off + 1 < buf_len)
		val |= (u32)buf[byte_off + 1] << 8;
	if (byte_off + 2 < buf_len)
		val |= (u32)buf[byte_off + 2] << 16;
	if (byte_off + 3 < buf_len)
		val |= (u32)buf[byte_off + 3] << 24;

	val >>= shift;
	mask = (bits == 32) ? 0xffffffffU : ((1U << bits) - 1U);
	*out = val & mask;
	return 0;
}

static void tcm_parse_and_report(struct marie_tcm *ts, const u8 *payload, u32 plen)
{
	struct tcm_obj_data objs[TCM_MAX_OBJECTS];
	const u8 *cfg = ts->report_cfg;
	u32 cfg_len = ts->report_cfg_len;
	u32 idx = 0, bit_off = 0, bits = 0, data = 0;
	u32 obj = 0, objects = 0, active_objs = 0, next_loop = 0, end_of_foreach = 0;
	bool active_only = false, has_active_cnt = false;
	u32 max_objs = min_t(u32, ts->max_objects ? ts->max_objects : TCM_MAX_OBJECTS,
			     TCM_MAX_OBJECTS);

	/* Pre-find end_of_foreach for zero-active-object fast skip */
	for (idx = 0; idx < cfg_len; idx++) {
		u8 c = cfg[idx];

		if (c == TOUCH_END)
			break;
		if (c == TOUCH_FOREACH_END) {
			end_of_foreach = idx + 1;
			break;
		}
		if (c == TOUCH_FOREACH_ACTIVE_OBJECT || c == TOUCH_FOREACH_OBJECT ||
		    c == TOUCH_PAD_TO_NEXT_BYTE)
			continue;
		if (c == TOUCH_ROI_DATA)
			idx += 2;
		else
			idx += 1;
	}
	idx = 0;

	memset(objs, 0, sizeof(objs));

	while (idx < cfg_len) {
		u8 code = cfg[idx++];

		switch (code) {
		case TOUCH_END:
			goto done_parse;
		case TOUCH_FOREACH_ACTIVE_OBJECT:
			obj = 0;
			next_loop = idx;
			active_only = true;
			break;
		case TOUCH_FOREACH_OBJECT:
			obj = 0;
			next_loop = idx;
			active_only = false;
			break;
		case TOUCH_FOREACH_END:
			end_of_foreach = idx;
			if (active_only) {
				if (has_active_cnt) {
					objects++;
					if (objects < active_objs)
						idx = next_loop;
				} else if (bit_off < plen * 8) {
					idx = next_loop;
				}
			} else {
				obj++;
				if (obj < max_objs)
					idx = next_loop;
			}
			break;
		case TOUCH_PAD_TO_NEXT_BYTE:
			bit_off = ALIGN(bit_off, 8);
			break;
		case TOUCH_OBJECT_N_INDEX:
			if (idx >= cfg_len)
				goto done_parse;
			bits = cfg[idx++];
			if (tcm_get_bits(payload, plen, bit_off, bits, &data))
				goto done_parse;
			obj = data;
			bit_off += bits;
			break;
		case TOUCH_OBJECT_N_CLASSIFICATION:
			if (idx >= cfg_len)
				goto done_parse;
			bits = cfg[idx++];
			if (tcm_get_bits(payload, plen, bit_off, bits, &data))
				goto done_parse;
			if (obj < TCM_MAX_OBJECTS)
				objs[obj].status = data;
			bit_off += bits;
			break;
		case TOUCH_OBJECT_N_X_POSITION:
			if (idx >= cfg_len)
				goto done_parse;
			bits = cfg[idx++];
			if (tcm_get_bits(payload, plen, bit_off, bits, &data))
				goto done_parse;
			if (obj < TCM_MAX_OBJECTS)
				objs[obj].x = data;
			bit_off += bits;
			break;
		case TOUCH_OBJECT_N_Y_POSITION:
			if (idx >= cfg_len)
				goto done_parse;
			bits = cfg[idx++];
			if (tcm_get_bits(payload, plen, bit_off, bits, &data))
				goto done_parse;
			if (obj < TCM_MAX_OBJECTS)
				objs[obj].y = data;
			bit_off += bits;
			break;
		case TOUCH_OBJECT_N_Z:
			if (idx >= cfg_len)
				goto done_parse;
			bits = cfg[idx++];
			if (tcm_get_bits(payload, plen, bit_off, bits, &data))
				goto done_parse;
			if (obj < TCM_MAX_OBJECTS)
				objs[obj].z = data;
			bit_off += bits;
			break;
		case TOUCH_OBJECT_N_X_WIDTH:
			if (idx >= cfg_len)
				goto done_parse;
			bits = cfg[idx++];
			if (tcm_get_bits(payload, plen, bit_off, bits, &data))
				goto done_parse;
			if (obj < TCM_MAX_OBJECTS)
				objs[obj].wx = data;
			bit_off += bits;
			break;
		case TOUCH_OBJECT_N_Y_WIDTH:
			if (idx >= cfg_len)
				goto done_parse;
			bits = cfg[idx++];
			if (tcm_get_bits(payload, plen, bit_off, bits, &data))
				goto done_parse;
			if (obj < TCM_MAX_OBJECTS)
				objs[obj].wy = data;
			bit_off += bits;
			break;
		case TOUCH_NUM_OF_ACTIVE_OBJECTS:
			if (idx >= cfg_len)
				goto done_parse;
			bits = cfg[idx++];
			if (tcm_get_bits(payload, plen, bit_off, bits, &data))
				goto done_parse;
			active_objs = data;
			has_active_cnt = true;
			bit_off += bits;
			if (active_objs == 0 && end_of_foreach > idx)
				idx = end_of_foreach;
			break;
		case TOUCH_ROI_DATA:
			if (idx + 1 >= cfg_len)
				goto done_parse;
			bits = (u32)cfg[idx] | ((u32)cfg[idx + 1] << 8);
			idx += 2;
			bit_off += bits;
			break;
		default:
			/* Any other field has a 1-byte bit-length following its code */
			if (idx >= cfg_len)
				goto done_parse;
			bits = cfg[idx++];
			bit_off += bits;
			break;
		}
	}

done_parse:
	for (idx = 0; idx < max_objs; idx++) {
		bool down = (objs[idx].status == 1 || objs[idx].status == 2);

		if (down) {
			u32 x = min(objs[idx].x, ts->max_x);
			u32 y = min(objs[idx].y, ts->max_y);
			u32 major = max(objs[idx].wx, objs[idx].wy);

			ts->touch_events++;

			input_mt_slot(ts->input, idx);
			input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
			input_report_abs(ts->input, ABS_MT_POSITION_X, x);
			input_report_abs(ts->input, ABS_MT_POSITION_Y, y);
			input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, major ? major : 8);
			input_report_abs(ts->input, ABS_MT_PRESSURE, objs[idx].z ? objs[idx].z : 50);
			ts->prev_status[idx] = objs[idx].status;
		} else if (ts->prev_status[idx]) {
			input_mt_slot(ts->input, idx);
			input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
			ts->prev_status[idx] = 0;
		}
	}

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static irqreturn_t marie_tcm_irq_thread(int irq, void *data)
{
	struct marie_tcm *ts = data;
	u32 plen;
	int ret;

	mutex_lock(&ts->io_lock);
	/* Exact 4.9 syna_tcm_read_one_package(): single 256-byte SPI read */
	ret = tcm_spi_read(ts, ts->irq_pkt, 256);
	if (ret || ts->irq_pkt[0] != TCM_MARKER) {
		if (ts->bad_irqs++ < 5)
			pr_info("MARIE_TOUCH: irq pkt ret=%d %02x %02x %02x %02x\n",
				ret, ts->irq_pkt[0], ts->irq_pkt[1],
				ts->irq_pkt[2], ts->irq_pkt[3]);
		usleep_range(1000, 2000);
		goto out;
	}

	if (ts->irq_pkt[1] != TCM_REPORT_TOUCH)
		goto out;

	plen = tcm_le2(&ts->irq_pkt[2]);
	if (plen == 0)
		goto out;
	if (plen > 256 - TCM_HDR_SIZE)
		plen = 256 - TCM_HDR_SIZE;

	tcm_parse_and_report(ts, &ts->irq_pkt[TCM_HDR_SIZE], plen);

out:
	mutex_unlock(&ts->io_lock);
	return IRQ_HANDLED;
}

static void marie_tcm_resolve_fw_name(struct marie_tcm *ts)
{
	struct device_node *lcd_panel, *lcd_cfg, *child;
	const char *panel_type = NULL;
	const char *proj_id = NULL;
	char short_id[16] = "MARI69110";

	lcd_panel = of_find_node_by_path("/huawei,lcd_panel");
	if (lcd_panel) {
		of_property_read_string(lcd_panel, "lcd_panel_type", &panel_type);
		of_node_put(lcd_panel);
	}
	lcd_cfg = of_find_node_by_path("/huawei,lcd_config");
	if (lcd_cfg && panel_type) {
		for_each_child_of_node(lcd_cfg, child) {
			if (of_device_is_compatible(child, panel_type)) {
				if (!of_property_read_string(child,
						"hw,lcdkit-default-project-id",
						&proj_id) && proj_id &&
				    strlen(proj_id) >= 9) {
					memcpy(short_id, proj_id, 9);
					short_id[9] = '\0';
				}
				of_node_put(child);
				break;
			}
		}
	}
	if (lcd_cfg)
		of_node_put(lcd_cfg);

	snprintf(ts->fw_name, sizeof(ts->fw_name), "ts/marie_synaptics_tcm_%s.img", short_id);
	pr_info("MARIE_TOUCH: panel=%s -> fw=%s\n", panel_type ? panel_type : "?", ts->fw_name);
}

static int marie_tcm_zeroflash_boot(struct marie_tcm *ts, const struct firmware *fw)
{
	const struct tcm_img_hdr *hdr;
	const u8 *app_fw = NULL, *app_cfg = NULL, *disp_cfg = NULL;
	u32 app_fw_sz = 0, app_cfg_sz = 0, disp_cfg_sz = 0;
	u32 num_areas, i;
	struct tcm_pdt_entry pdt;
	u8 fn_num = 0, f35_data = 0, cmd_val;
	u8 buf[140];
	int ret, tries;

	if (fw->size < sizeof(*hdr))
		return -EINVAL;
	hdr = (const struct tcm_img_hdr *)fw->data;
	if (tcm_le4(hdr->magic) != TCM_IMG_MAGIC) {
		pr_err("MARIE_TOUCH: bad fw magic 0x%08x\n", tcm_le4(hdr->magic));
		return -EINVAL;
	}
	num_areas = tcm_le4(hdr->num_areas);
	for (i = 0; i < num_areas; i++) {
		u32 off = sizeof(*hdr) + i * 4;
		u32 addr, len, csum;
		const struct tcm_area_desc *ad;
		const u8 *content;

		if (off + 4 > fw->size)
			return -EINVAL;
		addr = tcm_le4(fw->data + off);
		if (addr + sizeof(*ad) > fw->size)
			return -EINVAL;
		ad = (const struct tcm_area_desc *)(fw->data + addr);
		if (tcm_le4(ad->magic) != TCM_AREA_MAGIC)
			continue;
		len = tcm_le4(ad->length);
		csum = tcm_le4(ad->checksum);
		content = fw->data + addr + sizeof(*ad);
		if (addr + sizeof(*ad) + len > fw->size)
			return -EINVAL;
		if ((crc32(~0U, content, len) ^ ~0U) != csum) {
			pr_err("MARIE_TOUCH: area %.16s crc mismatch\n", ad->id_str);
			return -EINVAL;
		}
		if (!strncmp((const char *)ad->id_str, "F35_APP_CODE", 12)) {
			app_fw = content;
			app_fw_sz = len;
		} else if (!strncmp((const char *)ad->id_str, "APP_CONFIG", 10)) {
			app_cfg = content;
			app_cfg_sz = len;
		} else if (!strncmp((const char *)ad->id_str, "DISPLAY", 7)) {
			disp_cfg = content;
			disp_cfg_sz = len;
		}
	}

	pr_info("MARIE_TOUCH: parsed fw: app_fw=%u app_cfg=%u disp_cfg=%u\n",
		app_fw_sz, app_cfg_sz, disp_cfg_sz);
	if (!app_fw || !app_cfg)
		return -EINVAL;

	/* Verify & ensure SPI1 pinmux (GPIO7/8/9 = ALT1 SPI1, GPIO10 = GPIO CS) */
	{
		void __iomem *pmx = ioremap(0xe896c000, 0x1000);
		if (pmx) {
			pr_info("MARIE_TOUCH: pinmux before: g7=0x%x g8=0x%x g9=0x%x g10=0x%x\n",
				readl(pmx + 0x708), readl(pmx + 0x70c),
				readl(pmx + 0x710), readl(pmx + 0x714));
			writel(0x1, pmx + 0x708);
			writel(0x1, pmx + 0x70c);
			writel(0x1, pmx + 0x710);
			writel(0x0, pmx + 0x714);
			dsb(sy);
			iounmap(pmx);
		}
	}

	/* Check F$35 microbootloader at 100 kHz (exact 4.9 syna_tcm_comm_check) */
	memset(&pdt, 0, sizeof(pdt));
	ret = tcm_rmi_read(ts, TCM_PDT_START_ADDR, (u8 *)&pdt, sizeof(pdt));
	pr_info("MARIE_TOUCH: PDT_START(100kHz) ret=%d: %02x %02x %02x %02x %02x %02x\n",
		ret, ((u8 *)&pdt)[0], ((u8 *)&pdt)[1], ((u8 *)&pdt)[2],
		((u8 *)&pdt)[3], ((u8 *)&pdt)[4], ((u8 *)&pdt)[5]);
	fn_num = pdt.fn_number;
	if (ret || fn_num != TCM_UBL_FN_NUMBER) {
		/* Try pulsing TP reset once in case chip isn't in uboot */
		gpiod_set_raw_value_cansleep(ts->reset_gpiod, 1);
		mdelay(1);
		gpiod_set_raw_value_cansleep(ts->reset_gpiod, 0);
		udelay(300);
		gpiod_set_raw_value_cansleep(ts->reset_gpiod, 1);
		mdelay(21);
		memset(&pdt, 0, sizeof(pdt));
		ret = tcm_rmi_read(ts, TCM_PDT_START_ADDR, (u8 *)&pdt, sizeof(pdt));
		pr_info("MARIE_TOUCH: PDT_START(post-rst) ret=%d: %02x %02x %02x %02x %02x %02x\n",
			ret, ((u8 *)&pdt)[0], ((u8 *)&pdt)[1], ((u8 *)&pdt)[2],
			((u8 *)&pdt)[3], ((u8 *)&pdt)[4], ((u8 *)&pdt)[5]);
		fn_num = pdt.fn_number;
	}
	if (ret || fn_num != TCM_UBL_FN_NUMBER)
		return -ENODEV;
	tcm_rmi_read(ts, pdt.data_base, &f35_data, 1);
	pr_info("MARIE_TOUCH: F$35 ctrl=0x%02x data=0x%02x err=0x%02x\n",
		pdt.ctrl_base, pdt.data_base, f35_data & 0x1f);

	/* Hardware TP reset pulse right before streaming F35_APP_CODE */
	gpiod_set_raw_value_cansleep(ts->reset_gpiod, 1);
	mdelay(1);
	gpiod_set_raw_value_cansleep(ts->reset_gpiod, 0);
	udelay(300);
	gpiod_set_raw_value_cansleep(ts->reset_gpiod, 1);
	mdelay(21);

	cmd_val = TCM_F35_WRITE_FW_TO_PMEM;
	ret = tcm_rmi_write(ts, pdt.ctrl_base + TCM_F35_CTRL3_OFFSET, &cmd_val, 1);
	if (ret) {
		pr_err("MARIE_TOUCH: F35 ctrl3 write failed: %d\n", ret);
		return ret;
	}
	ret = tcm_rmi_write(ts, pdt.ctrl_base + TCM_F35_CTRL7_OFFSET, app_fw, app_fw_sz);
	if (ret) {
		pr_err("MARIE_TOUCH: F35 app_fw write failed: %d\n", ret);
		return ret;
	}

	msleep(25);
	memset(buf, 0, 30);
	tcm_spi_read(ts, buf, 30);
	pr_info("MARIE_TOUCH: post-fw id: %02x %02x %02x %02x %02x %02x %02x %02x\n",
		buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
	if (buf[0] != TCM_MARKER) {
		u8 f35_after = 0xff;
		tcm_rmi_read(ts, pdt.data_base, &f35_after, 1);
		pr_info("MARIE_TOUCH: post-fw f35_data=0x%02x (err=0x%02x)\n",
			f35_after, f35_after & 0x1f);
	}

	/* Download display & app configs per REPORT_STATUS */
	usleep_range(2000, 3000);
	for (tries = 0; tries < 6; tries++) {
		u16 fw_st;
		int r;

		usleep_range(2000, 3000);
		for (r = 0; r < 3; r++) {
			memset(buf, 0, 10);
			ret = tcm_spi_read(ts, buf, 10);
			if (!ret && buf[0] == TCM_MARKER &&
			    (buf[1] == TCM_REPORT_STATUS || buf[1] == TCM_STATUS_OK))
				break;
			msleep(10);
		}
		fw_st = tcm_le2(&buf[4]);
		pr_info("MARIE_TOUCH: status[%d]: %02x %02x fw_st=0x%04x\n",
			tries, buf[0], buf[1], fw_st);
		if (!(fw_st & 0x06))
			break;
		if ((fw_st & BIT(1)) && disp_cfg) {
			ret = tcm_download_hdl_cfg(ts, TCM_HDL_DISP_CFG_PMEM,
						   disp_cfg, disp_cfg_sz);
			pr_info("MARIE_TOUCH: disp_cfg dl=%d\n", ret);
		} else if ((fw_st & BIT(2)) && app_cfg) {
			ret = tcm_download_hdl_cfg(ts, TCM_HDL_TOUCH_CFG_PMEM,
						   app_cfg, app_cfg_sz);
			pr_info("MARIE_TOUCH: app_cfg dl=%d\n", ret);
		}
	}

	/* Disable report temporarily, then IDENTIFY + APP_INFO + TOUCH_CFG + ENABLE */
	{
		u8 rep = TCM_REPORT_TOUCH;

		tcm_write_cmd(ts, TCM_CMD_DISABLE_REPORT, &rep, 1, 256);
		msleep(50);
		tcm_spi_read(ts, buf, 16);
	}

	ret = tcm_write_cmd(ts, TCM_CMD_IDENTIFY, NULL, 0, 256);
	if (ret)
		return ret;
	udelay(1000);
	memset(buf, 0, 30);
	tcm_spi_read(ts, buf, 30);
	pr_info("MARIE_TOUCH: IDENTIFY: %02x %02x mode=0x%02x part=%.16s\n",
		buf[0], buf[1], buf[5], &buf[6]);

	msleep(300);
	for (tries = 0; tries < 3; tries++) {
		msleep(100);
		tcm_write_cmd(ts, TCM_CMD_GET_APP_INFO, NULL, 0, 256);
		udelay(1000);
		memset(buf, 0, 50);
		tcm_spi_read(ts, buf, 50);
		if (buf[0] == TCM_MARKER && buf[1] == TCM_STATUS_OK)
			break;
	}
	if (buf[0] == TCM_MARKER && buf[1] == TCM_STATUS_OK) {
		/* struct syna_tcm_app_info starts at buf[4]; max_x@32, max_y@34, max_obj@36 */
		ts->max_x = tcm_le2(&buf[4 + 32]);
		ts->max_y = tcm_le2(&buf[4 + 34]);
		ts->max_objects = tcm_le2(&buf[4 + 36]);
	}
	if (!ts->max_x || ts->max_x > 4096)
		ts->max_x = 1080;
	if (!ts->max_y || ts->max_y > 4096)
		ts->max_y = 2312;
	if (!ts->max_objects || ts->max_objects > TCM_MAX_OBJECTS)
		ts->max_objects = TCM_MAX_OBJECTS;
	pr_info("MARIE_TOUCH: APP_INFO max_x=%u max_y=%u max_objects=%u\n",
		ts->max_x, ts->max_y, ts->max_objects);

	tcm_write_cmd(ts, TCM_CMD_GET_TOUCH_CFG, NULL, 0, 256);
	udelay(1000);
	memset(buf, 0, TCM_TOUCH_CFG_MAX + 4);
	tcm_spi_read(ts, buf, TCM_TOUCH_CFG_MAX + 4);
	pr_info("MARIE_TOUCH: TOUCH_CFG: %02x %02x len=%u\n",
		buf[0], buf[1], tcm_le2(&buf[2]));
	if (buf[0] != TCM_MARKER || buf[1] != TCM_STATUS_OK)
		return -EIO;
	ts->report_cfg_len = min_t(u32, tcm_le2(&buf[2]), TCM_TOUCH_CFG_MAX);
	memcpy(ts->report_cfg, &buf[4], ts->report_cfg_len);

	{
		u8 rep = TCM_REPORT_TOUCH;

		tcm_write_cmd(ts, TCM_CMD_ENABLE_REPORT, &rep, 1, 256);
		for (tries = 0; tries < 3; tries++) {
			msleep(100);
			memset(buf, 0, 8);
			tcm_spi_read(ts, buf, 4);
			if (buf[0] == TCM_MARKER && buf[1] == TCM_STATUS_OK)
				break;
		}
		pr_info("MARIE_TOUCH: ENABLE_REPORT: %02x %02x\n", buf[0], buf[1]);
	}

	return 0;
}

static void marie_tcm_init_work(struct work_struct *work)
{
	struct marie_tcm *ts =
		container_of(to_delayed_work(work), struct marie_tcm, init_work);
	struct spi_controller *ctlr;
	const struct firmware *fw = NULL;
	int ret;

	if (!ts->spi) {
		ctlr = marie_get_spi1_ctlr();
		if (!ctlr) {
			if (++ts->retries < 80)
				schedule_delayed_work(&ts->init_work, msecs_to_jiffies(500));
			else
				pr_err("MARIE_TOUCH: timed out waiting for spi1 controller\n");
			return;
		}
		struct spi_board_info info = {
			.modalias = "synaptics-tcm-marie",
			.max_speed_hz = TCM_SPI_SPEED_HZ,
			.chip_select = 0,
			.mode = SPI_MODE_3,
		};
		ts->spi = spi_new_device(ctlr, &info);
		if (!ts->spi) {
			pr_err("MARIE_TOUCH: spi_new_device failed\n");
			return;
		}
		pr_info("MARIE_TOUCH: attached to spi1 controller\n");
	}

	ret = firmware_request_nowarn(&fw, ts->fw_name, ts->dev);
	if (ret) {
		if (++ts->retries < 80)
			schedule_delayed_work(&ts->init_work, msecs_to_jiffies(500));
		else
			pr_err("MARIE_TOUCH: timed out waiting for %s (%d)\n", ts->fw_name, ret);
		return;
	}

	pr_info("MARIE_TOUCH: loaded %s (%zu bytes)\n", ts->fw_name, fw->size);
	mutex_lock(&ts->io_lock);
	ret = marie_tcm_zeroflash_boot(ts, fw);
	mutex_unlock(&ts->io_lock);
	release_firmware(fw);

	if (ret) {
		pr_err("MARIE_TOUCH: zeroflash boot failed: %d\n", ret);
		return;
	}

	ts->input = devm_input_allocate_device(ts->dev);
	if (!ts->input)
		return;
	ts->input->name = "synaptics_tcm";
	ts->input->phys = "spi1/input0";
	ts->input->id.bustype = BUS_SPI;
	__set_bit(EV_ABS, ts->input->evbit);
	__set_bit(EV_KEY, ts->input->evbit);
	__set_bit(BTN_TOUCH, ts->input->keybit);
	__set_bit(INPUT_PROP_DIRECT, ts->input->propbit);

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, ts->max_x, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, ts->max_y, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 255, 0, 0);

	ret = input_mt_init_slots(ts->input, ts->max_objects, INPUT_MT_DIRECT);
	if (ret) {
		pr_err("MARIE_TOUCH: input_mt_init_slots failed: %d\n", ret);
		return;
	}

	ret = input_register_device(ts->input);
	if (ret) {
		pr_err("MARIE_TOUCH: input_register_device failed: %d\n", ret);
		return;
	}

	ts->irq = gpiod_to_irq(ts->irq_gpiod);
	ret = devm_request_threaded_irq(ts->dev, ts->irq, NULL, marie_tcm_irq_thread,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					"synaptics_tcm", ts);
	if (ret) {
		pr_err("MARIE_TOUCH: request_threaded_irq(%d) failed: %d\n", ts->irq, ret);
		return;
	}

	pr_info("MARIE_TOUCH: Synaptics TD4320 touchscreen READY (%ux%u, irq=%d)\n",
		ts->max_x, ts->max_y, ts->irq);
}

static int marie_tcm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct marie_tcm *ts;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->dev = dev;
	mutex_init(&ts->io_lock);

	ts->cs_gpiod = marie_get_dt_gpiod(dev->of_node, "cs_gpio", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->cs_gpiod))
		return PTR_ERR(ts->cs_gpiod);

	ts->reset_gpiod = marie_get_dt_gpiod(dev->of_node, "reset_gpio", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpiod))
		return PTR_ERR(ts->reset_gpiod);

	ts->irq_gpiod = marie_get_dt_gpiod(dev->of_node, "irq_gpio", GPIOD_IN);
	if (IS_ERR(ts->irq_gpiod))
		return PTR_ERR(ts->irq_gpiod);

	marie_tcm_resolve_fw_name(ts);
	INIT_DELAYED_WORK(&ts->init_work, marie_tcm_init_work);
	schedule_delayed_work(&ts->init_work, msecs_to_jiffies(2000));

	pr_info("MARIE_TOUCH: ts_kit probed, waiting for spi1 + %s\n", ts->fw_name);
	return 0;
}

static const struct of_device_id marie_tcm_of_match[] = {
	{ .compatible = "huawei,ts_kit" },
	{ }
};

static struct platform_driver marie_tcm_driver = {
	.probe = marie_tcm_probe,
	.driver = {
		.name = "marie-synaptics-tcm",
		.of_match_table = marie_tcm_of_match,
	},
};

/* =========================================================================
 * 4. TI LM36274 Backlight Driver over I2C3 (0xfdf0c000, addr 0x11)
 * ========================================================================= */

#define LM36274_REG_BL_LSB	0x04
#define LM36274_REG_BL_MSB	0x05
#define LM36274_REG_BL_ENABLE	0x08

static struct i2c_client *marie_lm36274_client;
static DEFINE_MUTEX(marie_lm36274_lock);
static u8 marie_lm36274_en_val = 0x17;
static bool marie_lm36274_enabled = true;

/*
 * Inverse exponential curve matching stock Kirin 710 LM36274 exponential mode
 * (BL_CONFIG1 = 0x40). Maps Android linear brightness 0..255 -> 11-bit 0..2047.
 */
static const u16 marie_lm36274_lut[256] = {
	   0,  328,  507,  624,  709,  776,  832,  881,
	 923,  959,  992, 1022, 1051, 1076, 1100, 1122,
	1142, 1162, 1180, 1198, 1214, 1230, 1245, 1259,
	1273, 1286, 1299, 1311, 1323, 1335, 1345, 1356,
	1367, 1377, 1386, 1396, 1405, 1414, 1422, 1431,
	1439, 1447, 1455, 1463, 1470, 1478, 1485, 1492,
	1499, 1506, 1512, 1519, 1525, 1531, 1537, 1543,
	1549, 1555, 1561, 1566, 1572, 1577, 1582, 1588,
	1593, 1598, 1603, 1608, 1613, 1617, 1622, 1627,
	1631, 1636, 1640, 1645, 1649, 1653, 1658, 1662,
	1666, 1670, 1674, 1678, 1682, 1686, 1690, 1693,
	1697, 1701, 1704, 1708, 1712, 1715, 1719, 1722,
	1726, 1729, 1732, 1736, 1739, 1742, 1746, 1749,
	1752, 1755, 1758, 1761, 1764, 1767, 1770, 1773,
	1776, 1779, 1782, 1785, 1788, 1791, 1793, 1796,
	1799, 1802, 1804, 1807, 1810, 1812, 1815, 1817,
	1820, 1823, 1825, 1828, 1830, 1833, 1835, 1838,
	1840, 1842, 1845, 1847, 1849, 1852, 1854, 1856,
	1859, 1861, 1863, 1866, 1868, 1870, 1872, 1874,
	1877, 1879, 1881, 1883, 1885, 1887, 1889, 1891,
	1893, 1895, 1897, 1899, 1901, 1903, 1905, 1907,
	1909, 1911, 1913, 1915, 1917, 1919, 1921, 1923,
	1925, 1927, 1928, 1930, 1932, 1934, 1936, 1937,
	1939, 1941, 1943, 1945, 1946, 1948, 1950, 1952,
	1953, 1955, 1957, 1958, 1960, 1962, 1963, 1965,
	1967, 1968, 1970, 1972, 1973, 1975, 1976, 1978,
	1980, 1981, 1983, 1984, 1986, 1987, 1989, 1990,
	1992, 1993, 1995, 1996, 1998, 2000, 2001, 2002,
	2004, 2005, 2007, 2008, 2010, 2011, 2013, 2014,
	2015, 2017, 2018, 2020, 2021, 2022, 2024, 2025,
	2027, 2028, 2029, 2031, 2032, 2033, 2035, 2036,
	2037, 2039, 2040, 2041, 2043, 2044, 2045, 2047,
};

int marie_lm36274_set_brightness(unsigned int brightness)
{
	struct i2c_client *client;
	u32 level;
	u8 lsb, msb;
	int ret = 0;

	mutex_lock(&marie_lm36274_lock);
	client = marie_lm36274_client;
	if (!client) {
		mutex_unlock(&marie_lm36274_lock);
		return 0;
	}

	if (brightness == 0) {
		i2c_smbus_write_byte_data(client, LM36274_REG_BL_LSB, 0);
		i2c_smbus_write_byte_data(client, LM36274_REG_BL_MSB, 0);
		if (marie_lm36274_enabled) {
			ret = i2c_smbus_write_byte_data(client, LM36274_REG_BL_ENABLE, 0x00);
			marie_lm36274_enabled = false;
		}
	} else {
		if (brightness > 255)
			brightness = 255;
		level = marie_lm36274_lut[brightness];
		lsb = level & 0x07;
		msb = (level >> 3) & 0xff;
		if (!marie_lm36274_enabled) {
			i2c_smbus_write_byte_data(client, LM36274_REG_BL_ENABLE, marie_lm36274_en_val);
			marie_lm36274_enabled = true;
		}
		ret = i2c_smbus_write_byte_data(client, LM36274_REG_BL_LSB, lsb);
		if (!ret)
			ret = i2c_smbus_write_byte_data(client, LM36274_REG_BL_MSB, msb);
	}
	mutex_unlock(&marie_lm36274_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(marie_lm36274_set_brightness);

static int marie_lm36274_probe(struct i2c_client *client)
{
	int en, lsb, msb;

	en = i2c_smbus_read_byte_data(client, LM36274_REG_BL_ENABLE);
	lsb = i2c_smbus_read_byte_data(client, LM36274_REG_BL_LSB);
	msb = i2c_smbus_read_byte_data(client, LM36274_REG_BL_MSB);
	if (en < 0) {
		pr_warn("MARIE_BL: lm36274 probe read failed: %d\n", en);
		return en;
	}
	mutex_lock(&marie_lm36274_lock);
	if (en > 0)
		marie_lm36274_en_val = (u8)en;
	marie_lm36274_enabled = (en > 0);
	marie_lm36274_client = client;
	mutex_unlock(&marie_lm36274_lock);
	pr_info("MARIE_BL: LM36274 backlight READY (en=0x%02x lsb=0x%02x msb=0x%02x)\n",
		en, lsb, msb);
	return 0;
}

static const struct of_device_id marie_lm36274_of_match[] = {
	{ .compatible = "huawei,hw_lm36274" },
	{ .compatible = "huawei,hw_ktz8864" },
	{ }
};

static const struct i2c_device_id marie_lm36274_id[] = {
	{ "lm36274", 0 },
	{ }
};

static struct i2c_driver marie_lm36274_driver = {
	.driver = {
		.name = "marie-lm36274",
		.of_match_table = marie_lm36274_of_match,
	},
	.probe = marie_lm36274_probe,
	.id_table = marie_lm36274_id,
};

/* =========================================================================
 * 4b. HI6523 (SchargerV300) I2C6 Switching Charger IC (huawei,hi6523_charger)
 * ========================================================================= */

#define HI6523_REG_CHIP_VER0		0x00
#define HI6523_REG_CHIP_VER5		0x05
#define HI6523_REG_STATUS0		0x06
#define HI6523_REG_STATUS1		0x07
#define HI6523_REG_BUCK_REG0		0x60	/* bits [6:2] buck_ilimit */
#define HI6523_REG_BUCK_REG2		0x62	/* bits [4:2] buck_dpm_sel (VINDPM) */
#define HI6523_REG_CHG_REG0		0x90	/* bit 5 chg_en, bit 4 batfet_ctrl */
#define HI6523_REG_CHG_REG1		0x91	/* bits [4:0] chg_fast_ichg */
#define HI6523_REG_CHG_REG2		0x92	/* bits [6:3] chg_fast_vchg, [2:0] term_ichg */
#define HI6523_REG_CHG_REG3		0x93	/* bits [1:0] fastchg_timer */
#define HI6523_REG_CHG_REG4		0x94	/* bit 2 chg_en_term */
#define HI6523_REG_DET_TOP_REG0		0xa0	/* bits [1:0] vbus_vset (0=5V) */
#define HI6523_REG_SCHG_LOGIC_CTRL	0xa3	/* bit 0 buck_enb (HIZ) */
#define HI6523_REG_WDT_SOFT_RST		0xe8	/* bit 0 wd_rst_n */
#define HI6523_REG_WDT_CTRL		0xe9	/* bits [1:0] watchdog_timer */

#define HI6523_ILIMIT_900MA		3	/* 900 mA (PC USB with HW VINDPM) */
#define HI6523_ILIMIT_2000MA		14	/* 2000 mA (AC / USB-C wall charger) */
#define HI6523_ICHG_1000MA		9	/* 1000 mA */
#define HI6523_ICHG_2200MA		16	/* 2200 mA (full 5V/2A buck output) */
#define HI6523_VCHG_4400MV		12	/* (4400 - 3800) / 50 = 12 */
#define HI6523_ITERM_150MA		2	/* 150 mA termination (full 100% top-off) */

static DEFINE_MUTEX(marie_hi6523_lock);
static struct i2c_client *marie_hi6523_client;
static int marie_hi6523_last_mode = -1;	/* -1=unset, 0=off, 1=pc_usb, 2=ac */

static int hi6523_write_mask(struct i2c_client *client, u8 reg, u8 mask, u8 shift, u8 val)
{
	int cur = i2c_smbus_read_byte_data(client, reg);
	u8 next;

	if (cur < 0)
		return cur;
	next = ((u8)cur & ~mask) | ((val << shift) & mask);
	return i2c_smbus_write_byte_data(client, reg, next);
}

static bool marie_hi6523_is_charge_done(void)
{
	int st0;

	mutex_lock(&marie_hi6523_lock);
	if (!marie_hi6523_client) {
		mutex_unlock(&marie_hi6523_lock);
		return false;
	}
	st0 = i2c_smbus_read_byte_data(marie_hi6523_client, HI6523_REG_STATUS0);
	mutex_unlock(&marie_hi6523_lock);
	if (st0 < 0)
		return false;
	return ((st0 >> 4) & 0x03) == 0x03;
}

static void marie_hi6523_update(bool vbus_online, bool pc_usb)
{
	struct i2c_client *client;
	int mode = !vbus_online ? 0 : (pc_usb ? 1 : 2);
	u8 ilim, ichg;
	int st0, st1, r60, r90, r91, r92;

	mutex_lock(&marie_hi6523_lock);
	client = marie_hi6523_client;
	if (!client) {
		mutex_unlock(&marie_hi6523_lock);
		return;
	}

	if (!vbus_online) {
		marie_hi6523_last_mode = 0;
		mutex_unlock(&marie_hi6523_lock);
		return;
	}

	/* Always ensure WDT is disabled and kicked, HIZ off, 5V VBUS / 85% VINDPM, 4.40V VREG */
	ilim = pc_usb ? HI6523_ILIMIT_900MA : HI6523_ILIMIT_2000MA;
	ichg = pc_usb ? HI6523_ICHG_1000MA : HI6523_ICHG_2200MA;

	hi6523_write_mask(client, HI6523_REG_WDT_CTRL, 0x03, 0, 0x00);
	hi6523_write_mask(client, HI6523_REG_WDT_SOFT_RST, BIT(0), 0, 1);
	hi6523_write_mask(client, HI6523_REG_DET_TOP_REG0, 0x03, 0, 0x00); /* 5V VBUS */
	hi6523_write_mask(client, HI6523_REG_BUCK_REG2, 0x07 << 2, 2, 1);  /* 85% VINDPM (4.25V) */
	hi6523_write_mask(client, HI6523_REG_SCHG_LOGIC_CTRL, BIT(0), 0, 0);
	hi6523_write_mask(client, HI6523_REG_BUCK_REG0, 0x1f << 2, 2, ilim);
	hi6523_write_mask(client, HI6523_REG_CHG_REG1, 0x1f, 0, ichg);
	hi6523_write_mask(client, HI6523_REG_CHG_REG2, 0x0f << 3, 3, HI6523_VCHG_4400MV);
	hi6523_write_mask(client, HI6523_REG_CHG_REG2, 0x07, 0, HI6523_ITERM_150MA);
	hi6523_write_mask(client, HI6523_REG_CHG_REG3, 0x03, 0, 3);	/* 20h safety timer */
	hi6523_write_mask(client, HI6523_REG_CHG_REG4, BIT(2), 2, 1);	/* enable termination */
	hi6523_write_mask(client, HI6523_REG_CHG_REG0, BIT(4), 4, 1);	/* enable BATFET */
	hi6523_write_mask(client, HI6523_REG_CHG_REG0, BIT(5), 5, 1);	/* enable charging */

	if (mode != marie_hi6523_last_mode) {
		int r62;

		marie_hi6523_last_mode = mode;
		st0 = i2c_smbus_read_byte_data(client, HI6523_REG_STATUS0);
		st1 = i2c_smbus_read_byte_data(client, HI6523_REG_STATUS1);
		r60 = i2c_smbus_read_byte_data(client, HI6523_REG_BUCK_REG0);
		r62 = i2c_smbus_read_byte_data(client, HI6523_REG_BUCK_REG2);
		r90 = i2c_smbus_read_byte_data(client, HI6523_REG_CHG_REG0);
		r91 = i2c_smbus_read_byte_data(client, HI6523_REG_CHG_REG1);
		r92 = i2c_smbus_read_byte_data(client, HI6523_REG_CHG_REG2);
		pr_info("MARIE_CHG: HI6523 configured mode=%s (IIN=%dmA ICHG=%dmA VREG=4400mV) st0=0x%02x st1=0x%02x r60=0x%02x r62=0x%02x r90=0x%02x r91=0x%02x r92=0x%02x\n",
			pc_usb ? "USB" : "AC",
			pc_usb ? 900 : 2000, pc_usb ? 1000 : 2200,
			st0 & 0xff, st1 & 0xff, r60 & 0xff, r62 & 0xff,
			r90 & 0xff, r91 & 0xff, r92 & 0xff);
	}
	mutex_unlock(&marie_hi6523_lock);
}

static int marie_hi6523_probe(struct i2c_client *client)
{
	int v0, v5, st0, st1, r60, r90, r91, r92, wdt;

	v0 = i2c_smbus_read_byte_data(client, HI6523_REG_CHIP_VER0);
	if (v0 < 0) {
		pr_warn("MARIE_CHG: HI6523 I2C read failed: %d\n", v0);
		return v0;
	}
	v5 = i2c_smbus_read_byte_data(client, HI6523_REG_CHIP_VER5);
	st0 = i2c_smbus_read_byte_data(client, HI6523_REG_STATUS0);
	st1 = i2c_smbus_read_byte_data(client, HI6523_REG_STATUS1);
	r60 = i2c_smbus_read_byte_data(client, HI6523_REG_BUCK_REG0);
	r90 = i2c_smbus_read_byte_data(client, HI6523_REG_CHG_REG0);
	r91 = i2c_smbus_read_byte_data(client, HI6523_REG_CHG_REG1);
	r92 = i2c_smbus_read_byte_data(client, HI6523_REG_CHG_REG2);
	wdt = i2c_smbus_read_byte_data(client, HI6523_REG_WDT_CTRL);

	pr_info("MARIE_CHG: HI6523 READY (v0=0x%02x v5=0x%02x st0=0x%02x st1=0x%02x r60=0x%02x r90=0x%02x r91=0x%02x r92=0x%02x wdt=0x%02x)\n",
		v0 & 0xff, v5 & 0xff, st0 & 0xff, st1 & 0xff,
		r60 & 0xff, r90 & 0xff, r91 & 0xff, r92 & 0xff, wdt & 0xff);

	mutex_lock(&marie_hi6523_lock);
	marie_hi6523_client = client;
	marie_hi6523_last_mode = -1;
	mutex_unlock(&marie_hi6523_lock);

	marie_hi6523_update(true, kirin710_dwc3_is_configured());
	return 0;
}

static const struct of_device_id marie_hi6523_of_match[] = {
	{ .compatible = "huawei,hi6523_charger" },
	{ }
};

static const struct i2c_device_id marie_hi6523_id[] = {
	{ "hi6523_charger", 0 },
	{ }
};

static struct i2c_driver marie_hi6523_driver = {
	.driver = {
		.name = "marie-hi6523",
		.of_match_table = marie_hi6523_of_match,
	},
	.probe = marie_hi6523_probe,
	.id_table = marie_hi6523_id,
};

/* =========================================================================
 * 5. HI6555v200 SPMI Battery (Hardware Coulomb Counter) & USB Power Supply
 * ========================================================================= */

#define PMIC_STATUS0_REG	0x006
#define PMIC_STATUS1_REG	0x007
#define PMIC_HRST_REG6_REG	0x1da
#define PMIC_CURRENT_0_REG	0x2d2
#define PMIC_V_OUT_0_REG	0x2d4
#define PMIC_CL_OUT0_REG	0x2d8
#define PMIC_CL_IN0_REG		0x2dc
#define PMIC_CLJ_CTRL_REGS2_REG	0x331

#define MARIE_BAT_VBAT_MAX_UV	4400000
#define MARIE_BAT_FCC_UAH	3240000	/* Rated FCC from P30 lite DT (3240 mAh) */
#define MARIE_BAT_DESIGN_UAH	3340000	/* Typical design capacity (3340 mAh) */
#define MARIE_BAT_RBATT_MOHM	130	/* Internal resistance for OCV estimate */
#define MARIE_BAT_POLL_MS	5000
#define MARIE_BAT_SAMPLE_TICKS	1	/* 5s coulomb integration cadence */

struct marie_ocv_pt {
	int mv;
	int cap;
};

/* Factory 4.40V HV Li-Po OCV curve (25C) from P30 lite (marie) DT profiles */
static const struct marie_ocv_pt marie_ocv_table[] = {
	{ 4345, 100 }, { 4285,  95 }, { 4227,  90 }, { 4170,  85 },
	{ 4116,  80 }, { 4065,  75 }, { 4018,  70 }, { 3974,  65 },
	{ 3928,  60 }, { 3881,  55 }, { 3849,  50 }, { 3824,  45 },
	{ 3802,  40 }, { 3785,  35 }, { 3765,  30 }, { 3742,  25 },
	{ 3721,  20 }, { 3692,  15 }, { 3663,  10 }, { 3659,   9 },
	{ 3653,   8 }, { 3646,   7 }, { 3634,   6 }, { 3608,   5 },
	{ 3567,   4 }, { 3516,   3 }, { 3451,   2 }, { 3358,   1 },
	{ 3200,   0 },
};

/* Matches Huawei Kirin 710 stock struct ss_coul_nv_info (HICOUL NV #316) */
struct marie_coul_nv_info {
	s32 charge_cycles;	/* centi-cycles (cycles * 100) */
	s32 v_offset_a;		/* voltage gain (nominal 1000000) */
	s32 v_offset_b;		/* voltage offset in uV */
	s32 c_offset_a;		/* current gain (nominal 1000000) */
	s32 c_offset_b;		/* current offset in uA */
	s32 limit_fcc;		/* learned FCC limit in uAh */
	s16 temp[10];
	s16 real_fcc[10];
	s16 calc_ocv_reg_v;
	s16 calc_ocv_reg_c;
	s16 hkadc_batt_id_voltage;
	s16 real_fcc_record[5];
	s16 latest_record_index;
	s16 basp_delay_config;
	s32 fcc_check_sum_ext;
	s16 change_battery_learn_flag;
	s16 qmax;
	s32 report_full_fcc_real;
};

static unsigned long marie_pmu_nv_addr;

static int __init marie_parse_pmu_nv_addr(char *p)
{
	if (!p || kstrtoul(p, 16, &marie_pmu_nv_addr))
		marie_pmu_nv_addr = 0;
	return 0;
}
early_param("pmu_nv_addr", marie_parse_pmu_nv_addr);

struct marie_battery {
	struct spmi_device *sdev;
	struct power_supply *bat_psy;
	struct power_supply *usb_psy;
	struct power_supply *ac_psy;
	struct delayed_work poll_work;
	struct mutex lock;
	int vbat_uv;
	int i_out_ua;	/* >0 discharging, <0 charging (HI6555v200 convention) */
	int ocv_uv;
	s64 rem_uah;	/* Coulomb-counted remaining charge in uAh */
	s64 fcc_uah;	/* Learned/rated full charge capacity in uAh */
	s64 charged_acc_uah;	/* Runtime accumulated charge for cycle tracking */
	int cycle_count;
	int v_offset_a;
	int v_offset_b;
	int c_offset_a;
	int c_offset_b;
	u32 cl_out_prev;
	u32 cl_in_prev;
	int cap;
	int status;
	bool usb_online;
	bool pc_usb;
	bool ready;
	unsigned int poll_ticks;
	ktime_t last_sample_kt;
};

static struct marie_battery marie_bat;

static void marie_bat_load_hicoul_nv(struct marie_battery *mb)
{
	void __iomem *nv_io;
	struct marie_coul_nv_info nv;

	mb->fcc_uah = MARIE_BAT_FCC_UAH;
	mb->cycle_count = 0;
	mb->v_offset_a = 1000000;
	mb->v_offset_b = 0;
	mb->c_offset_a = 1000000;
	mb->c_offset_b = 0;

	if (!marie_pmu_nv_addr)
		return;

	nv_io = ioremap_wc(marie_pmu_nv_addr, sizeof(nv));
	if (!nv_io) {
		pr_warn("MARIE_BAT: ioremap_wc(pmu_nv_addr=0x%lx) failed\n",
			marie_pmu_nv_addr);
		return;
	}
	memcpy_fromio(&nv, nv_io, sizeof(nv));
	iounmap(nv_io);

	if (nv.v_offset_a >= 900000 && nv.v_offset_a <= 1100000)
		mb->v_offset_a = nv.v_offset_a;
	if (nv.v_offset_b >= -100000 && nv.v_offset_b <= 100000)
		mb->v_offset_b = nv.v_offset_b;
	if (nv.c_offset_a >= 900000 && nv.c_offset_a <= 1100000)
		mb->c_offset_a = nv.c_offset_a;
	if (nv.c_offset_b >= -100000 && nv.c_offset_b <= 100000)
		mb->c_offset_b = nv.c_offset_b;

	if (nv.report_full_fcc_real >= 1500000 && nv.report_full_fcc_real <= 3600000)
		mb->fcc_uah = nv.report_full_fcc_real;
	else if (nv.limit_fcc >= 1500000 && nv.limit_fcc <= 3600000)
		mb->fcc_uah = nv.limit_fcc;

	if (nv.charge_cycles >= 0 && nv.charge_cycles < 1000000)
		mb->cycle_count = nv.charge_cycles / 100;

	pr_info("MARIE_BAT: HICOUL nv@0x%lx cycles=%d (%d.%02d) fcc=%lld uAh (limit=%d full=%d qmax=%d) v_off=(%d,%d) c_off=(%d,%d)\n",
		marie_pmu_nv_addr, mb->cycle_count,
		nv.charge_cycles / 100, abs(nv.charge_cycles % 100),
		mb->fcc_uah, nv.limit_fcc, nv.report_full_fcc_real, nv.qmax,
		mb->v_offset_a, mb->v_offset_b, mb->c_offset_a, mb->c_offset_b);
}

static int marie_bat_read_u32(struct spmi_device *sdev, u16 reg, u32 *out)
{
	u8 b[4] = { 0 };
	int i;

	if (!sdev)
		return -ENODEV;
	for (i = 0; i < 4; i++) {
		if (spmi_ext_register_readl(sdev, reg + i, &b[i], 1))
			return -EIO;
	}
	*out = (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
	return 0;
}

static int marie_bat_read_uv(struct spmi_device *sdev)
{
	u8 lo = 0, hi = 0;
	s16 raw;
	s64 uv;

	if (!sdev)
		return 3850000;
	if (spmi_ext_register_readl(sdev, PMIC_V_OUT_0_REG, &lo, 1) ||
	    spmi_ext_register_readl(sdev, PMIC_V_OUT_0_REG + 1, &hi, 1))
		return 3850000;

	/* HI6555v200: 15-bit little-endian ADC, 1 LSB = 2400000 / 10661 uV */
	raw = (s16)(((u16)hi << 8) | lo) & 0x7fff;
	uv = div_s64((s64)raw * 2400000LL, 10661);
	if (marie_bat.v_offset_a)
		uv = div_s64(uv * marie_bat.v_offset_a, 1000000LL) + marie_bat.v_offset_b;
	if (uv < 2800000 || uv > 4600000)
		return 3850000;
	return (int)uv;
}

static int marie_bat_read_iout_ua(struct spmi_device *sdev)
{
	u8 lo = 0, hi = 0;
	s16 raw;
	s64 ua;

	if (!sdev)
		return 0;
	if (spmi_ext_register_readl(sdev, PMIC_CURRENT_0_REG, &lo, 1) ||
	    spmi_ext_register_readl(sdev, PMIC_CURRENT_0_REG + 1, &hi, 1))
		return 0;

	/*
	 * HI6555v200: 16-bit signed little-endian current ADC (r_coul_mohm=10).
	 * Positive = discharging (OUT of battery), Negative = charging (INTO battery).
	 * 1 LSB = 5,000,000 / 10661 uA (~469 uA).
	 */
	raw = (s16)(((u16)hi << 8) | lo);
	ua = div_s64((s64)raw * 5000000LL, 10661);
	if (marie_bat.c_offset_a)
		ua = div_s64(ua * marie_bat.c_offset_a, 1000000LL) + marie_bat.c_offset_b;
	if (ua < -6000000 || ua > 6000000)
		return 0;
	return (int)ua;
}

static u64 marie_bat_coul_bit_uah(struct spmi_device *sdev)
{
	u8 st1 = 0;

	if (sdev && !spmi_ext_register_readl(sdev, PMIC_STATUS1_REG, &st1, 1) &&
	    (st1 & BIT(2)))
		return 143127ULL;	/* 32K clock mode */
	return 143153ULL;		/* DCXO 586 mode */
}

static int marie_bat_ocv_to_cap(int ocv_uv)
{
	int mv = ocv_uv / 1000;
	size_t i;

	if (mv >= marie_ocv_table[0].mv)
		return 100;
	if (mv <= marie_ocv_table[ARRAY_SIZE(marie_ocv_table) - 1].mv)
		return 0;

	for (i = 0; i < ARRAY_SIZE(marie_ocv_table) - 1; i++) {
		int hi_mv = marie_ocv_table[i].mv;
		int lo_mv = marie_ocv_table[i + 1].mv;

		if (mv >= lo_mv) {
			int hi_cap = marie_ocv_table[i].cap;
			int lo_cap = marie_ocv_table[i + 1].cap;
			int span = hi_mv - lo_mv;

			if (span <= 0)
				return hi_cap;
			return lo_cap + ((mv - lo_mv) * (hi_cap - lo_cap) + span / 2) / span;
		}
	}
	return 0;
}

static bool marie_usb_is_online(struct spmi_device *sdev)
{
	u8 st = 0;

	if (!sdev)
		return false;
	if (spmi_ext_register_readl(sdev, PMIC_STATUS0_REG, &st, 1))
		return false;
	/* DT: hisilicon,hisi-pmic-vbus = <0x06 0x01> -> reg 0x006 BIT(1) */
	return !!(st & BIT(1));
}

static void marie_bat_save_soc(struct spmi_device *sdev, int cap)
{
	u8 val = (u8)(0x80 | clamp(cap, 0, 100));

	if (sdev)
		spmi_ext_register_writel(sdev, PMIC_HRST_REG6_REG, &val, 1);
}

static int marie_bat_calc_status(bool usb_online, int cap, int vbat_uv, int i_out_ua)
{
	if (!usb_online)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	if (cap >= 100 ||
	    (cap >= 99 && (marie_hi6523_is_charge_done() ||
			   (vbat_uv >= 4375000 && i_out_ua > -200000))))
		return POWER_SUPPLY_STATUS_FULL;
	return POWER_SUPPLY_STATUS_CHARGING;
}

static void marie_bat_update_cap_from_rem(struct marie_battery *mb)
{
	const s64 fcc = mb->fcc_uah > 0 ? mb->fcc_uah : MARIE_BAT_FCC_UAH;
	const s64 one_pct = div_s64(fcc, 100);
	const s64 hyst = div_s64(fcc, 300);	/* ~0.33% deadband (~11 mAh) */

	mb->rem_uah = clamp_t(s64, mb->rem_uah, 0, fcc);

	if (mb->rem_uah >= fcc) {
		mb->cap = 100;
		return;
	}
	if (mb->rem_uah <= 0) {
		mb->cap = 0;
		return;
	}

	while (mb->cap < 100 &&
	       mb->rem_uah >= (s64)(mb->cap + 1) * one_pct + hyst)
		mb->cap++;

	while (mb->cap > 0 &&
	       mb->rem_uah <= (s64)mb->cap * one_pct - hyst)
		mb->cap--;
}

static void marie_bat_sample_locked(struct marie_battery *mb, bool init)
{
	int vbat_uv, i_out_ua, ir_uv, ocv_uv, ocv_cap;
	u32 cl_out = 0, cl_in = 0;
	ktime_t now_kt = ktime_get_boottime();
	int prev_cap = mb->cap;

	vbat_uv = marie_bat_read_uv(mb->sdev);
	i_out_ua = marie_bat_read_iout_ua(mb->sdev);

	ir_uv = clamp_t(int, (int)div_s64((s64)i_out_ua * MARIE_BAT_RBATT_MOHM, 1000),
			-300000, 300000);
	ocv_uv = clamp(vbat_uv + ir_uv, 3000000, 4450000);
	ocv_cap = marie_bat_ocv_to_cap(ocv_uv);

	mb->vbat_uv = vbat_uv;
	mb->i_out_ua = i_out_ua;
	mb->ocv_uv = ocv_uv;

	if (init) {
		u8 hrst6 = 0;
		int init_cap = ocv_cap;

		if (!spmi_ext_register_readl(mb->sdev, PMIC_HRST_REG6_REG, &hrst6, 1) &&
		    (hrst6 & 0x80)) {
			int saved_cap = hrst6 & 0x7f;

			if (saved_cap <= 100 && abs(saved_cap - ocv_cap) <= 15)
				init_cap = saved_cap;
		}

		mb->cap = init_cap;
		/* Center inside the percentage bucket */
		{
			s64 fcc = mb->fcc_uah > 0 ? mb->fcc_uah : MARIE_BAT_FCC_UAH;

			mb->rem_uah = clamp_t(s64,
					      div_s64((s64)init_cap * fcc, 100) +
					      div_s64(fcc, 200),
					      0, fcc);
		}
		marie_bat_read_u32(mb->sdev, PMIC_CL_OUT0_REG, &mb->cl_out_prev);
		marie_bat_read_u32(mb->sdev, PMIC_CL_IN0_REG, &mb->cl_in_prev);
		mb->last_sample_kt = now_kt;
		marie_bat_save_soc(mb->sdev, mb->cap);
	} else {
		s64 dt_ms = ktime_ms_delta(now_kt, mb->last_sample_kt);
		s64 delta_uah = 0;
		bool hw_coul_ok = false;

		mb->last_sample_kt = now_kt;
		if (dt_ms < 0 || dt_ms > 3600000)
			dt_ms = MARIE_BAT_SAMPLE_TICKS * MARIE_BAT_POLL_MS;

		if (!marie_bat_read_u32(mb->sdev, PMIC_CL_OUT0_REG, &cl_out) &&
		    !marie_bat_read_u32(mb->sdev, PMIC_CL_IN0_REG, &cl_in)) {
			u32 d_out = cl_out - mb->cl_out_prev;
			u32 d_in = cl_in - mb->cl_in_prev;

			mb->cl_out_prev = cl_out;
			mb->cl_in_prev = cl_in;

			if (d_out != 0 || d_in != 0) {
				u64 bit_uah = marie_bat_coul_bit_uah(mb->sdev);
				s64 out_uah = div_s64((s64)d_out * bit_uah, 10000000LL);
				s64 in_uah = div_s64((s64)d_in * bit_uah, 10000000LL);

				/* Sanity bound: max 6A over dt_ms */
				s64 max_uah = div_s64(6000000LL * dt_ms, 3600000LL) + 5000;

				if (out_uah <= max_uah && in_uah <= max_uah) {
					delta_uah = in_uah - out_uah;
					hw_coul_ok = true;
				}
			}
		}

		/* Fallback to software current integration if HW counter didn't tick */
		if (!hw_coul_ok && dt_ms > 0)
			delta_uah = div_s64((s64)(-i_out_ua) * dt_ms, 3600000LL);

		mb->rem_uah += delta_uah;
		if (delta_uah > 0) {
			s64 fcc = mb->fcc_uah > 0 ? mb->fcc_uah : MARIE_BAT_FCC_UAH;

			mb->charged_acc_uah += delta_uah;
			while (mb->charged_acc_uah >= fcc) {
				mb->charged_acc_uah -= fcc;
				mb->cycle_count++;
			}
		}

		/* End-of-charge (HI6523 CHG_DONE or CV taper near 4.40V) -> finish to 100% */
		if (mb->usb_online &&
		    (marie_hi6523_is_charge_done() ||
		     (vbat_uv >= 4370000 && i_out_ua < 0 && i_out_ua > -180000))) {
			mb->rem_uah += 1500;
		} else if (!mb->usb_online && vbat_uv <= 3250000) {
			/* Low-voltage cutoff protection -> reach 0% before UVLO */
			mb->rem_uah -= 3000;
		} else if (abs(i_out_ua) < 250000) {
			/* Very gentle relaxed-OCV drift alignment (~0.1%/min max) */
			s64 fcc = mb->fcc_uah > 0 ? mb->fcc_uah : MARIE_BAT_FCC_UAH;
			s64 ocv_uah = div_s64((s64)ocv_cap * fcc, 100);
			s64 diff = ocv_uah - mb->rem_uah;
			const s64 tol = div_s64(fcc * 4, 100);

			if (diff > tol)
				mb->rem_uah += 300;
			else if (diff < -tol)
				mb->rem_uah -= 300;
		}

		marie_bat_update_cap_from_rem(mb);
		if (mb->cap != prev_cap)
			marie_bat_save_soc(mb->sdev, mb->cap);
	}

	mb->status = marie_bat_calc_status(mb->usb_online, mb->cap,
					   mb->vbat_uv, mb->i_out_ua);
}

static void marie_bat_vbus_irq_notify(void)
{
	if (!READ_ONCE(marie_bat.ready))
		return;
	if (marie_bat.sdev)
		pm_wakeup_event(&marie_bat.sdev->dev, 2500);
	mod_delayed_work(system_wq, &marie_bat.poll_work, 0);
}

static void marie_bat_poll_fn(struct work_struct *work)
{
	struct marie_battery *mb = container_of(to_delayed_work(work),
						struct marie_battery, poll_work);
	bool usb_now, usb_changed = false, bat_changed = false;
	int prev_cap, prev_status;

	usb_now = marie_usb_is_online(mb->sdev);

	mutex_lock(&mb->lock);
	mb->poll_ticks++;
	prev_cap = mb->cap;
	prev_status = mb->status;

	if (usb_now != mb->usb_online) {
		mb->usb_online = usb_now;
		usb_changed = true;
		marie_bat_sample_locked(mb, false);
		bat_changed = true;
		pr_info("MARIE_BAT: USB %s (vbat=%d mV cur=%d mA rem=%lld mAh cap=%d%% cl_out=%u cl_in=%u)\n",
			usb_now ? "connected" : "disconnected",
			mb->vbat_uv / 1000, -mb->i_out_ua / 1000,
			div_s64(mb->rem_uah, 1000), mb->cap,
			mb->cl_out_prev, mb->cl_in_prev);
	} else if ((mb->poll_ticks % MARIE_BAT_SAMPLE_TICKS) == 0) {
		marie_bat_sample_locked(mb, false);
		if (mb->cap != prev_cap || mb->status != prev_status ||
		    (mb->poll_ticks % 12) == 0)
			bat_changed = true;
	}
	mutex_unlock(&mb->lock);

	{
		bool pc_now = usb_now && kirin710_dwc3_is_configured();
		bool pc_changed;

		mutex_lock(&mb->lock);
		pc_changed = (pc_now != mb->pc_usb);
		mb->pc_usb = pc_now;
		mutex_unlock(&mb->lock);

		marie_hi6523_update(usb_now, pc_now);
		if (usb_changed)
			kirin710_dwc3_vbus_notify(usb_now);
		if (usb_changed || pc_changed) {
			if (!IS_ERR_OR_NULL(mb->usb_psy))
				power_supply_changed(mb->usb_psy);
			if (!IS_ERR_OR_NULL(mb->ac_psy))
				power_supply_changed(mb->ac_psy);
		}
	}
	if (bat_changed && !IS_ERR_OR_NULL(mb->bat_psy))
		power_supply_changed(mb->bat_psy);

	schedule_delayed_work(&mb->poll_work, msecs_to_jiffies(MARIE_BAT_POLL_MS));
}

static enum power_supply_property marie_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
};

static int marie_bat_get_prop(struct power_supply *psy,
			      enum power_supply_property psp,
			      union power_supply_propval *val)
{
	struct marie_battery *mb = power_supply_get_drvdata(psy);

	mutex_lock(&mb->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = mb->status;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = mb->cap;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = mb->vbat_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = MARIE_BAT_VBAT_MAX_UV;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		/* Standard Linux/Android ABI: positive = charging, negative = discharging */
		val->intval = -mb->i_out_ua;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = (int)mb->rem_uah;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = 280;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = (int)(mb->fcc_uah > 0 ? mb->fcc_uah : MARIE_BAT_FCC_UAH);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = MARIE_BAT_DESIGN_UAH;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = mb->cycle_count;
		break;
	default:
		mutex_unlock(&mb->lock);
		return -EINVAL;
	}
	mutex_unlock(&mb->lock);
	return 0;
}

static const struct power_supply_desc marie_bat_desc = {
	.name = "Battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = marie_bat_props,
	.num_properties = ARRAY_SIZE(marie_bat_props),
	.get_property = marie_bat_get_prop,
};

static enum power_supply_property marie_chg_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static int marie_usb_get_prop(struct power_supply *psy,
			      enum power_supply_property psp,
			      union power_supply_propval *val)
{
	struct marie_battery *mb = power_supply_get_drvdata(psy);

	mutex_lock(&mb->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = (mb->usb_online && mb->pc_usb) ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = 5000000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = 900000;
		break;
	default:
		mutex_unlock(&mb->lock);
		return -EINVAL;
	}
	mutex_unlock(&mb->lock);
	return 0;
}

static const struct power_supply_desc marie_usb_desc = {
	.name = "USB",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = marie_chg_props,
	.num_properties = ARRAY_SIZE(marie_chg_props),
	.get_property = marie_usb_get_prop,
};

static int marie_ac_get_prop(struct power_supply *psy,
			     enum power_supply_property psp,
			     union power_supply_propval *val)
{
	struct marie_battery *mb = power_supply_get_drvdata(psy);

	mutex_lock(&mb->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = (mb->usb_online && !mb->pc_usb) ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = 5000000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = 2000000;
		break;
	default:
		mutex_unlock(&mb->lock);
		return -EINVAL;
	}
	mutex_unlock(&mb->lock);
	return 0;
}

static const struct power_supply_desc marie_ac_desc = {
	.name = "Mains",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = marie_chg_props,
	.num_properties = ARRAY_SIZE(marie_chg_props),
	.get_property = marie_ac_get_prop,
};

static void marie_battery_register(void)
{
	struct power_supply_config cfg = { .drv_data = &marie_bat };
	u8 st = 0, st1 = 0, clj = 0, hrst6 = 0;

	marie_bat.sdev = kirin710_pmic_get_sdev();
	if (!marie_bat.sdev)
		return;

	mutex_init(&marie_bat.lock);
	spmi_ext_register_readl(marie_bat.sdev, PMIC_STATUS0_REG, &st, 1);
	spmi_ext_register_readl(marie_bat.sdev, PMIC_STATUS1_REG, &st1, 1);
	spmi_ext_register_readl(marie_bat.sdev, PMIC_CLJ_CTRL_REGS2_REG, &clj, 1);
	spmi_ext_register_readl(marie_bat.sdev, PMIC_HRST_REG6_REG, &hrst6, 1);

	/* Ensure HI6555v200 hardware coulomb counter is enabled */
	if (!(clj & BIT(0))) {
		u8 en = clj | BIT(0);
		spmi_ext_register_writel(marie_bat.sdev, PMIC_CLJ_CTRL_REGS2_REG, &en, 1);
	}

	mutex_lock(&marie_bat.lock);
	marie_bat.usb_online = !!(st & BIT(1));
	marie_bat.pc_usb = marie_bat.usb_online && kirin710_dwc3_is_configured();
	marie_bat_load_hicoul_nv(&marie_bat);
	marie_bat_sample_locked(&marie_bat, true);
	mutex_unlock(&marie_bat.lock);

	marie_bat.bat_psy = power_supply_register(&marie_bat.sdev->dev, &marie_bat_desc, &cfg);
	marie_bat.usb_psy = power_supply_register(&marie_bat.sdev->dev, &marie_usb_desc, &cfg);
	marie_bat.ac_psy = power_supply_register(&marie_bat.sdev->dev, &marie_ac_desc, &cfg);

	kirin710_dwc3_typec_set_connected(marie_bat.usb_online);
	if (!marie_bat.usb_online)
		kirin710_dwc3_vbus_notify(false);

	device_init_wakeup(&marie_bat.sdev->dev, true);
	INIT_DELAYED_WORK(&marie_bat.poll_work, marie_bat_poll_fn);
	WRITE_ONCE(marie_bat.ready, true);
	schedule_delayed_work(&marie_bat.poll_work, msecs_to_jiffies(MARIE_BAT_POLL_MS));

	pr_info("MARIE_BAT: registered Battery/USB (vbat=%d mV cur=%d mA ocv=%d mV -> %d%% rem=%lld mAh, cl_out=%u cl_in=%u clj=0x%02x st0=0x%02x st1=0x%02x hrst6=0x%02x)\n",
		marie_bat.vbat_uv / 1000, -marie_bat.i_out_ua / 1000,
		marie_bat.ocv_uv / 1000, marie_bat.cap,
		div_s64(marie_bat.rem_uah, 1000),
		marie_bat.cl_out_prev, marie_bat.cl_in_prev,
		clj, st, st1, hrst6);
}

/* =========================================================================
 * 6. Kirin 710 big.LITTLE CPU Frequency Scaling (LPM3 Mailbox-13 xfreq)
 * ========================================================================= */

#define KIRIN710_XFREQ_CLUSTER0_CMD	0x0001030aU
#define KIRIN710_XFREQ_CLUSTER1_CMD	0x0002030aU

static struct cpufreq_frequency_table marie_cluster0_tbl[] = {
	{ .frequency =  480000 },
	{ .frequency =  960000 },
	{ .frequency = 1152000 },
	{ .frequency = 1325000 },
	{ .frequency = 1440000 },
	{ .frequency = 1536000 },
	{ .frequency = 1709000 },
	{ .frequency = CPUFREQ_TABLE_END },
};

static struct cpufreq_frequency_table marie_cluster1_tbl[] = {
	{ .frequency =  807000 },
	{ .frequency = 1037000 },
	{ .frequency = 1268000 },
	{ .frequency = 1460000 },
	{ .frequency = 1671000 },
	{ .frequency = 1824000 },
	{ .frequency = 1997000 },
	{ .frequency = 2189000 },
	{ .frequency = CPUFREQ_TABLE_END },
};

static unsigned int marie_cluster_khz[2] = { 1325000, 1460000 };

static unsigned int marie_cpufreq_get(unsigned int cpu)
{
	return marie_cluster_khz[cpu >= 4 ? 1 : 0];
}

static int marie_cpufreq_target_index(struct cpufreq_policy *policy,
				      unsigned int index)
{
	int cluster = (policy->cpu >= 4) ? 1 : 0;
	u32 cmd0 = cluster ? KIRIN710_XFREQ_CLUSTER1_CMD : KIRIN710_XFREQ_CLUSTER0_CMD;
	unsigned int khz = policy->freq_table[index].frequency;
	int ret;

	ret = kirin710_xfreq_send_mhz(cmd0, khz / 1000);
	if (!ret)
		marie_cluster_khz[cluster] = khz;
	return ret;
}

static int marie_cpufreq_init(struct cpufreq_policy *policy)
{
	int cluster = (policy->cpu >= 4) ? 1 : 0;
	int base = cluster ? 4 : 0;
	int i;

	cpumask_clear(policy->cpus);
	for (i = base; i < base + 4; i++) {
		if (cpu_possible(i))
			cpumask_set_cpu(i, policy->cpus);
	}
	policy->freq_table = cluster ? marie_cluster1_tbl : marie_cluster0_tbl;
	policy->cpuinfo.transition_latency = 2000000;
	return 0;
}

static struct cpufreq_driver marie_cpufreq_driver = {
	.name = "kirin710-cpufreq",
	.flags = CPUFREQ_HAVE_GOVERNOR_PER_POLICY,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = marie_cpufreq_target_index,
	.get = marie_cpufreq_get,
	.init = marie_cpufreq_init,
	.attr = cpufreq_generic_attr,
};

static void marie_cpufreq_register(void)
{
	int ret, cpu;

	/* Set real Kirin 710 big.LITTLE capacities (A53@1.71GHz=446, A73@2.19GHz=1024) */
	for_each_possible_cpu(cpu)
		topology_set_cpu_scale(cpu, cpu < 4 ? 446 : 1024);
	rebuild_sched_domains();

	/* Verify LPM3 mailbox-13 accepts a frequency vote before registering */
	ret = kirin710_xfreq_send_mhz(KIRIN710_XFREQ_CLUSTER0_CMD, 1709);
	if (ret) {
		pr_warn("MARIE_CPUFREQ: LPM3 cluster0 probe failed (%d), skipping\n", ret);
		return;
	}
	marie_cluster_khz[0] = 1709000;
	if (!kirin710_xfreq_send_mhz(KIRIN710_XFREQ_CLUSTER1_CMD, 2189))
		marie_cluster_khz[1] = 2189000;

	ret = cpufreq_register_driver(&marie_cpufreq_driver);
	pr_info("MARIE_CPUFREQ: cpufreq_register_driver = %d (c0=%u kHz c1=%u kHz)\n",
		ret, marie_cluster_khz[0], marie_cluster_khz[1]);
}

/* =========================================================================
 * 7. Kirin 710 PMIC Reboot Reason (bootloader / recovery / erecovery)
 * ========================================================================= */

#define PMIC_HRST_REG13_REG	0x1e1

static int marie_reboot_notify(struct notifier_block *nb,
			       unsigned long action, void *data)
{
	struct spmi_device *sdev = kirin710_pmic_get_sdev();
	const char *cmd = data;
	u8 reason = 0x00;

	if (!sdev || action != SYS_RESTART)
		return NOTIFY_DONE;

	if (cmd) {
		if (!strcmp(cmd, "bootloader") || !strcmp(cmd, "fastboot"))
			reason = 0x01;
		else if (!strcmp(cmd, "recovery"))
			reason = 0x02;
		else if (!strcmp(cmd, "chargereboot"))
			reason = 0x06;
		else if (!strcmp(cmd, "erecovery"))
			reason = 0x08;
	}

	spmi_ext_register_writel(sdev, PMIC_HRST_REG13_REG, &reason, 1);
	pr_info("MARIE_REBOOT: cmd=%s -> PMIC_HRST_REG13(0x1e1)=0x%02x\n",
		cmd ? cmd : "(none)", reason);
	return NOTIFY_DONE;
}

static struct notifier_block marie_reboot_nb = {
	.notifier_call = marie_reboot_notify,
};

/* Kirin 710 hardware power-off via hisi,powerhold PL061 GPIO */
#define MARIE_PL061_GPIODIR	0x400

static void __iomem *marie_powerhold_base;
static void __iomem *marie_sysctrl_base;
static u32 marie_powerhold_offset;
static u32 marie_powerhold_prot_off = 0xffff;
static u32 marie_powerhold_prot_bit = 0xff;
static struct sys_off_handler *marie_poweroff_handler;

static int marie_sys_poweroff(struct sys_off_data *data)
{
	struct spmi_device *sdev = kirin710_pmic_get_sdev();
	u32 val;
	u8 reason = 0x06; /* chargereboot */

	if (!marie_powerhold_base)
		return NOTIFY_DONE;

	pr_emerg("MARIE_POWEROFF: dropping powerhold GPIO (offset=%u)\n",
		 marie_powerhold_offset);

	if (marie_sysctrl_base && marie_powerhold_prot_off != 0xffff &&
	    marie_powerhold_prot_bit < 16) {
		val = readl(marie_sysctrl_base + marie_powerhold_prot_off);
		val = (val & ~BIT(marie_powerhold_prot_bit)) |
		      BIT(marie_powerhold_prot_bit + 16);
		writel(val, marie_sysctrl_base + marie_powerhold_prot_off);
	}

	val = readl(marie_powerhold_base + MARIE_PL061_GPIODIR);
	val |= (1u << marie_powerhold_offset);
	writel(val, marie_powerhold_base + MARIE_PL061_GPIODIR);
	writel(0, marie_powerhold_base + (1u << (marie_powerhold_offset + 2)));
	dsb(sy);

	/*
	 * When unplugged, HI6555v200 cuts all power rails immediately above.
	 * When VBUS/charger is connected, PMIC stays alive, so after 1s we
	 * reboot with reason 0x06 ("chargereboot") into offline charger mode.
	 */
	mdelay(1000);
	pr_emerg("MARIE_POWEROFF: still powered (VBUS present) -> chargereboot\n");
	if (sdev)
		spmi_ext_register_writel(sdev, PMIC_HRST_REG13_REG, &reason, 1);
	do_kernel_restart("chargereboot");

	while (1)
		cpu_relax();

	return NOTIFY_DONE;
}

static void marie_poweroff_register(void)
{
	struct device_node *np;
	u32 off = 0;

	np = of_find_compatible_node(NULL, NULL, "hisi,powerhold");
	if (!np) {
		pr_warn("MARIE_POWEROFF: hisi,powerhold DT node not found\n");
		return;
	}

	marie_powerhold_base = of_iomap(np, 0);
	if (!marie_powerhold_base || of_property_read_u32(np, "offset", &off)) {
		pr_warn("MARIE_POWEROFF: failed to map hisi,powerhold\n");
		if (marie_powerhold_base) {
			iounmap(marie_powerhold_base);
			marie_powerhold_base = NULL;
		}
		of_node_put(np);
		return;
	}
	marie_powerhold_offset = off;

	if (!of_property_read_u32(np, "powerhold_protect_offset", &off)) {
		u32 bit = 0;

		if (!of_property_read_u32(np, "powerhold_protect_bit", &bit)) {
			marie_powerhold_prot_off = off;
			marie_powerhold_prot_bit = bit;
		}
	}
	of_node_put(np);

	np = of_find_compatible_node(NULL, NULL, "hisilicon,sysctrl");
	if (np) {
		marie_sysctrl_base = of_iomap(np, 0);
		of_node_put(np);
	}

	marie_poweroff_handler = register_sys_off_handler(SYS_OFF_MODE_POWER_OFF,
							  SYS_OFF_PRIO_HIGH,
							  marie_sys_poweroff,
							  NULL);
	if (IS_ERR(marie_poweroff_handler)) {
		pr_warn("MARIE_POWEROFF: register_sys_off_handler err=%ld\n",
			PTR_ERR(marie_poweroff_handler));
		marie_poweroff_handler = NULL;
	} else {
		pr_info("MARIE_POWEROFF: registered powerhold handler (offset=%u prot=0x%x/%u)\n",
			marie_powerhold_offset, marie_powerhold_prot_off,
			marie_powerhold_prot_bit);
	}
}

static struct delayed_work marie_deferred_boot_work;

static void marie_deferred_boot_fn(struct work_struct *work)
{
	void __iomem *crg, *spi1, *pmx;
	struct device_node *clk_np;
	struct clk_hw *hw;

	pr_info("MARIE_INPUT: deferred boot start\n");
	crg = ioremap(0xfff35000, 0x1000);
	if (crg) {
		/* Enable clk_spi1 (bit 9) and clk_i2c3 (bit 7), release i2c3 reset (bit 7) */
		writel(BIT(7), crg + 0x7c); /* PERRSTDIS2: release i2c3 reset */
		writel(BIT(9) | BIT(7), crg + 0x20); /* PEREN2: enable spi1 + i2c3 */
		dsb(sy);
		udelay(10);
		pr_info("MARIE_SPI1_DIAG: PERSTAT2=0x%08x PERRSTSTAT2=0x%08x\n",
			readl(crg + 0x28), readl(crg + 0x80));
		iounmap(crg);
	}

	/* Ensure I2C3 pinmux (GPIO5=0x700, GPIO6=0x704 -> ALT1) */
	pmx = ioremap(0xe896c000, 0x1000);
	if (pmx) {
		writel(0x1, pmx + 0x700);
		writel(0x1, pmx + 0x704);
		dsb(sy);
		iounmap(pmx);
	}

	spi1 = ioremap(0xfdf08000, 0x1000);
	if (spi1) {
		u32 pid = (readl(spi1 + 0xfe0) & 0xff) |
			  ((readl(spi1 + 0xfe4) & 0xff) << 8) |
			  ((readl(spi1 + 0xfe8) & 0xff) << 16) |
			  ((readl(spi1 + 0xfec) & 0xff) << 24);
		pr_info("MARIE_SPI1_DIAG: PL022 PrimeCell ID = 0x%08x\n", pid);
		iounmap(spi1);
	}

	clk_np = of_find_node_by_path("/clocks@0/crgctrl@0xfff35000/clk_spi1");
	if (clk_np) {
		hw = clk_hw_register_fixed_rate(NULL, "clk_spi1", NULL, 0, 40550400);
		if (!IS_ERR(hw)) {
			int cret = of_clk_add_hw_provider(clk_np, of_clk_hw_simple_get, hw);
			pr_info("MARIE_SPI1_DIAG: registered clk_spi1 provider = %d\n", cret);
			fw_devlink_purge_absent_suppliers(of_fwnode_handle(clk_np));
		}
		of_node_put(clk_np);
	}

	clk_np = of_find_node_by_path("/clocks@0/crgctrl@0xfff35000/clk_i2c3");
	if (clk_np) {
		hw = clk_hw_register_fixed_rate(NULL, "clk_i2c3", NULL, 0, 109000000);
		if (!IS_ERR(hw)) {
			int cret = of_clk_add_hw_provider(clk_np, of_clk_hw_simple_get, hw);
			pr_info("MARIE_BL: registered clk_i2c3 provider = %d\n", cret);
			fw_devlink_purge_absent_suppliers(of_fwnode_handle(clk_np));
		}
		of_node_put(clk_np);
	}

	/* Enable IOMCU I2C6 (0xFFD6A000): release reset & enable clock in iomcu_crgctrl (0xFFD7E000) */
	{
		void __iomem *iomcu_crg = ioremap(0xffd7e000, 0x1000);
		void __iomem *pmx6 = ioremap(0xfff11000, 0x1000);

		if (iomcu_crg) {
			writel(BIT(27), iomcu_crg + 0x24); /* RSTDIS bit 27 */
			writel(BIT(27), iomcu_crg + 0x10); /* PEREN0 bit 27 */
			dsb(sy);
			udelay(10);
			iounmap(iomcu_crg);
		}
		if (pmx6) {
			writel(0x1, pmx6 + 0x710); /* GPIO212 -> I2C6_SCL */
			writel(0x1, pmx6 + 0x714); /* GPIO213 -> I2C6_SDA */
			dsb(sy);
			iounmap(pmx6);
		}
	}

	/* clk_i2c6 is a fixed-factor /4 clock whose parent clk_i2c6_gt has no driver */
	clk_np = of_find_node_by_path("/clocks@0/iomcu_crgctrl@0xFFD7E000/clk_i2c6_gt");
	if (clk_np) {
		hw = clk_hw_register_fixed_rate(NULL, "clk_i2c6_gt", NULL, 0, 132000000);
		if (!IS_ERR(hw)) {
			int cret = of_clk_add_hw_provider(clk_np, of_clk_hw_simple_get, hw);
			pr_info("MARIE_CHG: registered clk_i2c6_gt (132MHz -> i2c6 33MHz) = %d\n", cret);
			fw_devlink_purge_absent_suppliers(of_fwnode_handle(clk_np));
		} else {
			pr_warn("MARIE_CHG: clk_i2c6_gt register err=%ld\n", PTR_ERR(hw));
		}
		of_node_put(clk_np);
	}

	i2c_add_driver(&marie_lm36274_driver);
	i2c_add_driver(&marie_hi6523_driver);

	/* Re-attach ffd6a000.i2c if its early probe failed before clk_i2c6_gt existed */
	clk_np = of_find_node_by_path("/amba/i2c@FFD6A000");
	if (clk_np) {
		struct platform_device *i2c6_pdev = of_find_device_by_node(clk_np);
		if (i2c6_pdev) {
			if (!i2c6_pdev->dev.driver) {
				int aret = device_attach(&i2c6_pdev->dev);
				pr_info("MARIE_CHG: device_attach(ffd6a000.i2c) = %d\n", aret);
			}
			put_device(&i2c6_pdev->dev);
		}
		of_node_put(clk_np);
	}
	marie_battery_register();
	marie_cpufreq_register();
	register_reboot_notifier(&marie_reboot_nb);

	pr_info("MARIE_INPUT: registering ts_kit driver...\n");
	platform_driver_register(&marie_tcm_driver);
}

static struct delayed_work marie_early_keys_work;

static void marie_early_keys_fn(struct work_struct *work)
{
	pr_info("MARIE_INPUT: registering early gpio keys + PMIC powerkey...\n");
	platform_driver_register(&marie_gpio_keys_driver);
	INIT_DELAYED_WORK(&marie_pk.init_work, marie_ponkey_init_work);
	schedule_delayed_work(&marie_pk.init_work, msecs_to_jiffies(200));
}

static int __init marie_input_init(void)
{
	marie_poweroff_register();
	INIT_DELAYED_WORK(&marie_early_keys_work, marie_early_keys_fn);
	schedule_delayed_work(&marie_early_keys_work, msecs_to_jiffies(800));
	INIT_DELAYED_WORK(&marie_deferred_boot_work, marie_deferred_boot_fn);
	schedule_delayed_work(&marie_deferred_boot_work, msecs_to_jiffies(5000));
	return 0;
}
late_initcall(marie_input_init);

MODULE_AUTHOR("apkreader");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Huawei P30 lite (marie) GPIO keys, PMIC powerkey, and TD4320 touchscreen");

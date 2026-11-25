// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 apkreader
 *
 * Huawei P30 lite (marie / Kirin 710) I2C Torch/Flash LED driver.
 * Supports AW3642 (0x63, ID 0x36), TI LM3642 (0x63, ID&0x07 == 0x00),
 * and MPS MP3331 (0x67, ID&0xF8 == 0x18).
 */

#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>

enum marie_flash_chip {
	MARIE_FLASH_NONE = 0,
	MARIE_FLASH_AW3642,
	MARIE_FLASH_LM3642,
	MARIE_FLASH_MP3331,
};

struct marie_flash {
	struct i2c_client *client;
	struct led_classdev cdev;
	struct mutex lock;
	enum marie_flash_chip chip;
	u8 default_torch_level;
	bool is_on;
};

static bool marie_flash_registered;

static int marie_flash_reg_read(struct i2c_client *client, u8 reg, u8 *val)
{
	int ret = i2c_smbus_read_byte_data(client, reg);

	if (ret < 0)
		return ret;
	*val = (u8)ret;
	return 0;
}

static int marie_flash_reg_write(struct i2c_client *client, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(client, reg, val);
}

static int marie_flash_set_aw3642(struct marie_flash *mf, enum led_brightness br)
{
	u8 dummy;

	if (br == LED_OFF) {
		/* TX_PIN_EN (0x80) | MODE_STANDBY (0x00) */
		return marie_flash_reg_write(mf->client, 0x01, 0x80);
	}

	/* Clear OCP/OVP flags */
	marie_flash_reg_read(mf->client, 0x0a, &dummy);
	marie_flash_reg_read(mf->client, 0x0b, &dummy);

	/* Map brightness 1..255 -> safe continuous torch levels 4..31 (~29..188 mA) */
	u8 lvl = 4 + ((u32)(br - 1) * 27) / 254;
	int ret = marie_flash_reg_write(mf->client, 0x05, 0x80 | (lvl & 0x3f));
	if (ret < 0)
		return ret;

	/* MODE_TORCH (0x08) | LED_EN (0x03) */
	return marie_flash_reg_write(mf->client, 0x01, 0x0b);
}

static int marie_flash_set_lm3642(struct marie_flash *mf, enum led_brightness br)
{
	u8 flags = 0, cur = 0;

	marie_flash_reg_read(mf->client, 0x0b, &flags);
	if (br == LED_OFF) {
		/* MODE_STANDBY */
		return marie_flash_reg_write(mf->client, 0x0a, 0x00);
	}

	/* Map brightness 1..255 -> safe torch levels 0..3 (~48..188 mA) */
	u8 lvl = ((u32)(br - 1) * 3) / 254;
	int ret = marie_flash_reg_read(mf->client, 0x09, &cur);
	if (ret < 0)
		return ret;
	cur = (cur & 0x0f) | ((lvl & 0x07) << 4);
	ret = marie_flash_reg_write(mf->client, 0x09, cur);
	if (ret < 0)
		return ret;

	/* MODE_TORCH (0x02) | IVFM_EN (0x80) */
	return marie_flash_reg_write(mf->client, 0x0a, 0x82);
}

static int marie_flash_set_mp3331(struct marie_flash *mf, enum led_brightness br)
{
	u8 dummy;

	marie_flash_reg_read(mf->client, 0x0b, &dummy);
	marie_flash_reg_read(mf->client, 0x0c, &dummy);

	if (br == LED_OFF) {
		/* STB_LV (0x80), LED1_EN cleared */
		return marie_flash_reg_write(mf->client, 0x01, 0x80);
	}

	/* Map brightness 1..255 -> safe torch levels 1..6 (~32..190 mA) */
	u8 lvl = 1 + ((u32)(br - 1) * 5) / 254;
	int ret = marie_flash_reg_write(mf->client, 0x01, 0x84); /* STB_LV | TORCH_MODE_CUR */
	if (ret < 0)
		return ret;
	ret = marie_flash_reg_write(mf->client, 0x0a, lvl);
	if (ret < 0)
		return ret;
	/* STB_LV | TORCH_MODE_CUR | LED1_EN */
	return marie_flash_reg_write(mf->client, 0x01, 0x94);
}

static int marie_flash_brightness_set(struct led_classdev *cdev,
				      enum led_brightness br)
{
	struct marie_flash *mf = container_of(cdev, struct marie_flash, cdev);
	int ret = -ENODEV;

	mutex_lock(&mf->lock);
	switch (mf->chip) {
	case MARIE_FLASH_AW3642:
		ret = marie_flash_set_aw3642(mf, br);
		break;
	case MARIE_FLASH_LM3642:
		ret = marie_flash_set_lm3642(mf, br);
		break;
	case MARIE_FLASH_MP3331:
		ret = marie_flash_set_mp3331(mf, br);
		break;
	default:
		break;
	}
	if (!ret)
		mf->is_on = (br != LED_OFF);
	mutex_unlock(&mf->lock);
	return ret;
}

static int marie_flash_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct marie_flash *mf;
	u8 id = 0;
	int ret;

	if (marie_flash_registered)
		return -ENODEV;

	ret = marie_flash_reg_read(client, 0x00, &id);
	if (ret < 0) {
		dev_info(dev, "marie-flash: no ACK at 0x%02x (%d)\n", client->addr, ret);
		return -ENODEV;
	}

	mf = devm_kzalloc(dev, sizeof(*mf), GFP_KERNEL);
	if (!mf)
		return -ENOMEM;

	mf->client = client;
	mutex_init(&mf->lock);

	if (client->addr == 0x63) {
		if (id == 0x36) {
			mf->chip = MARIE_FLASH_AW3642;
			/* UVLO_EN(0x40) | IVFM_VOLTAGE(0x18) | IVFM_EN(0x01) */
			marie_flash_reg_write(client, 0x02, 0x59);
			marie_flash_reg_write(client, 0x01, 0x80);
			dev_info(dev, "marie-flash: detected AW3642 at 0x63 (id=0x%02x)\n", id);
		} else if ((id & 0x07) == 0x00) {
			mf->chip = MARIE_FLASH_LM3642;
			/* IVFM_EN(0x80), UVLO_EN(0x80)|UVLO_3.2V(0x0C) */
			marie_flash_reg_write(client, 0x0a, 0x80);
			marie_flash_reg_write(client, 0x01, 0x8c);
			marie_flash_reg_write(client, 0x0a, 0x00);
			dev_info(dev, "marie-flash: detected LM3642 at 0x63 (id=0x%02x)\n", id);
		} else {
			dev_info(dev, "marie-flash: unknown chip at 0x63 (id=0x%02x)\n", id);
			return -ENODEV;
		}
	} else if (client->addr == 0x67) {
		if ((id & 0xf8) == 0x18) {
			mf->chip = MARIE_FLASH_MP3331;
			marie_flash_reg_write(client, 0x01, 0x80); /* STB_LV */
			marie_flash_reg_write(client, 0x02, 0x0b); /* FS_SD | IL_PEAK */
			marie_flash_reg_write(client, 0x03, 0xba); /* FL_TIM | LED_SD | SW_FS */
			marie_flash_reg_write(client, 0x04, 0x5f); /* VBL_RUN | VBL_SD */
			marie_flash_reg_write(client, 0x05, 0xf4); /* LED_OTAD | TUP_I | VTH_PAS */
			marie_flash_reg_write(client, 0x06, 0x18); /* I_FL */
			marie_flash_reg_write(client, 0x07, 0x08); /* I_TX */
			marie_flash_reg_write(client, 0x0a, 0x05); /* I_TOR */
			dev_info(dev, "marie-flash: detected MP3331 at 0x67 (id=0x%02x)\n", id);
		} else {
			dev_info(dev, "marie-flash: unknown chip at 0x67 (id=0x%02x)\n", id);
			return -ENODEV;
		}
	} else {
		return -ENODEV;
	}

	mf->cdev.name = "torch";
	mf->cdev.max_brightness = 255;
	mf->cdev.brightness = LED_OFF;
	mf->cdev.brightness_set_blocking = marie_flash_brightness_set;

	ret = devm_led_classdev_register(dev, &mf->cdev);
	if (ret) {
		dev_err(dev, "marie-flash: failed to register torch LED (%d)\n", ret);
		return ret;
	}

	marie_flash_registered = true;
	i2c_set_clientdata(client, mf);
	dev_info(dev, "marie-flash: registered /sys/class/leds/torch\n");
	return 0;
}

static void marie_flash_shutdown(struct i2c_client *client)
{
	struct marie_flash *mf = i2c_get_clientdata(client);

	if (mf && mf->is_on)
		marie_flash_brightness_set(&mf->cdev, LED_OFF);
}

static const struct of_device_id marie_flash_of_match[] = {
	{ .compatible = "huawei,lm3642" },
	{ .compatible = "huawei,mp3331" },
	{ }
};
MODULE_DEVICE_TABLE(of, marie_flash_of_match);

static struct i2c_driver marie_flash_driver = {
	.driver = {
		.name = "marie-flash",
		.of_match_table = marie_flash_of_match,
	},
	.probe = marie_flash_probe,
	.shutdown = marie_flash_shutdown,
};
module_i2c_driver(marie_flash_driver);

MODULE_DESCRIPTION("Huawei P30 lite (marie) I2C Torch/Flash LED Driver");
MODULE_LICENSE("GPL");

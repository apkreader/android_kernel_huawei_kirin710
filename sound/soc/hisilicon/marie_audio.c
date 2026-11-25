// SPDX-License-Identifier: GPL-2.0-only
/*
 * Huawei P30 lite (marie) Kirin 710 ALSA Audio Driver
 * Supports HI6555v2 SoCCODEC/PMIC analog codec, NXP TFA9874 SmartPA (I2S1),
 * HiFi3 DSP shared-memory PCM mailbox, and 3.5mm headset jack detection.
 *
 * Copyright (C) 2026 apkreader
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/ktime.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox/kirin710.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/spmi.h>
#include <linux/sysfs.h>
#include <linux/uio.h>
#include <linux/workqueue.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

extern struct spmi_device *kirin710_pmic_get_sdev(void);

/* Physical MMIO & Reserved DDR Addresses on Kirin 710 (CONFIG_HIFI_IPC_3660) */
#define MARIE_SCTRL_PHYS		0xfff0a000UL
#define MARIE_CRGCTRL_PHYS		0xfff35000UL
#define MARIE_IPC_NS_PHYS		0xe896b000UL
#define MARIE_SOCCODEC_PHYS		0xe8052000UL
#define MARIE_ASPCFG_PHYS		0xe804e000UL
#define MARIE_ASPDMAC_PHYS		0xe804b000UL

#define HIFI_UNSEC_BASE_ADDR		0x2d500000UL
#define HIFI_UNSEC_REGION_SIZE		0x00500000UL
#define PCM_PLAY_BUFF_OFFSET		0x00164000UL
#define PCM_PLAY_BUFF_SIZE		0x001ce000UL
#define DRV_DSP_UART_OFFSET		0x00332000UL
#define DRV_DSP_UART_SIZE		0x0007f000UL
#define HIFI_FLAG_DATA_OFFSET		0x003c5000UL
#define HIFI_AP_NV_OFFSET		0x003c7000UL
#define HIFI_AP_NV_SIZE			0x0004bc00UL
#define HIFI_MAILBOX_OFFSET		0x00412c00UL
#define HIFI_MAILBOX_SIZE		0x00010000UL

/* PCM & ASP_DMAC ring buffers inside HIFI_UNSEC_BASE_ADDR (PCM_PLAY_BUFF_OFFSET) */
#define PCM_PB_ALSA_OFF			0x00164000UL
#define PCM_PB_DMA_PHYS			(HIFI_UNSEC_BASE_ADDR + PCM_PB_ALSA_OFF)
#define PCM_PB_DMA_MAX_BYTES		65536UL

#define PCM_CP_ALSA_OFF			0x00174000UL
#define PCM_CP_DMA_PHYS			(HIFI_UNSEC_BASE_ADDR + PCM_CP_ALSA_OFF)
#define PCM_CP_DMA_MAX_BYTES		65536UL

#define DMAC_PB_L_OFF			0x00184000UL /* 128 KiB 32-bit MSB Left DL */
#define DMAC_PB_R_OFF			0x001a4000UL /* 128 KiB 32-bit MSB Right DL */
#define DMAC_CP_L_OFF			0x001c4000UL /* 128 KiB 32-bit MSB Left UL */
#define DMAC_CP_R_OFF			0x001e4000UL /* 128 KiB 32-bit MSB Right UL */
#define DMAC_LLI_OFF			0x00204000UL /* 4 KiB 32B-aligned cyclic LLI rings */
#define DMAC_MAX_PERIODS		16U

/* ASP_DMAC (0xe804b000) registers & HI6555v2 SoCCODEC FIFO ports */
#define ASP_DMA_INT_TC1_RAW		0x600U
#define ASP_DMA_INT_TC2_RAW		0x608U
#define ASP_DMA_INT_ERR1_RAW		0x610U
#define ASP_DMA_INT_ERR2_RAW		0x618U
#define ASP_DMA_INT_ERR3_RAW		0x620U
#define ASP_DMA_CH_STAT			0x690U
#define ASP_DMA_CX_CURR_SRC(ch)		(0x708U + 0x10U * (ch))
#define ASP_DMA_CX_CURR_DES(ch)		(0x70cU + 0x10U * (ch))
#define ASP_DMA_CX_LLI(ch)		(0x800U + 0x40U * (ch))
#define ASP_DMA_CX_BINDX(ch)		(0x804U + 0x40U * (ch))
#define ASP_DMA_CX_CINDX(ch)		(0x808U + 0x40U * (ch))
#define ASP_DMA_CX_CNT1(ch)		(0x80cU + 0x40U * (ch))
#define ASP_DMA_CX_CNT0(ch)		(0x810U + 0x40U * (ch))
#define ASP_DMA_CX_SRC(ch)		(0x814U + 0x40U * (ch))
#define ASP_DMA_CX_DES(ch)		(0x818U + 0x40U * (ch))
#define ASP_DMA_CX_CONFIG(ch)		(0x81cU + 0x40U * (ch))

#define DMAC_CH_PB_L			5U
#define DMAC_CH_PB_R			6U
#define DMAC_CH_CP_L			7U
#define DMAC_CH_CP_R			8U

#define SOCCODEC_PB_L_FIFO_PHYS		0xe805210cU
#define SOCCODEC_PB_R_FIFO_PHYS		0xe8052110U
#define SOCCODEC_CP_L_FIFO_PHYS		0xe8052114U
#define SOCCODEC_CP_R_FIFO_PHYS		0xe8052118U

#define DMAC_CFG_PB_L			0x833220d7U /* TX DDR->FIFO, 32b, burst 4, peri 13 */
#define DMAC_CFG_PB_R			0x833220e7U /* TX DDR->FIFO, 32b, burst 4, peri 14 */
#define DMAC_CFG_CP_L			0x43322067U /* RX FIFO->DDR, 32b, burst 4, peri 6 */
#define DMAC_CFG_CP_R			0x43322077U /* RX FIFO->DDR, 32b, burst 4, peri 7 */

/* HiFi3 Shared-Memory Mailbox Layout */
#define MB_PROTECT1			0x55aa55aaU
#define MB_PROTECT2			0x5a5a5a5aU
#define MB_MSGHEAD_MAGIC		0xa5a5a5a5U
#define MB_QUEUE_RAW_SIZE		0x00001800U
#define MB_QUEUE_USABLE_SIZE		(MB_QUEUE_RAW_SIZE - 16U)

#define MB_TX_HEAD_OFF			(HIFI_MAILBOX_OFFSET + 2U * 40U) /* ACPU -> HIFI */
#define MB_RX_HEAD_OFF			(HIFI_MAILBOX_OFFSET + 3U * 40U) /* HIFI -> ACPU */
#define MB_TX_DATA_OFF			(HIFI_MAILBOX_OFFSET + 30U * 40U)
#define MB_RX_DATA_OFF			(MB_TX_DATA_OFF + MB_QUEUE_RAW_SIZE)

#define MAILCODE_ACPU_TO_HIFI_AUDIO	0x00040003U
#define MAILCODE_ACPU_TO_HIFI_MISC	0x00040004U
#define MAILCODE_HIFI_TO_ACPU_AUDIO	0x04000003U
#define HIFI_NVPARAM_MAGIC		0x55aa55aaU
#define HIFI_NVPARAM_TAIL_OFF		0x0004b964U /* NVPARAM_TOTAL_SIZE(0x4b968) - 4 */

/* HiFi3 Audio Channel Message IDs */
#define HI_CHN_MSG_PCM_OPEN		0xdd25U
#define HI_CHN_MSG_PCM_CLOSE		0xdd26U
#define HI_CHN_MSG_PCM_HW_PARAMS	0xdd27U
#define HI_CHN_MSG_PCM_TRIGGER		0xdd2aU
#define HI_CHN_MSG_PCM_SET_BUF		0xdd2cU
#define HI_CHN_MSG_PCM_PERIOD_ELAPSED	0xdd2dU
#define HI_CHN_MSG_PCM_XRUN		0xdd2eU
#define HI_CHN_MSG_PCM_TRIGGER_CNF	0xdda0U

/* IPC mailbox-2 (HIFI -> AP RX) registers */
#define IPC_MBX_BASE(ch)		((ch) * 0x40U)
#define IPC_MBX_MODE_REG		0x10U
#define IPC_MBX_IMASK_REG		0x14U
#define IPC_MBX_ICLR_REG		0x18U
#define IPC_MBX_SEND_REG		0x1cU
#define IPC_MBX_DATA0_REG		0x20U
#define IPC_MODE_AUTOACK		BIT(0)
#define IPC_MODE_DEST_STAT		BIT(6)

/* ASPCFG (0xe804e000) registers */
#define ASPCFG_RST_CTRLEN		0x000U
#define ASPCFG_RST_CTRLDIS		0x004U
#define ASPCFG_GATE_EN			0x00cU
#define ASPCFG_CODEC_DMA_SEL		0x1d8U

/* SoCCODEC (0xe8052000) registers */
#define SC_VERSION_REG			0x00U
#define SC_CLK_EN0_REG			0x04U
#define SC_CLK_EN1_REG			0x08U
#define SC_SW_RST_N_REG			0x0cU
#define SC_I2S1_TDM_CTRL0_REG		0x10U
#define SC_I2S1_TDM_CTRL1_REG		0x14U
#define SC_AUDIO_L_DN_PGA_REG		0x38U
#define SC_AUDIO_R_DN_PGA_REG		0x3cU
#define SC_AUDIO_L_UP_PGA_REG		0x44U
#define SC_AUDIO_R_UP_PGA_REG		0x48U
#define SC_SRCUP_CTRL_REG		0x58U
#define SC_DACL_MIXER4_CTRL0_REG	0x60U
#define SC_DACR_MIXER4_CTRL0_REG	0x68U
#define SC_MIXER4_INPUT_MUTES		GENMASK(23, 20)
#define SC_MIXER4_IN3_MUTE		BIT(22)
#define SC_MIXER4_IN3_GAIN		GENMASK(17, 16)
#define SC_I2S1_TX_CLK_SEL		BIT(30)
#define SC_I2S1_IO_WORD_LENGTH		GENMASK(29, 28)
#define SC_DAC_FILTER_CTRL_REG		0x74U
#define SC_ADC_FILTER_CTRL_REG		0x78U
#define SC_AUDIO_DN_AFIFO_CTRL_REG	0x80U
#define SC_AUDIO_UP_AFIFO_CTRL_REG	0x84U
#define SC_FS_CTRL0_REG			0xa4U
#define SC_CODEC_DIN_MUX_REG		0xacU

/* PMIC LDO8 & Analog Codec registers (via SPMI) */
#define PMIC_LDO8_ENABLE_REG		0x017U
#define PMIC_LDO8_VSET_REG		0x050U
#define PMIC_SOFT_RST_REG		0x10dU
#define PMIC_CLASSD_CTRL0_REG		0x109U
#define PMIC_CLK_EN_CFG_REG		0x380U
#define PMIC_DAC_CHAN_CTRL_REG		0x383U
#define PMIC_ADC_CHAN_CTRL_REG		0x384U
#define PMIC_ANA_IRQ_STAT_REG		0x385U
#define PMIC_ANA_IRQM_REG0		0x386U
#define PMIC_ANA_IRQ_REG0		0x387U
#define PMIC_DEB_HS_MIC_REG		0x389U
#define PMIC_ANA_RW1_REG		0x38aU
#define PMIC_ANA_RW2_REG		0x38bU
#define PMIC_ANA_RW3_REG		0x38cU
#define PMIC_ANA_RW4_REG		0x38dU
#define PMIC_ANA_RW5_REG		0x38eU
#define PMIC_ANA_RW6_REG		0x38fU
#define PMIC_ANA_RW7_REG		0x390U
#define PMIC_ANA_RW8_REG		0x391U
#define PMIC_ANA_RW9_REG		0x392U
#define PMIC_ANA_RW10_REG		0x393U
#define PMIC_ANA_RW12_REG		0x395U
#define PMIC_ANA_RW14_REG		0x397U
#define PMIC_ANA_RW16_REG		0x399U
#define PMIC_ANA_RW19_REG		0x39cU
#define PMIC_ANA_RW20_REG		0x39dU
#define PMIC_ANA_RW21_REG		0x39eU
#define PMIC_ANA_RW22_REG		0x39fU
#define PMIC_ANA_RW23_REG		0x3a0U
#define PMIC_ANA_RW24_REG		0x3a1U
#define PMIC_ANA_RW25_REG		0x3a2U
#define PMIC_ANA_RW26_REG		0x3a3U
#define PMIC_ANA_RW28_REG		0x3a5U
#define PMIC_ANA_RW29_REG		0x3a6U
#define PMIC_ANA_RW31_REG		0x3a8U
#define PMIC_ANA_RW33_REG		0x3aaU
#define PMIC_ANA_RW37_REG		0x3aeU
#define PMIC_ANA_RW40_REG		0x3b1U

enum marie_out_route {
	MARIE_OUT_SPEAKER = 0,
	MARIE_OUT_HEADPHONES = 1,
	MARIE_OUT_EARPIECE = 2,
	MARIE_OUT_SPK_HP = 3,
};

enum marie_in_route {
	MARIE_IN_MAIN_MIC = 0,
	MARIE_IN_HEADSET_MIC = 1,
};

struct hifi_mb_head {
	u32 protect1;
	u32 protect2;
	u32 front;
	u32 rear;
	u32 front_slice;
	u32 rear_slice;
	u16 reserve[4];
	u32 protect3;
	u32 protect4;
} __packed;

struct hifi_mb_mail {
	u32 partition;
	u32 write_slice;
	u32 read_slice;
	u32 seq_num;
	u32 priority;
	u32 mail_code;
	u32 reserve[2];
	u32 msg_length;
} __packed;

struct hifi_chn_pcm_open {
	u16 msg_type;
	u16 pcm_device;
	u16 pcm_mode;
	u16 reserved;
	u32 channels;
	u32 rate;
	u32 period_size;
	u32 period_count;
	u32 format;
} __packed;

struct hifi_chn_pcm_close {
	u16 msg_type;
	u16 pcm_device;
	u16 pcm_mode;
	u16 reserved;
} __packed;

struct hifi_chn_pcm_hw_params {
	u16 msg_type;
	u16 pcm_device;
	u16 pcm_mode;
	u16 reserved;
	u32 channel_num;
	u32 sample_rate;
	u32 format;
} __packed;

struct hifi_chn_pcm_trigger {
	u16 msg_type;
	u16 pcm_device;
	u16 pcm_mode;
	u16 reserved;
	u16 tg_cmd;
	u16 reserved1;
	u32 substream_l32;
	u32 substream_h32;
	u32 data_addr;
	u32 data_len;
} __packed;

struct hifi_chn_set_buffer {
	u16 msg_type;
	u16 pcm_device;
	u16 pcm_mode;
	u16 reserved;
	u32 data_addr;
	u32 data_len;
} __packed;

struct hifi_chn_period_elapsed {
	u16 msg_type;
	u16 pcm_device;
	u16 pcm_mode;
	u16 reserved;
	u32 substream_l32;
	u32 substream_h32;
	u32 msg_timestamp;
} __packed;

struct marie_dma_lli {
	u32 lli;
	u32 reserved[3];
	u32 a_count;
	u32 src_addr;
	u32 des_addr;
	u32 config;
} __aligned(32);

struct marie_stream_rt {
	spinlock_t lock;
	struct snd_pcm_substream *substream;
	unsigned int period_bytes;
	unsigned int period_frames;
	unsigned int periods;
	unsigned int buf_frames;
	unsigned int channels;
	unsigned int period_cur;
	unsigned int period_next;
	unsigned int last_hw_frame;
	bool running;
	bool hifi_acked;
	u64 start_ns;
	u64 last_adv_ns;
	u64 periods_elapsed;
};

struct marie_audio {
	struct device *dev;
	struct spmi_device *pmic;
	struct i2c_client *tfa_i2c;
	struct pinctrl *codec_pctrl;
	struct pinctrl_state *codec_pin_default;

	void __iomem *sctrl;
	void __iomem *crgctrl;
	void __iomem *ipc_base;
	void __iomem *soccodec;
	void __iomem *aspcfg;
	void __iomem *dmac;
	u8 *hifi_mem;

	struct mbox_client tx_client;
	struct mbox_chan *tx_chan;
	u32 tx_ipc_buf[8];
	int rx_irq;
	bool hifi_woken;

	spinlock_t mb_tx_lock;
	spinlock_t mb_rx_lock;
	u32 tx_seq;
	u32 rx_seq;
	atomic_t tx_mail_cnt;
	atomic_t rx_irq_cnt;
	atomic_t rx_mail_cnt;
	u32 last_rx_msg;

	struct mutex route_lock;
	enum marie_out_route out_route;
	enum marie_in_route in_route;
	int spk_vol;   /* 0..100 */
	int hp_vol;    /* 0..15 */
	int ear_vol;   /* 0..15 */
	int mic_vol;   /* 0..7 */
	bool pb_active;
	bool cp_active;
	bool tfa_inited;
	bool tfa_powered;
	bool hp_powered;
	bool ear_powered;
	u16 tfa_rev;
	u32 sc_version;
	u32 sc_i2s1_ctrl0_boot;

	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_jack *jack;
	bool jack_plugged;
	struct delayed_work jack_work;
	struct delayed_work route_work;

	struct marie_stream_rt streams[2];
	struct hrtimer poll_timer;
};

static struct marie_audio *g_marie_audio;
static struct kobject *marie_audio_kobj;

/* ---------- SPMI & SoCCODEC Register Helpers ---------- */

static u8 marie_pmic_read(struct marie_audio *ma, u16 reg)
{
	u8 val = 0;

	if (ma->pmic)
		spmi_ext_register_readl(ma->pmic, reg, &val, 1);
	return val;
}

static void marie_pmic_write(struct marie_audio *ma, u16 reg, u8 val)
{
	if (ma->pmic)
		spmi_ext_register_writel(ma->pmic, reg, &val, 1);
}

static void marie_pmic_update(struct marie_audio *ma, u16 reg, u8 mask, u8 val)
{
	u8 cur = marie_pmic_read(ma, reg);
	u8 nxt = (cur & ~mask) | (val & mask);

	if (nxt != cur)
		marie_pmic_write(ma, reg, nxt);
}

static u32 marie_sc_read(struct marie_audio *ma, u32 off)
{
	return readl(ma->soccodec + off);
}

static void marie_sc_write(struct marie_audio *ma, u32 off, u32 val)
{
	writel(val, ma->soccodec + off);
}

static void marie_sc_update(struct marie_audio *ma, u32 off, u32 mask, u32 val)
{
	u32 cur = marie_sc_read(ma, off);
	u32 nxt = (cur & ~mask) | (val & mask);

	if (nxt != cur)
		marie_sc_write(ma, off, nxt);
}

/* ---------- NXP TFA9874 SmartPA I2C (16-bit Big-Endian) ---------- */

static int tfa9874_read16(struct marie_audio *ma, u8 reg, u16 *out)
{
	u8 rx[2] = { 0, 0 };
	struct i2c_msg msgs[2];
	int ret;

	if (!ma->tfa_i2c)
		return -ENODEV;

	msgs[0].addr = ma->tfa_i2c->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &reg;

	msgs[1].addr = ma->tfa_i2c->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 2;
	msgs[1].buf = rx;

	ret = i2c_transfer(ma->tfa_i2c->adapter, msgs, 2);
	if (ret != 2)
		return ret < 0 ? ret : -EIO;

	*out = ((u16)rx[0] << 8) | rx[1];
	return 0;
}

static int tfa9874_write16(struct marie_audio *ma, u8 reg, u16 val)
{
	u8 buf[3] = { reg, (u8)(val >> 8), (u8)(val & 0xff) };
	struct i2c_msg msg = {
		.addr = ma->tfa_i2c ? ma->tfa_i2c->addr : 0x34,
		.flags = 0,
		.len = 3,
		.buf = buf,
	};
	int ret;

	if (!ma->tfa_i2c)
		return -ENODEV;

	ret = i2c_transfer(ma->tfa_i2c->adapter, &msg, 1);
	return (ret == 1) ? 0 : (ret < 0 ? ret : -EIO);
}

static int tfa9874_update16(struct marie_audio *ma, u8 reg, u16 mask, u16 val)
{
	u16 cur = 0;
	int ret;

	if (mask == 0xffff)
		return tfa9874_write16(ma, reg, val);

	ret = tfa9874_read16(ma, reg, &cur);
	if (ret)
		return ret;
	return tfa9874_write16(ma, reg, (cur & ~mask) | (val & mask));
}

struct tfa_reg_op {
	u8 reg;
	u16 mask;
	u16 val;
	u8 op; /* 0 = update/write, 4 = RXORW (read val-reg, xor mask, write reg), 5 = dummy read */
};

/* Exact Huawei marie factory INIT_CHIP sequence from /odm/etc/audio/customize/tfa9874_configs.xml */
static const struct tfa_reg_op marie_tfa_init_seq[] = {
	{ 0x0f, 0xffff, 0x5a6b, 0 },
	{ 0xa0, 0x005a, 0x00fb, 4 },
	{ 0x00, 0xffff, 0x0002, 0 }, /* soft reset */
	{ 0x02, 0xffff, 0x22c8, 0 },
	{ 0x52, 0xffff, 0x57dc, 0 },
	{ 0x53, 0xffff, 0x003e, 0 },
	{ 0x56, 0xffff, 0x0400, 0 },
	{ 0x58, 0xffff, 0x16a4, 0 },
	{ 0x61, 0xffff, 0x013c, 0 },
	{ 0x66, 0xffff, 0x0701, 0 },
	{ 0x6f, 0xffff, 0x00a5, 0 },
	{ 0x70, 0xffff, 0x87e0, 0 },
	{ 0x73, 0xffff, 0x0047, 0 },
	{ 0x74, 0xffff, 0x6e80, 0 },
	{ 0x75, 0xffff, 0x9020, 0 },
	{ 0x76, 0xffff, 0x7e68, 0 },
	{ 0x83, 0xffff, 0x0594, 0 },
	{ 0x84, 0xffff, 0x0001, 0 },
	{ 0x85, 0xffff, 0x0001, 0 },
	{ 0x88, 0xffff, 0x0000, 0 },
	{ 0xb0, 0xffff, 0x4c00, 0 },
	{ 0xc4, 0xffff, 0x2001, 0 },
	{ 0xd5, 0xffff, 0x0000, 0 },
	{ 0x64, 0xc000, 0x8000, 0 },
	{ 0x20, 0xffff, 0x2890, 0 },
	{ 0x22, 0xffff, 0x047c, 0 },
	{ 0x23, 0xffff, 0x000b, 0 },
	{ 0x58, 0xffff, 0x1614, 0 },
	{ 0x66, 0xffff, 0x0700, 0 },
	{ 0x68, 0xffff, 0x07cb, 0 },
	{ 0x80, 0xffff, 0x0000, 0 },
	{ 0x83, 0xffff, 0x0799, 0 },
	{ 0x84, 0xffff, 0x0081, 0 },
	{ 0x85, 0xffff, 0x0003, 0 },
	{ 0x88, 0xffff, 0x0002, 0 },
	{ 0xc4, 0xffff, 0x0001, 0 },
	{ 0x26, 0xffff, 0x0010, 0 },
	{ 0x27, 0xffff, 0x0001, 0 },
	{ 0x48, 0xffff, 0x0000, 0 },
	{ 0x10, 0xffff, 0x0000, 5 },
	{ 0x44, 0xffff, 0x001f, 0 },
};

static u16 marie_spk_vol_to_tfa_attn(int vol)
{
	/*
	 * TFA9874 reg 0x61[9:6]:
	 * 0x04 (0x0100) = factory max 17 dB, 0x0f (0x03c0) = minimum/mute (6 dB).
	 */
	int idx;

	if (vol <= 0)
		return 0x03c0U;
	idx = 15 - ((clamp(vol, 0, 100) * 11) / 100);
	idx = clamp(idx, 4, 15);
	return (u16)(idx << 6);
}

static void marie_find_smartpa_i2c(struct marie_audio *ma);

static int tfa9874_init_chip(struct marie_audio *ma)
{
	size_t i;
	u16 tmp = 0;
	int ret;

	if (!ma->tfa_i2c)
		marie_find_smartpa_i2c(ma);
	if (!ma->tfa_i2c)
		return -ENODEV;

	ret = tfa9874_read16(ma, 0x03, &ma->tfa_rev);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(marie_tfa_init_seq); i++) {
		const struct tfa_reg_op *op = &marie_tfa_init_seq[i];

		if (op->op == 4) {
			ret = tfa9874_read16(ma, (u8)op->val, &tmp);
			if (!ret)
				ret = tfa9874_write16(ma, op->reg, tmp ^ op->mask);
		} else if (op->op == 5) {
			ret = tfa9874_read16(ma, op->reg, &tmp);
		} else {
			ret = tfa9874_update16(ma, op->reg, op->mask, op->val);
			if (op->reg == 0x00 && op->val == 0x0002)
				usleep_range(1000, 2000);
		}
		if (ret) {
			dev_warn(ma->dev, "TFA9874 init step %zu (reg 0x%02x) err %d\n",
				 i, op->reg, ret);
			return ret;
		}
	}

	ma->tfa_inited = true;
	dev_info(ma->dev, "NXP TFA9874 initialized (rev=0x%04x)\n", ma->tfa_rev);
	return 0;
}

static void tfa9874_power_set(struct marie_audio *ma, bool on)
{
	if (!ma->tfa_i2c)
		marie_find_smartpa_i2c(ma);
	if (!ma->tfa_i2c)
		return;

	if (on) {
		if (!ma->tfa_inited && tfa9874_init_chip(ma))
			return;
		tfa9874_update16(ma, 0x61, 0x03c0, marie_spk_vol_to_tfa_attn(ma->spk_vol));
		tfa9874_update16(ma, 0x22, 0x0600, 0x0400);
		tfa9874_update16(ma, 0x00, 0x0008, 0x0008);
		tfa9874_update16(ma, 0x00, 0x0010, 0x0010);
		tfa9874_update16(ma, 0x00, 0x0001, 0x0000); /* PWDN = 0 */
		tfa9874_update16(ma, 0x01, 0x0014, 0x0014); /* MANCOLD | MANSCONF */
		tfa9874_write16(ma, 0x48, 0x001d);
		ma->tfa_powered = true;
	} else if (ma->tfa_powered) {
		tfa9874_update16(ma, 0x61, 0x03c0, 0x03c0);
		msleep(10);
		tfa9874_write16(ma, 0x48, 0x0000);
		tfa9874_update16(ma, 0x00, 0x0008, 0x0000);
		tfa9874_update16(ma, 0x00, 0x0010, 0x0000);
		tfa9874_update16(ma, 0x00, 0x0001, 0x0001); /* PWDN = 1 */
		tfa9874_update16(ma, 0x22, 0x0600, 0x0400);
		tfa9874_update16(ma, 0x01, 0x0014, 0x0000);
		ma->tfa_powered = false;
	}
}

/* ---------- HI6555v2 Digital & Analog Routing ---------- */

static void marie_codec_hw_reset_init(struct marie_audio *ma)
{
	/* Reset PMIC codec */
	marie_pmic_write(ma, PMIC_SOFT_RST_REG, 0x22);
	msleep(10);
	marie_pmic_write(ma, PMIC_SOFT_RST_REG, 0x2f);
	marie_pmic_write(ma, PMIC_ANA_RW40_REG, 0x75);

	/* Release ASPCFG codec + ASP_DMAC + SRC + OCRAM + SIO + TIMER resets & gate clocks */
	writel(BIT(0), ma->aspcfg + ASPCFG_RST_CTRLEN);
	udelay(10);
	writel(BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(6) | BIT(8) |
	       BIT(9) | BIT(10) | BIT(11) | BIT(13) | BIT(14) | BIT(17) | BIT(18),
	       ma->aspcfg + ASPCFG_RST_CTRLDIS);
	writel(BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(9) |
	       BIT(11) | BIT(12) | BIT(13) | BIT(14) | BIT(16) | BIT(17) |
	       BIT(22) | BIT(23) | BIT(25),
	       ma->aspcfg + ASPCFG_GATE_EN);
	writel(readl(ma->aspcfg + ASPCFG_CODEC_DMA_SEL) | BIT(0),
	       ma->aspcfg + ASPCFG_CODEC_DMA_SEL);

	/* Release SoCCODEC global soft reset */
	marie_sc_write(ma, SC_SW_RST_N_REG, 0x1);
	ma->sc_version = marie_sc_read(ma, SC_VERSION_REG);
	ma->sc_i2s1_ctrl0_boot = marie_sc_read(ma, SC_I2S1_TDM_CTRL0_REG);

	/* Mute all DACL/DACR mixer inputs initially */
	marie_sc_update(ma, SC_DACL_MIXER4_CTRL0_REG, 0x00f00000U, 0x00f00000U);
	marie_sc_update(ma, SC_DACR_MIXER4_CTRL0_REG, 0x00f00000U, 0x00f00000U);

	/* Factory SRC & Sample-Rate defaults from mixer_paths_MAR.xml */
	marie_sc_update(ma, SC_SRCUP_CTRL_REG,
			(7U << 24) | (7U << 20) | (7U << 16) | (7U << 12),
			(4U << 24) | (4U << 20) | (1U << 16) | (1U << 12));
	marie_sc_update(ma, SC_FS_CTRL0_REG,
			BIT(29) | BIT(28) | (3U << 11) | (3U << 9) | (3U << 7) | (3U << 5),
			(3U << 11) | (3U << 9) | (3U << 7) | (3U << 5));

	/* Audio DL/UL PGA 0 dB, un-bypassed */
	marie_sc_update(ma, SC_AUDIO_L_DN_PGA_REG, (0xffU << 24) | BIT(10), 0);
	marie_sc_update(ma, SC_AUDIO_R_DN_PGA_REG, (0xffU << 24) | BIT(10), 0);
	marie_sc_update(ma, SC_AUDIO_L_UP_PGA_REG, (0xffU << 24) | BIT(10), 0);
	marie_sc_update(ma, SC_AUDIO_R_UP_PGA_REG, (0xffU << 24) | BIT(10), 0);

	/* MBHC 3.5mm headset jack detect init (hi6xxx_mbhc_reg_init) */
	marie_pmic_write(ma, PMIC_ANA_IRQM_REG0, 0xff);
	marie_pmic_write(ma, PMIC_ANA_IRQ_REG0, 0xff);
	marie_pmic_write(ma, PMIC_ANA_RW31_REG, 0x01); /* Schmitt HSD enable */
	marie_pmic_write(ma, PMIC_DEB_HS_MIC_REG, 0x14);
	marie_pmic_write(ma, PMIC_ANA_RW33_REG, 0x9e);
	marie_pmic_write(ma, PMIC_ANA_RW28_REG, 0x0b); /* HSMICBIAS 2.7V */
	marie_pmic_write(ma, PMIC_ANA_RW29_REG, 0x0b); /* MICBIAS1 2.7V */
	marie_pmic_write(ma, PMIC_ANA_RW20_REG, 0x00);
	marie_pmic_write(ma, PMIC_ANA_RW21_REG, 0x00);
	marie_pmic_write(ma, PMIC_ANA_RW37_REG, 0x1a); /* Charge Pump 768kHz */
	marie_pmic_update(ma, PMIC_ANA_RW8_REG, BIT(0), 0);
	marie_pmic_update(ma, PMIC_CLASSD_CTRL0_REG, BIT(2), BIT(2));
}

static void marie_pmic_ibias_update(struct marie_audio *ma, bool want_on)
{
	if (want_on) {
		marie_pmic_update(ma, PMIC_ANA_RW12_REG, BIT(6), BIT(6));
		if (!ma->cp_active)
			marie_pmic_write(ma, PMIC_ANA_RW3_REG, 0x77);
		marie_pmic_write(ma, PMIC_ANA_RW22_REG, 0x05);
		marie_pmic_write(ma, PMIC_ANA_RW23_REG, 0x3c);
		if (marie_pmic_read(ma, PMIC_ANA_RW1_REG) != 0x01) {
			marie_pmic_write(ma, PMIC_ANA_RW1_REG, 0x07);
			msleep(6);
			marie_pmic_write(ma, PMIC_ANA_RW1_REG, 0x03);
			marie_pmic_write(ma, PMIC_ANA_RW1_REG, 0x01);
		}
	} else if (!ma->pb_active && !ma->cp_active) {
		marie_pmic_write(ma, PMIC_ANA_RW1_REG, 0x04);
	}
}

static void marie_apply_routes_locked(struct marie_audio *ma)
{
	bool need_spk = ma->pb_active &&
			(ma->out_route == MARIE_OUT_SPEAKER || ma->out_route == MARIE_OUT_SPK_HP);
	bool need_hp  = ma->pb_active &&
			(ma->out_route == MARIE_OUT_HEADPHONES || ma->out_route == MARIE_OUT_SPK_HP);
	bool need_ear = ma->pb_active && (ma->out_route == MARIE_OUT_EARPIECE);
	bool need_ana_dl = need_hp || need_ear;
	u32 playback_gain = need_spk ? FIELD_PREP(SC_MIXER4_IN3_GAIN, 3) : 0;

	/* 1. Turn off unused analog/SmartPA outputs first (pop-free) */
	if (!need_spk && ma->tfa_powered) {
		tfa9874_power_set(ma, false);
		marie_sc_update(ma, SC_I2S1_TDM_CTRL0_REG, BIT(16) | BIT(17), 0);
		marie_sc_update(ma, SC_CLK_EN0_REG, BIT(0) | BIT(1), 0);
		marie_sc_update(ma, SC_CLK_EN1_REG, BIT(30), 0);
	}
	if (!need_hp && ma->hp_powered) {
		marie_pmic_write(ma, PMIC_ANA_RW5_REG, 0x40);
		marie_pmic_write(ma, PMIC_ANA_RW5_REG, 0x4c);
		msleep(20);
		marie_pmic_write(ma, PMIC_ANA_RW5_REG, 0x7c);
		marie_pmic_write(ma, PMIC_ANA_RW19_REG, 0x8c);
		marie_pmic_write(ma, PMIC_ANA_RW5_REG, 0xfc);
		marie_pmic_write(ma, PMIC_ANA_RW24_REG, 0x36);
		marie_pmic_write(ma, PMIC_ANA_RW20_REG, 0x00);
		ma->hp_powered = false;
	}
	if (!need_ear && ma->ear_powered) {
		marie_pmic_update(ma, PMIC_ANA_RW6_REG, 0x0f, 0x0f);
		marie_pmic_update(ma, PMIC_ANA_RW25_REG, 0xcc, 0x00);
		ma->ear_powered = false;
	}
	if (!need_ana_dl) {
		marie_pmic_update(ma, PMIC_DAC_CHAN_CTRL_REG, 0x03, 0x00);
		marie_pmic_update(ma, PMIC_ANA_RW4_REG, 0x03, 0x03);
		marie_pmic_update(ma, PMIC_CLK_EN_CFG_REG, 0x03, 0x00);
		marie_sc_update(ma, SC_DAC_FILTER_CTRL_REG,
				BIT(10) | BIT(11) | BIT(16) | BIT(18), 0);
		marie_sc_update(ma, SC_CLK_EN0_REG, 0xff000000U, 0);
	}

	/* 2. Downlink digital core */
	if (ma->pb_active) {
		marie_sc_update(ma, SC_CLK_EN0_REG, BIT(3) | BIT(4), 0);
		udelay(5);
		marie_sc_update(ma, SC_CLK_EN0_REG,
				BIT(3) | BIT(4) | BIT(19) | BIT(20) | BIT(22) | BIT(23),
				BIT(3) | BIT(4) | BIT(19) | BIT(20) | BIT(22) | BIT(23));
		/* Enable input 3 at Huawei's gain for the SmartPA media route. */
		marie_sc_update(ma, SC_DACL_MIXER4_CTRL0_REG,
				SC_MIXER4_INPUT_MUTES | SC_MIXER4_IN3_GAIN,
				(SC_MIXER4_INPUT_MUTES & ~SC_MIXER4_IN3_MUTE) |
				playback_gain);
		marie_sc_update(ma, SC_DACR_MIXER4_CTRL0_REG,
				SC_MIXER4_INPUT_MUTES | SC_MIXER4_IN3_GAIN,
				(SC_MIXER4_INPUT_MUTES & ~SC_MIXER4_IN3_MUTE) |
				playback_gain);
		marie_sc_update(ma, SC_CODEC_DIN_MUX_REG, (3U << 28) | (3U << 26), 0);
	} else {
		marie_sc_update(ma, SC_DACL_MIXER4_CTRL0_REG, 0x00f00000U, 0x00f00000U);
		marie_sc_update(ma, SC_DACR_MIXER4_CTRL0_REG, 0x00f00000U, 0x00f00000U);
		marie_sc_update(ma, SC_CLK_EN0_REG,
				BIT(3) | BIT(4) | BIT(19) | BIT(20) | BIT(22) | BIT(23), 0);
	}

	/* 3. Enable Speaker (I2S1 -> NXP TFA9874) */
	if (need_spk) {
		/* Route DACL_MIXER/DACR_MIXER to I2S1 PA L/R mux (bits 31 & 30 = 1) */
		marie_sc_update(ma, SC_CODEC_DIN_MUX_REG, BIT(31) | BIT(30), BIT(31) | BIT(30));
		/* Standard I2S mode (TDM_MODE = 0) */
		marie_sc_update(ma, SC_I2S1_TDM_CTRL1_REG, BIT(18), 0);
		/* Enable I2S1 clock + SmartPA feedback AFIFO + I2S1 TX/RX */
		marie_sc_update(ma, SC_CLK_EN1_REG, BIT(30), BIT(30));
		marie_sc_update(ma, SC_CLK_EN0_REG, BIT(0) | BIT(1), BIT(0) | BIT(1));
		/* Use the stock SmartPA transmit clock and feedback word length. */
		marie_sc_update(ma, SC_I2S1_TDM_CTRL0_REG,
				BIT(31) | SC_I2S1_TX_CLK_SEL | SC_I2S1_IO_WORD_LENGTH |
				GENMASK(23, 16),
				SC_I2S1_TX_CLK_SEL | FIELD_PREP(SC_I2S1_IO_WORD_LENGTH, 3) |
				BIT(17) | BIT(16));
		usleep_range(1000, 2000);
		if (!ma->tfa_powered)
			tfa9874_power_set(ma, true);
		else
			tfa9874_update16(ma, 0x61, 0x03c0, marie_spk_vol_to_tfa_attn(ma->spk_vol));
	}

	/* 4. Enable PMIC Analog Downlink (Headphones / Earpiece via SIF) */
	if (need_ana_dl) {
		marie_pmic_ibias_update(ma, true);
		marie_sc_update(ma, SC_CLK_EN1_REG, BIT(17), BIT(17)); /* SIF_MST_CLKEN */
		marie_sc_update(ma, SC_CLK_EN0_REG, 0xff000000U, 0xff000000U);
		marie_sc_update(ma, SC_DAC_FILTER_CTRL_REG,
				BIT(10) | BIT(11) | BIT(16) | BIT(18),
				BIT(10) | BIT(11) | BIT(16) | BIT(18));

		marie_pmic_update(ma, PMIC_CLK_EN_CFG_REG,
				  BIT(6) | BIT(5) | BIT(1) | BIT(0),
				  BIT(6) | BIT(5) | BIT(1) | BIT(0));
		marie_pmic_update(ma, PMIC_DAC_CHAN_CTRL_REG, 0x3f, 0x03); /* SIFL/SIFR + DWA */
		marie_pmic_update(ma, PMIC_ANA_RW4_REG, 0x03, 0x00);       /* DAPL/DAPR PU */

		if (need_hp) {
			u8 g = (u8)(((ma->hp_vol & 0xf) << 4) | (ma->hp_vol & 0xf));

			marie_pmic_write(ma, PMIC_ANA_RW21_REG, g);
			if (!ma->hp_powered) {
				marie_pmic_write(ma, PMIC_ANA_RW20_REG, 0x88);
				marie_pmic_write(ma, PMIC_ANA_RW24_REG, 0x36);
				marie_pmic_write(ma, PMIC_ANA_RW19_REG, 0x8c);
				marie_pmic_write(ma, PMIC_ANA_RW5_REG, 0x7c);
				msleep(5);
				marie_pmic_write(ma, PMIC_ANA_RW19_REG, 0xcc);
				marie_pmic_write(ma, PMIC_ANA_RW5_REG, 0x4c);
				marie_pmic_write(ma, PMIC_ANA_RW24_REG, 0x16);
				marie_pmic_write(ma, PMIC_ANA_RW5_REG, 0x40);
				msleep(60);
				marie_pmic_write(ma, PMIC_ANA_RW5_REG, 0x00);
				ma->hp_powered = true;
			}
		}

		if (need_ear) {
			marie_pmic_update(ma, PMIC_ANA_RW26_REG, 0x0f, (u8)(ma->ear_vol & 0xf));
			marie_pmic_update(ma, PMIC_ANA_RW25_REG, 0xcc, 0xc0);
			marie_pmic_update(ma, PMIC_ANA_RW6_REG, 0x0f, 0x00);
			ma->ear_powered = true;
		}
	}

	/* 5. Uplink Capture Path (Main Mic / Headset Mic -> ADC1/ADC2 -> SIF -> AUDIO_UL/UR) */
	if (ma->cp_active) {
		u8 mic_sel = (ma->in_route == MARIE_IN_HEADSET_MIC) ? (2U << 4) : (1U << 4);

		marie_pmic_ibias_update(ma, true);
		marie_pmic_update(ma, PMIC_CLK_EN_CFG_REG,
				  BIT(6) | BIT(5) | BIT(3) | BIT(2),
				  BIT(6) | BIT(5) | BIT(3) | BIT(2));

		if (ma->in_route == MARIE_IN_HEADSET_MIC)
			marie_pmic_update(ma, PMIC_ANA_RW7_REG, BIT(6) | BIT(2), 0);
		else
			marie_pmic_update(ma, PMIC_ANA_RW7_REG, BIT(5) | BIT(1), 0);

		marie_pmic_write(ma, PMIC_ANA_RW9_REG, mic_sel | BIT(3) | (u8)(ma->mic_vol & 0x7));
		marie_pmic_write(ma, PMIC_ANA_RW10_REG, BIT(3) | (u8)(ma->mic_vol & 0x7));
		marie_pmic_update(ma, PMIC_ANA_RW2_REG, BIT(6) | BIT(5) | BIT(2) | BIT(1), 0);
		marie_pmic_update(ma, PMIC_ANA_RW12_REG, 0x33, BIT(0)); /* ADCL <- MAIN_MIC */
		marie_pmic_update(ma, PMIC_ANA_RW14_REG, 0x33, BIT(1)); /* ADCR <- MAIN_MIC */
		marie_pmic_update(ma, PMIC_ANA_RW3_REG, BIT(6) | BIT(5) | BIT(2) | BIT(1), 0);
		marie_pmic_update(ma, PMIC_ADC_CHAN_CTRL_REG, 0x0f, 0x00); /* ADC1/ADC2 -> SIF */

		marie_sc_update(ma, SC_CLK_EN1_REG,
				BIT(17) | BIT(18) | BIT(19) | BIT(22) | BIT(23) | BIT(24) | BIT(25),
				BIT(17) | BIT(18) | BIT(19) | BIT(22) | BIT(23) | BIT(24) | BIT(25));
		marie_sc_update(ma, SC_DAC_FILTER_CTRL_REG, BIT(8) | BIT(9), BIT(8) | BIT(9));
		if ((marie_sc_read(ma, SC_ADC_FILTER_CTRL_REG) & (0x3fU << 26)) == 0)
			marie_sc_update(ma, SC_ADC_FILTER_CTRL_REG,
					(0x3fU << 26) | (0x3fU << 16),
					(18U << 26) | (18U << 16));
		/* ADCL_DIN=ADC1(0), ADCR_DIN=ADC2(1), AUDIO_UL_MUX=ADCL(1), AUDIO_UR_MUX=ADCR(1) */
		marie_sc_update(ma, SC_CODEC_DIN_MUX_REG,
				(3U << 22) | (3U << 20) | (3U << 13) | (3U << 11),
				(1U << 22) | (1U << 20) | (0U << 13) | (1U << 11));
		marie_sc_update(ma, SC_CLK_EN0_REG, BIT(5) | BIT(6), 0);
		udelay(5);
		marie_sc_update(ma, SC_CLK_EN0_REG, BIT(5) | BIT(6), BIT(5) | BIT(6));
	} else {
		marie_sc_update(ma, SC_CLK_EN0_REG, BIT(5) | BIT(6), 0);
		marie_sc_update(ma, SC_DAC_FILTER_CTRL_REG, BIT(8) | BIT(9), 0);
		marie_sc_update(ma, SC_CLK_EN1_REG,
				BIT(18) | BIT(19) | BIT(22) | BIT(23) | BIT(24) | BIT(25), 0);
		if (!need_ana_dl)
			marie_sc_update(ma, SC_CLK_EN1_REG, BIT(17), 0);

		marie_pmic_update(ma, PMIC_ANA_RW3_REG,
				  BIT(6) | BIT(5) | BIT(2) | BIT(1),
				  BIT(6) | BIT(5) | BIT(2) | BIT(1));
		marie_pmic_update(ma, PMIC_ANA_RW2_REG,
				  BIT(6) | BIT(5) | BIT(2) | BIT(1),
				  BIT(6) | BIT(5) | BIT(2) | BIT(1));
		marie_pmic_update(ma, PMIC_ANA_RW7_REG, BIT(6) | BIT(5), BIT(6) | BIT(5));
		marie_pmic_update(ma, PMIC_CLK_EN_CFG_REG, BIT(3) | BIT(2), 0);
		if (!need_ana_dl)
			marie_pmic_update(ma, PMIC_CLK_EN_CFG_REG, BIT(6) | BIT(5), 0);
	}

	if (!need_ana_dl && !ma->cp_active)
		marie_pmic_ibias_update(ma, false);
}

static void marie_route_work_fn(struct work_struct *work)
{
	struct marie_audio *ma = container_of(work, struct marie_audio, route_work.work);

	mutex_lock(&ma->route_lock);
	marie_apply_routes_locked(ma);
	mutex_unlock(&ma->route_lock);
}

/* ---------- HiFi3 Shared-Memory Mailbox Engine ---------- */

static inline u32 marie_get_slice(struct marie_audio *ma)
{
	return ma->sctrl ? readl(ma->sctrl + 0x534U) : (u32)jiffies;
}

static void hifi_mb_ring_write(u8 *base, u32 ring_len, u32 *front, const void *src, u32 len)
{
	u32 to_end = ring_len - *front;

	if (to_end > len) {
		memcpy(base + *front, src, len);
		*front += len;
	} else {
		memcpy(base + *front, src, to_end);
		memcpy(base, (const u8 *)src + to_end, len - to_end);
		*front = len - to_end;
	}
}

static void hifi_mb_ring_read(const u8 *base, u32 ring_len, u32 *rear, void *dst, u32 len)
{
	u32 to_end = ring_len - *rear;

	if (to_end > len) {
		memcpy(dst, base + *rear, len);
		*rear += len;
	} else {
		memcpy(dst, base + *rear, to_end);
		memcpy((u8 *)dst + to_end, base, len - to_end);
		*rear = len - to_end;
	}
}

static void hifi_mb_init_queue(u8 *head_ptr, u8 *data_ptr)
{
	struct hifi_mb_head *head = (struct hifi_mb_head *)head_ptr;
	u32 *words = (u32 *)data_ptr;
	u32 nwords = MB_QUEUE_RAW_SIZE / 4U;

	if (head->protect1 == MB_PROTECT1 && head->protect2 == MB_PROTECT2 &&
	    head->protect3 == MB_PROTECT1 && head->protect4 == MB_PROTECT2 &&
	    head->front < (MB_QUEUE_USABLE_SIZE / 4U) &&
	    head->rear < (MB_QUEUE_USABLE_SIZE / 4U))
		return;

	memset(data_ptr, 0, MB_QUEUE_RAW_SIZE);
	words[0] = MB_PROTECT1;
	words[1] = MB_PROTECT2;
	words[nwords - 2] = MB_PROTECT1;
	words[nwords - 1] = MB_PROTECT2;

	memset(head, 0, sizeof(*head));
	head->protect1 = MB_PROTECT1;
	head->protect2 = MB_PROTECT2;
	head->protect3 = MB_PROTECT1;
	head->protect4 = MB_PROTECT2;
	dsb(sy);
}

static int marie_hifi_send_mail(struct marie_audio *ma, u32 mailcode,
				const void *payload, u32 len)
{
	struct hifi_mb_head *head = (struct hifi_mb_head *)(ma->hifi_mem + MB_TX_HEAD_OFF);
	u8 *ring_base = ma->hifi_mem + MB_TX_DATA_OFF + 8U;
	struct hifi_mb_mail mail;
	u32 front, rear, used, free_bytes, total_aligned;
	unsigned long tx_flags;
	int ret;

	if (!ma->hifi_mem || !ma->tx_chan)
		return -ENODEV;

	spin_lock_irqsave(&ma->mb_tx_lock, tx_flags);
	hifi_mb_init_queue((u8 *)head, ma->hifi_mem + MB_TX_DATA_OFF);

	front = (READ_ONCE(head->front) * 4U) % MB_QUEUE_USABLE_SIZE;
	rear  = (READ_ONCE(head->rear) * 4U) % MB_QUEUE_USABLE_SIZE;
	used  = (front >= rear) ? (front - rear) : (MB_QUEUE_USABLE_SIZE - (rear - front));
	free_bytes = (MB_QUEUE_USABLE_SIZE > used + 4U) ? (MB_QUEUE_USABLE_SIZE - used - 4U) : 0;

	total_aligned = sizeof(mail) + ALIGN(len, 4U);
	if (free_bytes < total_aligned) {
		spin_unlock_irqrestore(&ma->mb_tx_lock, tx_flags);
		return -EBUSY;
	}

	memset(&mail, 0, sizeof(mail));
	mail.partition = MB_MSGHEAD_MAGIC;
	mail.write_slice = marie_get_slice(ma);
	mail.seq_num = ma->tx_seq++;
	mail.mail_code = mailcode;
	mail.msg_length = len;

	hifi_mb_ring_write(ring_base, MB_QUEUE_USABLE_SIZE, &front, &mail, sizeof(mail));
	hifi_mb_ring_write(ring_base, MB_QUEUE_USABLE_SIZE, &front, payload, len);
	front = ALIGN(front, 4U) % MB_QUEUE_USABLE_SIZE;

	dsb(sy);
	WRITE_ONCE(head->front, front / 4U);
	WRITE_ONCE(head->front_slice, mail.write_slice);
	dsb(sy);

	/* IPC_CORE_ACPU (0) << 24 | IPC_HIFI_INT_SRC_ACPU_MSG (0) << 8 */
	memset(ma->tx_ipc_buf, 0, sizeof(ma->tx_ipc_buf));
	ret = mbox_send_message(ma->tx_chan, ma->tx_ipc_buf);
	atomic_inc(&ma->tx_mail_cnt);
	spin_unlock_irqrestore(&ma->mb_tx_lock, tx_flags);

	return ret < 0 ? ret : 0;
}

static void marie_hifi_sync_routes(struct marie_audio *ma)
{
	struct {
		u16 msg_id;
		u16 reserved;
		u32 val;
	} __packed cmd8;
	struct {
		u16 msg_id;
		u16 reserved;
		u32 dev;
		u32 extra;
	} __packed out_cmd;
	u32 out_dev = 0x2U; /* AUDIO_DEVICE_OUT_SPEAKER */
	u32 in_dev = (ma->in_route == MARIE_IN_HEADSET_MIC) ? 0x80000010U : 0x80000004U;

	if (!ma->hifi_woken)
		return;

	if (ma->out_route == MARIE_OUT_HEADPHONES)
		out_dev = 0x8U; /* AUDIO_DEVICE_OUT_WIRED_HEADPHONE */
	else if (ma->out_route == MARIE_OUT_EARPIECE)
		out_dev = 0x1U; /* AUDIO_DEVICE_OUT_EARPIECE */
	else if (ma->out_route == MARIE_OUT_SPK_HP)
		out_dev = 0xaU;

	cmd8.msg_id = 0xddc0U; /* soc_dsp_notify_routing_status = start(0) */
	cmd8.reserved = 0;
	cmd8.val = 0U;
	marie_hifi_send_mail(ma, MAILCODE_ACPU_TO_HIFI_MISC, &cmd8, sizeof(cmd8));

	out_cmd.msg_id = 0xdd96U; /* set output device */
	out_cmd.reserved = 0;
	out_cmd.dev = out_dev;
	out_cmd.extra = 0U;
	marie_hifi_send_mail(ma, MAILCODE_ACPU_TO_HIFI_MISC, &out_cmd, sizeof(out_cmd));

	cmd8.msg_id = 0xdd97U; /* set input device */
	cmd8.val = in_dev;
	marie_hifi_send_mail(ma, MAILCODE_ACPU_TO_HIFI_MISC, &cmd8, sizeof(cmd8));

	cmd8.msg_id = 0xddc0U; /* soc_dsp_notify_routing_status = complete(1) */
	cmd8.val = 1U;
	marie_hifi_send_mail(ma, MAILCODE_ACPU_TO_HIFI_MISC, &cmd8, sizeof(cmd8));
}

static void marie_pcm_advance_period(struct marie_audio *ma, int stream, bool from_hifi);

static void marie_hifi_drain_rx(struct marie_audio *ma)
{
	struct hifi_mb_head *head;
	u8 *ring_base;
	unsigned long flags;
	int guard = 32;

	if (!ma->hifi_mem)
		return;

	head = (struct hifi_mb_head *)(ma->hifi_mem + MB_RX_HEAD_OFF);
	ring_base = ma->hifi_mem + MB_RX_DATA_OFF + 8U;

	spin_lock_irqsave(&ma->mb_rx_lock, flags);
	if (head->protect1 != MB_PROTECT1 || head->protect2 != MB_PROTECT2) {
		spin_unlock_irqrestore(&ma->mb_rx_lock, flags);
		return;
	}

	while (guard-- > 0) {
		u32 front = READ_ONCE(head->front) * 4U;
		u32 rear  = READ_ONCE(head->rear) * 4U;
		struct hifi_mb_mail mail;
		u32 msg_rear;
		u8 body[64];
		u32 copy_len;

		if (front >= MB_QUEUE_USABLE_SIZE || rear >= MB_QUEUE_USABLE_SIZE || front == rear)
			break;

		hifi_mb_ring_read(ring_base, MB_QUEUE_USABLE_SIZE, &rear, &mail, sizeof(mail));
		if (mail.partition != MB_MSGHEAD_MAGIC || mail.msg_length > MB_QUEUE_USABLE_SIZE - sizeof(mail)) {
			/* Resync corrupted queue */
			WRITE_ONCE(head->rear, front / 4U);
			dsb(sy);
			break;
		}

		msg_rear = rear;
		copy_len = min_t(u32, mail.msg_length, sizeof(body));
		if (copy_len)
			hifi_mb_ring_read(ring_base, MB_QUEUE_USABLE_SIZE, &msg_rear, body, copy_len);

		rear = (rear + ALIGN(mail.msg_length, 4U)) % MB_QUEUE_USABLE_SIZE;
		WRITE_ONCE(head->rear, rear / 4U);
		WRITE_ONCE(head->rear_slice, marie_get_slice(ma));
		dsb(sy);
		atomic_inc(&ma->rx_mail_cnt);

		if (copy_len >= sizeof(u16)) {
			u16 rx_id = *(const u16 *)body;

			ma->last_rx_msg = rx_id;
			if (rx_id == 0xddc1U) {
				struct {
					u16 msg_id;
					u16 reserved;
					u32 val;
				} __packed dp_ack = {
					.msg_id = 0xddc2U,
					.reserved = 0,
					.val = 1U,
				};

				spin_unlock_irqrestore(&ma->mb_rx_lock, flags);
				marie_hifi_send_mail(ma, MAILCODE_ACPU_TO_HIFI_MISC,
						     &dp_ack, sizeof(dp_ack));
				spin_lock_irqsave(&ma->mb_rx_lock, flags);
			}
		}

		if (mail.mail_code == MAILCODE_HIFI_TO_ACPU_AUDIO &&
		    copy_len >= sizeof(u16) * 4) {
			const struct hifi_chn_period_elapsed *pe =
				(const struct hifi_chn_period_elapsed *)body;

			if (pe->msg_type == HI_CHN_MSG_PCM_PERIOD_ELAPSED &&
			    pe->pcm_mode < 2) {
				spin_unlock_irqrestore(&ma->mb_rx_lock, flags);
				marie_pcm_advance_period(ma, pe->pcm_mode, true);
				spin_lock_irqsave(&ma->mb_rx_lock, flags);
			}
		}
	}
	spin_unlock_irqrestore(&ma->mb_rx_lock, flags);
}

static irqreturn_t marie_hifi_rx_isr(int irq, void *data)
{
	struct marie_audio *ma = data;
	void __iomem *mbx2 = ma->ipc_base + IPC_MBX_BASE(2);
	u32 imask, todo, status;

	atomic_inc(&ma->rx_irq_cnt);

	/* Ensure HiIPCV230 lock register is unlocked before clearing RX IRQ */
	if (readl(ma->ipc_base + 0xa00U))
		writel(0x1acce551U, ma->ipc_base + 0xa00U);

	/* Ack HiIPCV230 fast RX mailbox-2 exactly per hisi_mdev_clr_irq_and_ack */
	imask = readl(mbx2 + IPC_MBX_IMASK_REG);
	todo = (BIT(0) | BIT(1)) & (~imask);
	if (!todo)
		todo = BIT(0) | BIT(1);
	writel(0, mbx2 + IPC_MBX_DATA0_REG);
	writel(0, mbx2 + IPC_MBX_DATA0_REG + 4);
	writel(todo, mbx2 + IPC_MBX_ICLR_REG);
	status = readl(mbx2 + IPC_MBX_MODE_REG);
	if ((status & IPC_MODE_DEST_STAT) && !(status & IPC_MODE_AUTOACK))
		writel(todo, mbx2 + IPC_MBX_SEND_REG);

	marie_hifi_drain_rx(ma);
	return IRQ_HANDLED;
}

/* ---------- Direct Hardware ASP_DMAC <-> HI6555v2 AFIFO Engine ---------- */

static void marie_pb_sync_frames(struct marie_audio *ma, unsigned int start_frame,
				 unsigned int frame_cnt, unsigned int buf_frames)
{
	const s16 *src = (const s16 *)(ma->hifi_mem + PCM_PB_ALSA_OFF);
	u32 *dst_l = (u32 *)(ma->hifi_mem + DMAC_PB_L_OFF);
	u32 *dst_r = (u32 *)(ma->hifi_mem + DMAC_PB_R_OFF);
	unsigned int i;

	if (!buf_frames)
		return;

	for (i = 0; i < frame_cnt; i++) {
		unsigned int f = (start_frame + i) % buf_frames;
		u32 l = ((u32)(u16)src[2U * f]) << 16;
		u32 r = ((u32)(u16)src[2U * f + 1U]) << 16;

		WRITE_ONCE(dst_l[f], l);
		WRITE_ONCE(dst_r[f], r);
	}
	dsb(sy);
}

static void marie_cp_sync_frames(struct marie_audio *ma, unsigned int start_frame,
				 unsigned int frame_cnt, unsigned int buf_frames,
				 unsigned int channels)
{
	const u32 *src_l = (const u32 *)(ma->hifi_mem + DMAC_CP_L_OFF);
	const u32 *src_r = (const u32 *)(ma->hifi_mem + DMAC_CP_R_OFF);
	s16 *dst = (s16 *)(ma->hifi_mem + PCM_CP_ALSA_OFF);
	unsigned int i;

	if (!buf_frames)
		return;

	dsb(sy);
	for (i = 0; i < frame_cnt; i++) {
		unsigned int f = (start_frame + i) % buf_frames;
		s16 l = (s16)(READ_ONCE(src_l[f]) >> 16);

		if (channels >= 2U) {
			s16 r = (s16)(READ_ONCE(src_r[f]) >> 16);

			dst[2U * f] = l;
			dst[2U * f + 1U] = r;
		} else {
			dst[f] = l;
		}
	}
	dsb(sy);
}

static void marie_dmac_clear_irqs(struct marie_audio *ma, u32 ch_mask)
{
	writel(ch_mask, ma->dmac + ASP_DMA_INT_TC1_RAW);
	writel(ch_mask, ma->dmac + ASP_DMA_INT_TC2_RAW);
	writel(ch_mask, ma->dmac + ASP_DMA_INT_ERR1_RAW);
	writel(ch_mask, ma->dmac + ASP_DMA_INT_ERR2_RAW);
	writel(ch_mask, ma->dmac + ASP_DMA_INT_ERR3_RAW);
}

static void marie_dmac_stop_ch(struct marie_audio *ma, unsigned int ch)
{
	u32 mask = BIT(ch);
	u32 cfg;
	int tries = 50;

	if (!ma->dmac)
		return;

	cfg = readl(ma->dmac + ASP_DMA_CX_CONFIG(ch));
	if (cfg & BIT(0)) {
		writel(cfg & ~BIT(0), ma->dmac + ASP_DMA_CX_CONFIG(ch));
		while (tries-- > 0 && (readl(ma->dmac + ASP_DMA_CH_STAT) & mask))
			udelay(20);
	}
	writel(0, ma->dmac + ASP_DMA_CX_LLI(ch));
	marie_dmac_clear_irqs(ma, mask);
}

static void marie_dmac_stop_stream(struct marie_audio *ma, int stream)
{
	unsigned int ch0 = (stream == SNDRV_PCM_STREAM_PLAYBACK) ? DMAC_CH_PB_L : DMAC_CH_CP_L;
	unsigned int ch1 = (stream == SNDRV_PCM_STREAM_PLAYBACK) ? DMAC_CH_PB_R : DMAC_CH_CP_R;
	unsigned long lli_base_off = DMAC_LLI_OFF + (stream == SNDRV_PCM_STREAM_PLAYBACK ? 0UL : 0x400UL);
	struct marie_dma_lli *lli_l = (struct marie_dma_lli *)(ma->hifi_mem + lli_base_off);
	struct marie_dma_lli *lli_r = (struct marie_dma_lli *)(ma->hifi_mem + lli_base_off + 0x200UL);
	unsigned int k;

	for (k = 0; k < DMAC_MAX_PERIODS; k++) {
		WRITE_ONCE(lli_l[k].lli, 0U);
		WRITE_ONCE(lli_r[k].lli, 0U);
	}
	dsb(sy);

	marie_dmac_stop_ch(ma, ch0);
	marie_dmac_stop_ch(ma, ch1);
}

static void marie_dmac_start_stream(struct marie_audio *ma, int stream,
				    unsigned int period_frames, unsigned int periods)
{
	bool is_pb = (stream == SNDRV_PCM_STREAM_PLAYBACK);
	unsigned int ch[2] = {
		is_pb ? DMAC_CH_PB_L : DMAC_CH_CP_L,
		is_pb ? DMAC_CH_PB_R : DMAC_CH_CP_R,
	};
	u32 fifo_phys[2] = {
		is_pb ? SOCCODEC_PB_L_FIFO_PHYS : SOCCODEC_CP_L_FIFO_PHYS,
		is_pb ? SOCCODEC_PB_R_FIFO_PHYS : SOCCODEC_CP_R_FIFO_PHYS,
	};
	u32 cfg_val[2] = {
		is_pb ? DMAC_CFG_PB_L : DMAC_CFG_CP_L,
		is_pb ? DMAC_CFG_PB_R : DMAC_CFG_CP_R,
	};
	u32 buf_phys[2] = {
		(u32)(HIFI_UNSEC_BASE_ADDR + (is_pb ? DMAC_PB_L_OFF : DMAC_CP_L_OFF)),
		(u32)(HIFI_UNSEC_BASE_ADDR + (is_pb ? DMAC_PB_R_OFF : DMAC_CP_R_OFF)),
	};
	unsigned long lli_base_off = DMAC_LLI_OFF + (is_pb ? 0UL : 0x400UL);
	u32 pbytes32 = period_frames * 4U;
	unsigned int p, k;

	if (!periods || periods > DMAC_MAX_PERIODS || !period_frames)
		return;

	marie_dmac_stop_ch(ma, ch[0]);
	marie_dmac_stop_ch(ma, ch[1]);

	/* Ensure CODEC_DMA_SEL routes SoCCODEC AFIFO DREQs to ASP_DMAC */
	writel(readl(ma->aspcfg + ASPCFG_CODEC_DMA_SEL) | BIT(0),
	       ma->aspcfg + ASPCFG_CODEC_DMA_SEL);

	/* Clear HI6555v2 AFIFOs cleanly before arming DMA */
	if (is_pb) {
		marie_sc_update(ma, SC_AUDIO_DN_AFIFO_CTRL_REG, BIT(31) | BIT(15), BIT(31) | BIT(15));
		udelay(5);
		marie_sc_update(ma, SC_AUDIO_DN_AFIFO_CTRL_REG, BIT(31) | BIT(15), 0);
	} else {
		marie_sc_update(ma, SC_AUDIO_UP_AFIFO_CTRL_REG, BIT(31) | BIT(15), BIT(31) | BIT(15));
		udelay(5);
		marie_sc_update(ma, SC_AUDIO_UP_AFIFO_CTRL_REG, BIT(31) | BIT(15), 0);
	}

	for (p = 0; p < 2; p++) {
		unsigned long off = lli_base_off + p * 0x200UL;
		struct marie_dma_lli *ring = (struct marie_dma_lli *)(ma->hifi_mem + off);
		u32 ring_phys = (u32)(HIFI_UNSEC_BASE_ADDR + off);

		memset(ring, 0, sizeof(*ring) * DMAC_MAX_PERIODS);
		for (k = 0; k < periods; k++) {
			u32 next_phys = ring_phys + ((k + 1U) % periods) * sizeof(struct marie_dma_lli);
			u32 ddr_addr = buf_phys[p] + k * pbytes32;

			ring[k].lli = (next_phys & 0xffffffe0U) | 0x2U;
			ring[k].a_count = pbytes32;
			ring[k].src_addr = is_pb ? ddr_addr : fifo_phys[p];
			ring[k].des_addr = is_pb ? fifo_phys[p] : ddr_addr;
			ring[k].config = cfg_val[p];
		}
	}
	dsb(sy);

	for (p = 0; p < 2; p++) {
		unsigned long off = lli_base_off + p * 0x200UL;
		struct marie_dma_lli *lli0 = (struct marie_dma_lli *)(ma->hifi_mem + off);
		unsigned int c = ch[p];

		writel(lli0->a_count, ma->dmac + ASP_DMA_CX_CNT0(c));
		writel(0, ma->dmac + ASP_DMA_CX_CNT1(c));
		writel(0, ma->dmac + ASP_DMA_CX_BINDX(c));
		writel(0, ma->dmac + ASP_DMA_CX_CINDX(c));
		writel(lli0->src_addr, ma->dmac + ASP_DMA_CX_SRC(c));
		writel(lli0->des_addr, ma->dmac + ASP_DMA_CX_DES(c));
		writel(lli0->lli, ma->dmac + ASP_DMA_CX_LLI(c));
		marie_dmac_clear_irqs(ma, BIT(c));
	}
	dsb(sy);

	/* Start Left and Right DMA channels simultaneously */
	writel(cfg_val[0], ma->dmac + ASP_DMA_CX_CONFIG(ch[0]));
	writel(cfg_val[1], ma->dmac + ASP_DMA_CX_CONFIG(ch[1]));
}

static bool marie_dmac_read_hw_frame(struct marie_audio *ma, int stream,
				     unsigned int buf_frames, unsigned int *out_frame)
{
	bool is_pb = (stream == SNDRV_PCM_STREAM_PLAYBACK);
	u32 base = (u32)(HIFI_UNSEC_BASE_ADDR + (is_pb ? DMAC_PB_L_OFF : DMAC_CP_L_OFF));
	u32 span = buf_frames * 4U;
	u32 cur;

	if (!ma->dmac || !span)
		return false;

	cur = is_pb ? readl(ma->dmac + ASP_DMA_CX_CURR_SRC(DMAC_CH_PB_L))
		    : readl(ma->dmac + ASP_DMA_CX_CURR_DES(DMAC_CH_CP_L));
	if (cur >= base && cur < base + span) {
		*out_frame = (cur - base) / 4U;
		return true;
	}
	if (cur == base + span) {
		*out_frame = 0U;
		return true;
	}
	return false;
}

static void marie_pcm_advance_period(struct marie_audio *ma, int stream, bool from_hifi)
{
	struct marie_stream_rt *srt = &ma->streams[stream];
	struct snd_pcm_substream *substream;
	unsigned int done_idx = 0, pframes = 0, bframes = 0, chs = 2, periods = 0;
	unsigned long flags;
	bool do_elapsed = false;

	spin_lock_irqsave(&srt->lock, flags);
	substream = srt->substream;
	if (srt->running && substream && substream->runtime && srt->periods > 0) {
		if (from_hifi)
			srt->hifi_acked = true;
		srt->last_adv_ns = ktime_get_ns();
		done_idx = srt->period_cur;
		pframes = srt->period_frames;
		bframes = srt->buf_frames;
		chs = srt->channels;
		periods = srt->periods;
		srt->period_cur = (srt->period_cur + 1U) % srt->periods;
		srt->periods_elapsed++;
		do_elapsed = true;
	}
	spin_unlock_irqrestore(&srt->lock, flags);

	if (do_elapsed) {
		if (stream == SNDRV_PCM_STREAM_CAPTURE)
			marie_cp_sync_frames(ma, done_idx * pframes, pframes, bframes, chs);
		else if (periods > 1U)
			marie_pb_sync_frames(ma, ((done_idx + 2U) % periods) * pframes,
					     pframes, bframes);
		snd_pcm_period_elapsed(substream);
	}
}

static enum hrtimer_restart marie_poll_timer_fn(struct hrtimer *timer)
{
	struct marie_audio *ma = container_of(timer, struct marie_audio, poll_timer);
	bool any_running = false;
	u64 now_ns = ktime_get_ns();
	int s;

	for (s = 0; s < 2; s++) {
		struct marie_stream_rt *srt = &ma->streams[s];
		struct snd_pcm_substream *substream = NULL;
		unsigned int pframes = 0, bframes = 0, periods = 0, chs = 2;
		unsigned int hw_frame = 0, target_period = 0, guard = 0;
		bool got_hw = false, do_elapsed = false, fallback_tick = false;
		unsigned long flags;

		spin_lock_irqsave(&srt->lock, flags);
		if (srt->running && srt->substream && srt->substream->runtime &&
		    srt->periods > 0 && srt->period_frames > 0) {
			unsigned int rate = srt->substream->runtime->rate ?
					    srt->substream->runtime->rate : 48000U;
			u64 period_ns = div_u64((u64)srt->period_frames * 1000000000ULL, rate);

			any_running = true;
			substream = srt->substream;
			pframes = srt->period_frames;
			bframes = srt->buf_frames;
			periods = srt->periods;
			chs = srt->channels;

			if (marie_dmac_read_hw_frame(ma, s, bframes, &hw_frame)) {
				if (hw_frame != srt->last_hw_frame) {
					srt->last_hw_frame = hw_frame;
					srt->hifi_acked = true;
					srt->last_adv_ns = now_ns;
				}
				target_period = (hw_frame / pframes) % periods;
				got_hw = srt->hifi_acked;
			}

			if (!got_hw && period_ns && now_ns >= srt->last_adv_ns + period_ns + 6000000ULL)
				fallback_tick = true;
		}
		spin_unlock_irqrestore(&srt->lock, flags);

		if (got_hw && substream) {
			while (guard++ < periods) {
				unsigned int done_idx = 0;
				bool step = false;

				spin_lock_irqsave(&srt->lock, flags);
				if (srt->running && srt->period_cur != target_period) {
					done_idx = srt->period_cur;
					srt->period_cur = (srt->period_cur + 1U) % periods;
					srt->periods_elapsed++;
					step = true;
				}
				spin_unlock_irqrestore(&srt->lock, flags);

				if (!step)
					break;
				if (s == SNDRV_PCM_STREAM_CAPTURE)
					marie_cp_sync_frames(ma, done_idx * pframes, pframes,
							     bframes, chs);
				else if (periods > 1U)
					marie_pb_sync_frames(ma,
							     ((done_idx + 2U) % periods) * pframes,
							     pframes, bframes);
				do_elapsed = true;
			}
			if (do_elapsed)
				snd_pcm_period_elapsed(substream);
		} else if (fallback_tick) {
			marie_pcm_advance_period(ma, s, false);
		}
	}

	if (any_running) {
		hrtimer_forward_now(timer, ms_to_ktime(1));
		return HRTIMER_RESTART;
	}
	return HRTIMER_NORESTART;
}

/* ---------- ALSA PCM Operations ---------- */

static const struct snd_pcm_hardware marie_pcm_hw_pb = {
	.info			= SNDRV_PCM_INFO_INTERLEAVED |
				  SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_PAUSE,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE,
	.rates			= SNDRV_PCM_RATE_48000,
	.rate_min		= 48000,
	.rate_max		= 48000,
	.channels_min		= 2,
	.channels_max		= 2,
	.buffer_bytes_max	= PCM_PB_DMA_MAX_BYTES,
	.period_bytes_min	= 960,
	.period_bytes_max	= 16384,
	.periods_min		= 2,
	.periods_max		= DMAC_MAX_PERIODS,
	.fifo_size		= 32,
};

static const struct snd_pcm_hardware marie_pcm_hw_cp = {
	.info			= SNDRV_PCM_INFO_INTERLEAVED |
				  SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_MMAP_VALID,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE,
	.rates			= SNDRV_PCM_RATE_48000,
	.rate_min		= 48000,
	.rate_max		= 48000,
	.channels_min		= 1,
	.channels_max		= 2,
	.buffer_bytes_max	= PCM_CP_DMA_MAX_BYTES,
	.period_bytes_min	= 960,
	.period_bytes_max	= 16384,
	.periods_min		= 2,
	.periods_max		= DMAC_MAX_PERIODS,
	.fifo_size		= 32,
};

static int marie_pcm_open(struct snd_pcm_substream *substream)
{
	struct marie_audio *ma = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	int stream = substream->stream;
	struct marie_stream_rt *srt = &ma->streams[stream];
	unsigned long flags;

	runtime->hw = (stream == SNDRV_PCM_STREAM_PLAYBACK) ? marie_pcm_hw_pb : marie_pcm_hw_cp;
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);

	spin_lock_irqsave(&srt->lock, flags);
	srt->substream = substream;
	srt->running = false;
	srt->period_cur = 0;
	srt->period_next = 0;
	srt->last_hw_frame = 0;
	spin_unlock_irqrestore(&srt->lock, flags);

	return 0;
}

static int marie_pcm_close(struct snd_pcm_substream *substream)
{
	struct marie_audio *ma = substream->private_data;
	int stream = substream->stream;
	struct marie_stream_rt *srt = &ma->streams[stream];
	unsigned long flags;

	spin_lock_irqsave(&srt->lock, flags);
	srt->running = false;
	srt->substream = NULL;
	spin_unlock_irqrestore(&srt->lock, flags);

	marie_dmac_stop_stream(ma, stream);

	mutex_lock(&ma->route_lock);
	if (stream == SNDRV_PCM_STREAM_PLAYBACK)
		ma->pb_active = false;
	else
		ma->cp_active = false;
	marie_apply_routes_locked(ma);
	mutex_unlock(&ma->route_lock);

	return 0;
}

static int marie_pcm_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	struct marie_audio *ma = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	int stream = substream->stream;
	struct marie_stream_rt *srt = &ma->streams[stream];
	size_t bytes = params_buffer_bytes(params);
	dma_addr_t phys = (stream == SNDRV_PCM_STREAM_PLAYBACK) ? PCM_PB_DMA_PHYS : PCM_CP_DMA_PHYS;
	size_t max_bytes = (stream == SNDRV_PCM_STREAM_PLAYBACK) ? PCM_PB_DMA_MAX_BYTES : PCM_CP_DMA_MAX_BYTES;
	unsigned long flags;

	if (bytes > max_bytes || params_periods(params) > DMAC_MAX_PERIODS)
		return -EINVAL;

	runtime->dma_area = ma->hifi_mem + (phys - HIFI_UNSEC_BASE_ADDR);
	runtime->dma_addr = phys;
	runtime->dma_bytes = bytes;
	memset(runtime->dma_area, 0, bytes);
	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		memset(ma->hifi_mem + DMAC_PB_L_OFF, 0, params_buffer_size(params) * 4U);
		memset(ma->hifi_mem + DMAC_PB_R_OFF, 0, params_buffer_size(params) * 4U);
	} else {
		memset(ma->hifi_mem + DMAC_CP_L_OFF, 0, params_buffer_size(params) * 4U);
		memset(ma->hifi_mem + DMAC_CP_R_OFF, 0, params_buffer_size(params) * 4U);
	}
	dsb(sy);

	spin_lock_irqsave(&srt->lock, flags);
	srt->period_bytes = params_period_bytes(params);
	srt->period_frames = params_period_size(params);
	srt->periods = params_periods(params);
	srt->buf_frames = params_buffer_size(params);
	srt->channels = params_channels(params);
	srt->period_cur = 0;
	srt->period_next = 0;
	srt->last_hw_frame = 0;
	spin_unlock_irqrestore(&srt->lock, flags);

	mutex_lock(&ma->route_lock);
	if (stream == SNDRV_PCM_STREAM_PLAYBACK)
		ma->pb_active = true;
	else
		ma->cp_active = true;
	marie_apply_routes_locked(ma);
	mutex_unlock(&ma->route_lock);

	return 0;
}

static int marie_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct marie_audio *ma = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;

	marie_dmac_stop_stream(ma, substream->stream);
	if (runtime) {
		runtime->dma_area = NULL;
		runtime->dma_addr = 0;
		runtime->dma_bytes = 0;
	}
	return 0;
}

static int marie_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct marie_audio *ma = substream->private_data;
	int stream = substream->stream;
	struct marie_stream_rt *srt = &ma->streams[stream];
	unsigned long flags;

	marie_dmac_stop_stream(ma, stream);

	spin_lock_irqsave(&srt->lock, flags);
	srt->running = false;
	srt->hifi_acked = false;
	srt->period_cur = 0;
	srt->period_next = 0;
	srt->last_hw_frame = 0;
	srt->periods_elapsed = 0;
	spin_unlock_irqrestore(&srt->lock, flags);

	return 0;
}

static int marie_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct marie_audio *ma = substream->private_data;
	int stream = substream->stream;
	struct marie_stream_rt *srt = &ma->streams[stream];
	unsigned int pframes = 0, periods = 0, bframes = 0;
	unsigned long flags;
	bool start_dma = false, stop_dma = false;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		spin_lock_irqsave(&srt->lock, flags);
		pframes = srt->period_frames;
		periods = srt->periods;
		bframes = srt->buf_frames;
		srt->running = true;
		srt->hifi_acked = false;
		srt->period_cur = 0;
		srt->last_hw_frame = 0;
		srt->start_ns = ktime_get_ns();
		srt->last_adv_ns = srt->start_ns;
		srt->periods_elapsed = 0;
		spin_unlock_irqrestore(&srt->lock, flags);
		start_dma = true;
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		spin_lock_irqsave(&srt->lock, flags);
		srt->running = false;
		spin_unlock_irqrestore(&srt->lock, flags);
		stop_dma = true;
		break;

	default:
		return -EINVAL;
	}

	if (start_dma) {
		if (stream == SNDRV_PCM_STREAM_PLAYBACK)
			marie_pb_sync_frames(ma, 0U, bframes, bframes);
		marie_dmac_start_stream(ma, stream, pframes, periods);
		if (!hrtimer_active(&ma->poll_timer))
			hrtimer_start(&ma->poll_timer, ms_to_ktime(1), HRTIMER_MODE_REL);
	} else if (stop_dma) {
		marie_dmac_stop_stream(ma, stream);
	}

	return 0;
}

static snd_pcm_uframes_t marie_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct marie_audio *ma = substream->private_data;
	int stream = substream->stream;
	struct marie_stream_rt *srt = &ma->streams[stream];
	unsigned long flags;
	unsigned int bframes, hw_frame = 0, cur_frames;
	bool use_hw = false;

	spin_lock_irqsave(&srt->lock, flags);
	bframes = srt->buf_frames;
	cur_frames = srt->period_cur * srt->period_frames;
	if (srt->running && srt->hifi_acked &&
	    marie_dmac_read_hw_frame(ma, stream, bframes, &hw_frame))
		use_hw = true;
	spin_unlock_irqrestore(&srt->lock, flags);

	if (use_hw)
		cur_frames = hw_frame;
	if (cur_frames >= substream->runtime->buffer_size)
		cur_frames = 0;
	return cur_frames;
}

static int marie_pcm_copy(struct snd_pcm_substream *substream, int channel,
			  unsigned long pos, struct iov_iter *iter, unsigned long bytes)
{
	struct marie_audio *ma = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	void *hwbuf;

	if (!runtime || !runtime->dma_area)
		return -EFAULT;
	hwbuf = runtime->dma_area + pos;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		unsigned int fbytes = frames_to_bytes(runtime, 1);

		if (copy_from_iter(hwbuf, bytes, iter) != bytes)
			return -EFAULT;
		if (fbytes)
			marie_pb_sync_frames(ma, pos / fbytes, bytes / fbytes,
					     runtime->buffer_size);
		else
			dsb(sy);
	} else {
		dsb(sy);
		if (copy_to_iter(hwbuf, bytes, iter) != bytes)
			return -EFAULT;
	}
	return 0;
}

static int marie_pcm_fill_silence(struct snd_pcm_substream *substream, int channel,
				  unsigned long pos, unsigned long bytes)
{
	struct marie_audio *ma = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;

	if (!runtime || !runtime->dma_area)
		return -EFAULT;
	memset(runtime->dma_area + pos, 0, bytes);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		unsigned int fbytes = frames_to_bytes(runtime, 1);

		if (fbytes)
			marie_pb_sync_frames(ma, pos / fbytes, bytes / fbytes,
					     runtime->buffer_size);
	}
	dsb(sy);
	return 0;
}

static int marie_pcm_mmap(struct snd_pcm_substream *substream, struct vm_area_struct *vma)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	if (!runtime || !runtime->dma_addr || offset + size > PAGE_ALIGN(runtime->dma_bytes))
		return -EINVAL;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start,
			       (runtime->dma_addr + offset) >> PAGE_SHIFT,
			       size, vma->vm_page_prot);
}

static const struct snd_pcm_ops marie_pcm_ops = {
	.open		= marie_pcm_open,
	.close		= marie_pcm_close,
	.hw_params	= marie_pcm_hw_params,
	.hw_free	= marie_pcm_hw_free,
	.prepare	= marie_pcm_prepare,
	.trigger	= marie_pcm_trigger,
	.pointer	= marie_pcm_pointer,
	.copy		= marie_pcm_copy,
	.fill_silence	= marie_pcm_fill_silence,
	.mmap		= marie_pcm_mmap,
};

/* ---------- ALSA Mixer Controls ---------- */

static const char * const marie_out_texts[] = {
	"Speaker", "Headphones", "Earpiece", "Speaker+Headphones"
};

static int marie_out_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(marie_out_texts), marie_out_texts);
}

static int marie_out_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct marie_audio *ma = snd_kcontrol_chip(kcontrol);

	ucontrol->value.enumerated.item[0] = ma->out_route;
	return 0;
}

static int marie_out_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct marie_audio *ma = snd_kcontrol_chip(kcontrol);
	unsigned int val = ucontrol->value.enumerated.item[0];
	bool changed = false;

	if (val >= ARRAY_SIZE(marie_out_texts))
		return -EINVAL;

	mutex_lock(&ma->route_lock);
	if (ma->out_route != val) {
		ma->out_route = val;
		marie_apply_routes_locked(ma);
		changed = true;
	}
	mutex_unlock(&ma->route_lock);
	if (changed)
		marie_hifi_sync_routes(ma);
	return changed ? 1 : 0;
}

static const char * const marie_in_texts[] = { "Main Mic", "Headset Mic" };

static int marie_in_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(marie_in_texts), marie_in_texts);
}

static int marie_in_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct marie_audio *ma = snd_kcontrol_chip(kcontrol);

	ucontrol->value.enumerated.item[0] = ma->in_route;
	return 0;
}

static int marie_in_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct marie_audio *ma = snd_kcontrol_chip(kcontrol);
	unsigned int val = ucontrol->value.enumerated.item[0];
	bool changed = false;

	if (val >= ARRAY_SIZE(marie_in_texts))
		return -EINVAL;

	mutex_lock(&ma->route_lock);
	if (ma->in_route != val) {
		ma->in_route = val;
		marie_apply_routes_locked(ma);
		changed = true;
	}
	mutex_unlock(&ma->route_lock);
	if (changed)
		marie_hifi_sync_routes(ma);
	return changed ? 1 : 0;
}

static int marie_vol_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	int max_val = (int)kcontrol->private_value;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = max_val;
	return 0;
}

static int marie_spk_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct marie_audio *ma = snd_kcontrol_chip(kc);

	uc->value.integer.value[0] = ma->spk_vol;
	return 0;
}

static int marie_spk_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct marie_audio *ma = snd_kcontrol_chip(kc);
	int v = clamp_t(long, uc->value.integer.value[0], 0, 100);
	bool changed = false;

	mutex_lock(&ma->route_lock);
	if (ma->spk_vol != v) {
		ma->spk_vol = v;
		if (ma->tfa_powered)
			tfa9874_update16(ma, 0x61, 0x03c0, marie_spk_vol_to_tfa_attn(v));
		changed = true;
	}
	mutex_unlock(&ma->route_lock);
	return changed ? 1 : 0;
}

static int marie_hp_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct marie_audio *ma = snd_kcontrol_chip(kc);

	uc->value.integer.value[0] = ma->hp_vol;
	return 0;
}

static int marie_hp_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct marie_audio *ma = snd_kcontrol_chip(kc);
	int v = clamp_t(long, uc->value.integer.value[0], 0, 15);
	bool changed = false;

	mutex_lock(&ma->route_lock);
	if (ma->hp_vol != v) {
		ma->hp_vol = v;
		if (ma->hp_powered)
			marie_pmic_write(ma, PMIC_ANA_RW21_REG, (u8)((v << 4) | v));
		changed = true;
	}
	mutex_unlock(&ma->route_lock);
	return changed ? 1 : 0;
}

static int marie_ear_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct marie_audio *ma = snd_kcontrol_chip(kc);

	uc->value.integer.value[0] = ma->ear_vol;
	return 0;
}

static int marie_ear_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct marie_audio *ma = snd_kcontrol_chip(kc);
	int v = clamp_t(long, uc->value.integer.value[0], 0, 15);
	bool changed = false;

	mutex_lock(&ma->route_lock);
	if (ma->ear_vol != v) {
		ma->ear_vol = v;
		if (ma->ear_powered)
			marie_pmic_update(ma, PMIC_ANA_RW26_REG, 0x0f, (u8)v);
		changed = true;
	}
	mutex_unlock(&ma->route_lock);
	return changed ? 1 : 0;
}

static int marie_mic_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct marie_audio *ma = snd_kcontrol_chip(kc);

	uc->value.integer.value[0] = ma->mic_vol;
	return 0;
}

static int marie_mic_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct marie_audio *ma = snd_kcontrol_chip(kc);
	int v = clamp_t(long, uc->value.integer.value[0], 0, 7);
	bool changed = false;

	mutex_lock(&ma->route_lock);
	if (ma->mic_vol != v) {
		ma->mic_vol = v;
		if (ma->cp_active)
			marie_apply_routes_locked(ma);
		changed = true;
	}
	mutex_unlock(&ma->route_lock);
	return changed ? 1 : 0;
}

static int marie_jack_ctl_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int marie_jack_ctl_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct marie_audio *ma = snd_kcontrol_chip(kc);

	uc->value.integer.value[0] = ma->jack_plugged ? 1 : 0;
	return 0;
}

static const struct snd_kcontrol_new marie_snd_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name  = "Output Path",
		.info  = marie_out_info,
		.get   = marie_out_get,
		.put   = marie_out_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name  = "Capture Mic",
		.info  = marie_in_info,
		.get   = marie_in_get,
		.put   = marie_in_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name  = "Speaker Volume",
		.info  = marie_vol_info,
		.get   = marie_spk_vol_get,
		.put   = marie_spk_vol_put,
		.private_value = 100,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name  = "Headphone Volume",
		.info  = marie_vol_info,
		.get   = marie_hp_vol_get,
		.put   = marie_hp_vol_put,
		.private_value = 15,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name  = "Earpiece Volume",
		.info  = marie_vol_info,
		.get   = marie_ear_vol_get,
		.put   = marie_ear_vol_put,
		.private_value = 15,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name  = "Mic Volume",
		.info  = marie_vol_info,
		.get   = marie_mic_vol_get,
		.put   = marie_mic_vol_put,
		.private_value = 7,
	},
	{
		.iface  = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name   = "Headset Plugged",
		.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info   = marie_jack_ctl_info,
		.get    = marie_jack_ctl_get,
	},
};

/* ---------- 3.5mm Headset Jack Polling ---------- */

static void marie_jack_work_fn(struct work_struct *work)
{
	struct marie_audio *ma = container_of(work, struct marie_audio, jack_work.work);
	u8 stat = marie_pmic_read(ma, PMIC_ANA_IRQ_STAT_REG);
	bool plugged = !!(stat & BIT(3)); /* HS_DET_IRQ */

	if (plugged != ma->jack_plugged) {
		ma->jack_plugged = plugged;
		if (ma->jack)
			snd_jack_report(ma->jack, plugged ? SND_JACK_HEADSET : 0);
		dev_info(ma->dev, "3.5mm jack %s (stat=0x%02x)\n",
			 plugged ? "inserted" : "removed", stat);
	}
	schedule_delayed_work(&ma->jack_work, msecs_to_jiffies(1000));
}

/* ---------- Sysfs Diagnostics (/sys/kernel/marie_audio/status) ---------- */

static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct marie_audio *ma = g_marie_audio;
	u32 panic_mark = 0, loaded = 0, pwr_stat = 0, uart_cur = 0;
	u32 tx_f = 0, tx_r = 0, rx_f = 0, rx_r = 0;
	u16 tfa_st0 = 0, tfa_st1 = 0, tfa_attn = 0;

	if (!ma)
		return sysfs_emit(buf, "not probed\n");

	if (ma->hifi_mem) {
		u32 *flg = (u32 *)(ma->hifi_mem + HIFI_FLAG_DATA_OFFSET);
		struct hifi_mb_head *txh = (struct hifi_mb_head *)(ma->hifi_mem + MB_TX_HEAD_OFF);
		struct hifi_mb_head *rxh = (struct hifi_mb_head *)(ma->hifi_mem + MB_RX_HEAD_OFF);

		panic_mark = READ_ONCE(flg[0]);
		uart_cur   = READ_ONCE(flg[2]);
		loaded     = READ_ONCE(flg[5]);
		pwr_stat   = READ_ONCE(flg[8]);
		tx_f = READ_ONCE(txh->front);
		tx_r = READ_ONCE(txh->rear);
		rx_f = READ_ONCE(rxh->front);
		rx_r = READ_ONCE(rxh->rear);
	}
	if (!ma->tfa_i2c)
		marie_find_smartpa_i2c(ma);
	if (ma->tfa_i2c) {
		if (!ma->tfa_rev)
			tfa9874_read16(ma, 0x03, &ma->tfa_rev);
		tfa9874_read16(ma, 0x10, &tfa_st0);
		tfa9874_read16(ma, 0x11, &tfa_st1);
		tfa9874_read16(ma, 0x61, &tfa_attn);
	}

	return sysfs_emit(buf,
		"soccodec_ver=0x%08x i2s1_boot=0x%08x i2s1_cur=0x%08x clk_en0=0x%08x clk_en1=0x%08x\n"
		"aspcfg_rst=0x%08x aspcfg_clk=0x%08x dma_sel=0x%08x dmac_ch=0x%08x tc2=0x%08x err=0x%08x\n"
		"ch5_src=0x%08x ch5_cfg=0x%08x ch6_cfg=0x%08x ch7_des=0x%08x ch7_cfg=0x%08x\n"
		"hifi_loaded=0x%08x hifi_pwr=0x%08x hifi_panic=0x%08x uart_cur=0x%08x\n"
		"mb_tx=%u/%u mb_rx=%u/%u tx_cnt=%d rx_irq=%d rx_mail=%d last_rx=0x%04x\n"
		"tfa_rev=0x%04x tfa_pwr=%d tfa_st0=0x%04x tfa_st1=0x%04x tfa_0x61=0x%04x\n"
		"out=%d in=%d spk_vol=%d hp_vol=%d ear_vol=%d mic_vol=%d jack=%d pb=%d cp=%d\n",
		ma->sc_version, ma->sc_i2s1_ctrl0_boot,
		marie_sc_read(ma, SC_I2S1_TDM_CTRL0_REG),
		marie_sc_read(ma, SC_CLK_EN0_REG),
		marie_sc_read(ma, SC_CLK_EN1_REG),
		ma->aspcfg ? readl(ma->aspcfg + 0x008U) : 0U,
		ma->aspcfg ? readl(ma->aspcfg + 0x018U) : 0U,
		ma->aspcfg ? readl(ma->aspcfg + ASPCFG_CODEC_DMA_SEL) : 0U,
		ma->dmac ? readl(ma->dmac + ASP_DMA_CH_STAT) : 0U,
		ma->dmac ? readl(ma->dmac + ASP_DMA_INT_TC2_RAW) : 0U,
		ma->dmac ? (readl(ma->dmac + ASP_DMA_INT_ERR1_RAW) |
			    readl(ma->dmac + ASP_DMA_INT_ERR2_RAW) |
			    readl(ma->dmac + ASP_DMA_INT_ERR3_RAW)) : 0U,
		ma->dmac ? readl(ma->dmac + ASP_DMA_CX_CURR_SRC(DMAC_CH_PB_L)) : 0U,
		ma->dmac ? readl(ma->dmac + ASP_DMA_CX_CONFIG(DMAC_CH_PB_L)) : 0U,
		ma->dmac ? readl(ma->dmac + ASP_DMA_CX_CONFIG(DMAC_CH_PB_R)) : 0U,
		ma->dmac ? readl(ma->dmac + ASP_DMA_CX_CURR_DES(DMAC_CH_CP_L)) : 0U,
		ma->dmac ? readl(ma->dmac + ASP_DMA_CX_CONFIG(DMAC_CH_CP_L)) : 0U,
		loaded, pwr_stat, panic_mark, uart_cur,
		tx_f, tx_r, rx_f, rx_r,
		atomic_read(&ma->tx_mail_cnt),
		atomic_read(&ma->rx_irq_cnt),
		atomic_read(&ma->rx_mail_cnt),
		ma->last_rx_msg,
		ma->tfa_rev, ma->tfa_powered ? 1 : 0, tfa_st0, tfa_st1, tfa_attn,
		ma->out_route, ma->in_route, ma->spk_vol, ma->hp_vol, ma->ear_vol,
		ma->mic_vol, ma->jack_plugged ? 1 : 0,
		ma->pb_active ? 1 : 0, ma->cp_active ? 1 : 0);
}

static ssize_t dsp_log_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct marie_audio *ma = g_marie_audio;
	u32 cur;
	const char *uart_buf;
	size_t start, n, i, out = 0;

	if (!ma || !ma->hifi_mem)
		return 0;

	cur = READ_ONCE(*(u32 *)(ma->hifi_mem + HIFI_FLAG_DATA_OFFSET + 8U));
	if (cur <= 0x100U || cur > DRV_DSP_UART_SIZE)
		cur = 0x900U;
	uart_buf = (const char *)(ma->hifi_mem + DRV_DSP_UART_OFFSET);
	start = (cur > 2048U + 0x100U) ? (cur - 2048U) : 0x100U;
	n = cur - start;

	for (i = 0; i < n && out + 2 < PAGE_SIZE; i++) {
		char c = uart_buf[start + i];

		if ((c >= 0x20 && c <= 0x7e) || c == '\n' || c == '\t')
			buf[out++] = c;
	}
	buf[out] = '\0';
	return out;
}

static struct kobj_attribute status_attr = __ATTR_RO(status);
static struct kobj_attribute dsp_log_attr = __ATTR_RO(dsp_log);

static struct attribute *marie_audio_attrs[] = {
	&status_attr.attr,
	&dsp_log_attr.attr,
	NULL,
};

static const struct attribute_group marie_audio_attr_group = {
	.attrs = marie_audio_attrs,
};

/* ---------- ASP Power/Clocks & Platform Driver Probe ---------- */

static int marie_asp_power_clk_enable(struct marie_audio *ma)
{
	int ret;

	/* 1. Enable PMIC LDO8 @ 1.80 V (selector 3 = 1800000 uV) */
	marie_pmic_update(ma, PMIC_LDO8_VSET_REG, 0x07, 0x03);
	marie_pmic_update(ma, PMIC_LDO8_ENABLE_REG, 0x01, 0x01);
	usleep_range(200, 400);

	/* 2. Enable ASP power domain via LPM3 IP regulator mailbox (ch 14) */
	ret = kirin710_ip_reg_send(0x00050001U, 0x00000000U);
	if (ret) {
		dev_warn(ma->dev, "ASP IP regulator LPM3 send returned %d\n", ret);
		return ret;
	}

	/* 3. Enable SCTRL & CRGCTRL ASP clock gates */
	writel(0x00010001U, ma->sctrl + 0x258U);   /* clkgt_aspsys */
	writel(0x00400040U, ma->sctrl + 0x270U);   /* clkgt_asp_codec */
	writel(BIT(27) | BIT(30), ma->sctrl + 0x160U); /* clk_asp_tcxo | clkasp_codec */
	writel(BIT(4) | BIT(6), ma->sctrl + 0x170U);   /* clk_asp_subsys | clk_asp_subsys_peri */
	writel(0x00100010U, ma->crgctrl + 0x0f4U); /* clk_ao_asp_gt */
	writel(BIT(26), ma->crgctrl + 0x000U);     /* clk_ao_asp */
	udelay(50);

	/* 4. Enable interactive clk_asp_codec (49.152 MHz) via LPM3 clk mailbox (ch 13) */
	ret = kirin710_xfreq_send_mhz(0x000d0002U, 0U);
	if (ret) {
		dev_warn(ma->dev, "clk_asp_codec LPM3 send returned %d\n", ret);
		return ret;
	}
	usleep_range(500, 1000);
	return 0;
}

static void marie_find_smartpa_i2c(struct marie_audio *ma)
{
	struct device *d;
	struct i2c_adapter *adap;
	struct device_node *spa_np;

	if (ma->tfa_i2c)
		return;

	d = bus_find_device_by_name(&i2c_bus_type, NULL, "3-0034");
	if (d) {
		ma->tfa_i2c = i2c_verify_client(d);
		if (ma->tfa_i2c)
			return;
		put_device(d);
	}

	for_each_compatible_node(spa_np, NULL, "huawei,smartpakit_i2c") {
		u32 addr = 0;

		if (of_property_read_u32(spa_np, "reg", &addr) == 0 && addr == 0x34U) {
			ma->tfa_i2c = of_find_i2c_device_by_node(spa_np);
			if (ma->tfa_i2c) {
				of_node_put(spa_np);
				return;
			}
		}
	}

	adap = i2c_get_adapter(3);
	if (adap) {
		struct i2c_board_info info = {
			I2C_BOARD_INFO("tfa9874", 0x34),
		};

		ma->tfa_i2c = i2c_new_client_device(adap, &info);
		if (IS_ERR(ma->tfa_i2c))
			ma->tfa_i2c = NULL;
		i2c_put_adapter(adap);
	}
}

static int marie_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct marie_audio *ma;
	struct device_node *codec_np, *ipc_np, *mbx2_np;
	struct platform_device *codec_pdev;
	size_t i;
	int ret;

	if (!of_machine_is_compatible("hisilicon,kirin710"))
		return -ENODEV;

	ma = devm_kzalloc(dev, sizeof(*ma), GFP_KERNEL);
	if (!ma)
		return -ENOMEM;

	ma->dev = dev;
	ma->pmic = kirin710_pmic_get_sdev();
	if (!ma->pmic)
		return -EPROBE_DEFER;

	spin_lock_init(&ma->mb_tx_lock);
	spin_lock_init(&ma->mb_rx_lock);
	mutex_init(&ma->route_lock);
	spin_lock_init(&ma->streams[0].lock);
	spin_lock_init(&ma->streams[1].lock);
	INIT_DELAYED_WORK(&ma->jack_work, marie_jack_work_fn);
	INIT_DELAYED_WORK(&ma->route_work, marie_route_work_fn);
	hrtimer_init(&ma->poll_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ma->poll_timer.function = marie_poll_timer_fn;

	ma->out_route = MARIE_OUT_SPEAKER;
	ma->in_route = MARIE_IN_MAIN_MIC;
	ma->spk_vol = 85;
	ma->hp_vol = 12;
	ma->ear_vol = 12;
	ma->mic_vol = 5;

	ma->sctrl = devm_ioremap(dev, MARIE_SCTRL_PHYS, 0x1000);
	ma->crgctrl = devm_ioremap(dev, MARIE_CRGCTRL_PHYS, 0x1000);
	ma->ipc_base = devm_ioremap(dev, MARIE_IPC_NS_PHYS, 0x1000);
	ma->soccodec = devm_ioremap(dev, MARIE_SOCCODEC_PHYS, 0x1000);
	ma->aspcfg = devm_ioremap(dev, MARIE_ASPCFG_PHYS, 0x400);
	ma->dmac = devm_ioremap(dev, MARIE_ASPDMAC_PHYS, 0x1000);
	if (!ma->sctrl || !ma->crgctrl || !ma->ipc_base || !ma->soccodec ||
	    !ma->aspcfg || !ma->dmac)
		return -ENOMEM;

	ma->hifi_mem = devm_memremap(dev, HIFI_UNSEC_BASE_ADDR, HIFI_UNSEC_REGION_SIZE, MEMREMAP_WC);
	if (IS_ERR(ma->hifi_mem))
		return PTR_ERR(ma->hifi_mem);

	/* Enable ASP power & clocks */
	ret = marie_asp_power_clk_enable(ma);
	if (ret == -EPROBE_DEFER)
		return ret;

	/* Select codec default pinctrl (I2S1 + SIF pads) */
	codec_np = of_find_compatible_node(NULL, NULL, "hisilicon,hi6555v2-codec");
	if (codec_np) {
		codec_pdev = of_find_device_by_node(codec_np);
		of_node_put(codec_np);
		if (codec_pdev) {
			ma->codec_pctrl = pinctrl_get(&codec_pdev->dev);
			if (!IS_ERR(ma->codec_pctrl)) {
				ma->codec_pin_default =
					pinctrl_lookup_state(ma->codec_pctrl, PINCTRL_STATE_DEFAULT);
				if (!IS_ERR(ma->codec_pin_default))
					pinctrl_select_state(ma->codec_pctrl, ma->codec_pin_default);
			} else {
				ma->codec_pctrl = NULL;
			}
			put_device(&codec_pdev->dev);
		}
	}

	/* Initialize HiFi3 shared-memory regions & mailbox channels */
	memset(ma->hifi_mem + HIFI_AP_NV_OFFSET, 0, HIFI_AP_NV_SIZE);
	WRITE_ONCE(*(u32 *)(ma->hifi_mem + HIFI_AP_NV_OFFSET + 0U), HIFI_NVPARAM_MAGIC);
	WRITE_ONCE(*(u32 *)(ma->hifi_mem + HIFI_AP_NV_OFFSET + HIFI_NVPARAM_TAIL_OFF),
		   HIFI_NVPARAM_MAGIC);
	memset(ma->hifi_mem + PCM_PLAY_BUFF_OFFSET, 0, PCM_PLAY_BUFF_SIZE);
	{
		u32 *flg = (u32 *)(ma->hifi_mem + HIFI_FLAG_DATA_OFFSET);

		WRITE_ONCE(flg[1], 2U);          /* DRV_DSP_UART_LOG_LEVEL = INFO */
		WRITE_ONCE(flg[3], 0xffffffffU); /* DRV_DSP_EXCEPTION_NO */
		WRITE_ONCE(flg[10], 0x5a5a5a5aU);/* FAMA head magic */
		WRITE_ONCE(flg[11], 0U);         /* FAMA off */
		WRITE_ONCE(flg[12], 0xa5a5a5a5U);/* FAMA rear magic */
		hifi_mb_init_queue(ma->hifi_mem + MB_RX_HEAD_OFF, ma->hifi_mem + MB_RX_DATA_OFF);
		hifi_mb_init_queue(ma->hifi_mem + MB_TX_HEAD_OFF, ma->hifi_mem + MB_TX_DATA_OFF);
	}
	dsb(sy);

	ma->tx_client.dev = dev;
	ma->tx_client.tx_block = false;
	ma->tx_chan = kirin710_mbox_request_channel(&ma->tx_client, 18);
	if (IS_ERR(ma->tx_chan)) {
		ret = PTR_ERR(ma->tx_chan);
		ma->tx_chan = NULL;
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_warn(dev, "IPC mailbox-18 request returned %d\n", ret);
	}

	ipc_np = of_find_compatible_node(NULL, NULL, "hisilicon,HiIPCV230");
	if (ipc_np) {
		mbx2_np = of_get_child_by_name(ipc_np, "mailbox-2");
		of_node_put(ipc_np);
		if (mbx2_np) {
			ma->rx_irq = irq_of_parse_and_map(mbx2_np, 0);
			of_node_put(mbx2_np);
			if (ma->rx_irq > 0) {
				ret = devm_request_irq(dev, ma->rx_irq, marie_hifi_rx_isr,
						       IRQF_SHARED, "marie-hifi-rx", ma);
				if (ret)
					dev_warn(dev, "mailbox-2 irq %d err %d\n", ma->rx_irq, ret);
			}
		}
	}

	/* Initialize HI6555v2 digital/analog codec & locate NXP TFA9874 SmartPA */
	marie_codec_hw_reset_init(ma);
	marie_find_smartpa_i2c(ma);
	if (ma->tfa_i2c)
		tfa9874_read16(ma, 0x03, &ma->tfa_rev);

	/* Register ALSA sound card 0 */
	ret = snd_card_new(dev, 0, "MarieAudio", THIS_MODULE, 0, &ma->card);
	if (ret < 0)
		return ret;

	strscpy(ma->card->driver, "marie_audio", sizeof(ma->card->driver));
	strscpy(ma->card->shortname, "Kirin710-Marie", sizeof(ma->card->shortname));
	snprintf(ma->card->longname, sizeof(ma->card->longname),
		 "Huawei P30 lite Kirin 710 HI6555v2 + TFA9874 Audio");

	ret = snd_pcm_new(ma->card, "Marie PCM", 0, 1, 1, &ma->pcm);
	if (ret < 0)
		goto err_card;

	ma->pcm->private_data = ma;
	strscpy(ma->pcm->name, "Kirin710 HiFi PCM", sizeof(ma->pcm->name));
	snd_pcm_set_ops(ma->pcm, SNDRV_PCM_STREAM_PLAYBACK, &marie_pcm_ops);
	snd_pcm_set_ops(ma->pcm, SNDRV_PCM_STREAM_CAPTURE, &marie_pcm_ops);

	for (i = 0; i < ARRAY_SIZE(marie_snd_controls); i++) {
		ret = snd_ctl_add(ma->card, snd_ctl_new1(&marie_snd_controls[i], ma));
		if (ret < 0)
			goto err_card;
	}

	ret = snd_jack_new(ma->card, "Headset Jack",
			   SND_JACK_HEADSET, &ma->jack, true, false);
	if (ret == 0 && ma->jack)
		snd_jack_report(ma->jack, 0);

	ret = snd_card_register(ma->card);
	if (ret < 0)
		goto err_card;

	platform_set_drvdata(pdev, ma);
	g_marie_audio = ma;

	marie_audio_kobj = kobject_create_and_add("marie_audio", kernel_kobj);
	if (marie_audio_kobj) {
		if (sysfs_create_group(marie_audio_kobj, &marie_audio_attr_group))
			dev_warn(dev, "failed to create marie_audio sysfs group\n");
	}

	schedule_delayed_work(&ma->jack_work, msecs_to_jiffies(200));

	{
		u32 *flg = (u32 *)(ma->hifi_mem + HIFI_FLAG_DATA_OFFSET);

		dev_info(dev,
			 "Kirin 710 audio ready: SoCCODEC=0x%08x I2S1=0x%08x TFA9874=0x%04x HiFi(loaded=0x%x pwr=0x%x panic=0x%x) rx_irq=%d\n",
			 ma->sc_version, ma->sc_i2s1_ctrl0_boot, ma->tfa_rev,
			 READ_ONCE(flg[5]), READ_ONCE(flg[8]), READ_ONCE(flg[0]), ma->rx_irq);
	}
	return 0;

err_card:
	snd_card_free(ma->card);
	return ret;
}

static const struct of_device_id marie_audio_of_match[] = {
	{ .compatible = "hi6250_hi6555c" },
	{ }
};
MODULE_DEVICE_TABLE(of, marie_audio_of_match);

static struct platform_driver marie_audio_driver = {
	.driver = {
		.name = "marie-audio",
		.of_match_table = marie_audio_of_match,
	},
	.probe = marie_audio_probe,
};

static int __init marie_audio_init(void)
{
	return platform_driver_register(&marie_audio_driver);
}
late_initcall(marie_audio_init);

MODULE_DESCRIPTION("Huawei P30 lite Kirin 710 Audio Driver");
MODULE_LICENSE("GPL");

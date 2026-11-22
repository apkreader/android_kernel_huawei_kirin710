#include <linux/fs.h>
#include <linux/mmc/ioctl.h>	/* for struct mmc_ioc_rpmb */
#include <linux/mmc/card.h>	/* for struct mmc_card */
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/unaligned.h>
#include <ufs/ufs-kirin710.h>
#include <linux/hisi/rpmb.h>

#include "teek_client_constants.h"
/*#define TC_DEBUG*/
#include "teek_ns_client.h"
#include "agent.h"
#include "libhwsecurec/securec.h"
#include "tc_ns_log.h"
#include "smc.h"
#include <linux/timekeeping.h>
#include <linux/delay.h>


typedef enum {
	sec_get_devinfo,
	sec_send_ioccmd,
	sec_rpmb_lock,
	sec_rpmb_unlock,
} rpmb_cmd_t;

#define RPMB_EMMC_CID_SIZE 32

/*lint -e754 -esym(754,*)*/

struct rpmb_devinfo {
	uint8_t cid[RPMB_EMMC_CID_SIZE]; /* eMMC card ID */

	uint8_t rpmb_size_mult; /* EXT CSD-slice 168 "RPMB Size" */
	uint8_t rel_wr_sec_cnt; /* EXT CSD-slice 222 "Reliable Write Sector Count" */
	uint8_t tmp[2];
	uint32_t blk_size; /* RPMB blocksize*/

	uint32_t max_blk_idx; /* The highest block index supported by current device */
	uint32_t access_start_blk_idx; /* The start block index SecureOS can access */

	uint32_t access_total_blk; /* The total blocks SecureOS can access */
	uint32_t tmp2;

	uint32_t mdt;	/* 1: EMMC 2: UFS */
	uint32_t support_bit_map;/* the device's support bit map, for example, if it support 1,2,32, then the value is 0x80000003 */
	uint32_t version; /*??16bit??0x5a5a???support_bit_map??��????16bit??��????	0x1???*/
	uint32_t tmp3;
};
struct rpmb_ioc {
	struct storage_blk_ioc_rpmb_data ioc_rpmb;  /* sizeof() = 72 */

	uint32_t buf_offset[STORAGE_IOC_MAX_RPMB_CMD];
	uint32_t tmp;
};

#define RPMB_CTRL_MAGIC	0x5A5A5A5A
struct rpmb_ctrl_t {
	uint32_t	magic;
	uint32_t	cmd_sn;

	uint8_t		lock_flag;
	uint8_t 	tmp[3];
	enum rpmb_op_type op_type;

	union __args {
		struct rpmb_devinfo get_devinfo;	/* ��С 8 * 7 */
		struct rpmb_ioc send_ioccmd;		/* ��С 8 * 11 */
	} args;


	rpmb_cmd_t	cmd;
	uint32_t 	reserved;

	uint32_t	buf_len;
	uint16_t	head_crc;
	uint16_t	buf_crc;

	int32_t		ret;
	uint32_t	reserved2;

	uint32_t buf_start[0];
};/* sizeof() = 8 * 16 = 128 */

static_assert(sizeof(struct storage_blk_ioc_data) == 24);
static_assert(offsetof(struct rpmb_ctrl_t, buf_start) == 128);

/*lint -e754 +esym(754,*)*/

#define GET_RPMB_LOCK_MASK BIT(0)
#define FREE_RPMB_LOCK_MASK BIT(1)
#define RPMB_COUNTER_RESPONSE 0x0200
#define RPMB_RESULT_NO_KEY 0x0007
#define RPMB_NONCE_OFFSET 484
#define RPMB_NONCE_SIZE 16

static DEFINE_MUTEX(rpmb_agent_lock);
static bool rpmb_locked;
static unsigned int rpmb_owner;

static int rpmb_buffer(struct rpmb_ctrl_t *ctrl, unsigned int index,
		       u8 **buf, unsigned int *len)
{
	struct rpmb_ioc *ioc = &ctrl->args.send_ioccmd;
	struct storage_blk_ioc_data *data = &ioc->ioc_rpmb.data[index];
	u32 offset = ioc->buf_offset[index];
	unsigned int bytes;

	if (!data->buf || !data->blocks || data->blocks > U16_MAX ||
	    offset > ctrl->buf_len || data->buf_bytes > ctrl->buf_len - offset)
		return -EINVAL;

	bytes = data->blocks * KIRIN710_RPMB_FRAME_SIZE;
	if (bytes > data->buf_bytes)
		return -EINVAL;

	*buf = (u8 *)ctrl->buf_start + offset;
	*len = bytes;
	return 0;
}

static int rpmb_check_key(void)
{
	u8 *req, *resp;
	u16 result;
	int ret;

	req = kzalloc(2 * KIRIN710_RPMB_FRAME_SIZE, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	resp = req + KIRIN710_RPMB_FRAME_SIZE;
	put_unaligned_be16(KIRIN710_RPMB_COUNTER, req + KIRIN710_RPMB_REQUEST);
	get_random_bytes(req + RPMB_NONCE_OFFSET, RPMB_NONCE_SIZE);
	ret = ufs_kirin710_rpmb_transfer(req, KIRIN710_RPMB_FRAME_SIZE, NULL,
					resp, KIRIN710_RPMB_FRAME_SIZE);
	if (ret)
		goto out;

	if (get_unaligned_be16(resp + KIRIN710_RPMB_REQUEST) != RPMB_COUNTER_RESPONSE ||
	    memcmp(req + RPMB_NONCE_OFFSET, resp + RPMB_NONCE_OFFSET, RPMB_NONCE_SIZE)) {
		ret = -EIO;
		goto out;
	}

	/* The preflight detects an unprovisioned device. TrustCore owns MAC verification. */
	result = get_unaligned_be16(resp + KIRIN710_RPMB_RESULT);
	if (result == RPMB_RESULT_NO_KEY)
		ret = -ENOKEY;
	else if (result)
		ret = -EIO;
out:
	kfree_sensitive(req);
	return ret;
}

static int rpmb_agent_transfer(struct rpmb_ctrl_t *ctrl)
{
	u8 *req, *result = NULL, *resp;
	unsigned int req_len, result_len, resp_len;
	u16 opcode;
	int ret;

	ret = rpmb_buffer(ctrl, 0, &req, &req_len);
	if (ret)
		return ret;
	opcode = get_unaligned_be16(req + KIRIN710_RPMB_REQUEST);
	switch (ctrl->op_type) {
	case RPMB_OP_RD:
		if (opcode != KIRIN710_RPMB_READ)
			return -EINVAL;
		break;
	case RPMB_OP_WR_CNT:
		if (opcode != KIRIN710_RPMB_COUNTER)
			return -EINVAL;
		break;
	case RPMB_OP_WR_DATA:
		if (opcode != KIRIN710_RPMB_WRITE)
			return -EINVAL;
		ret = rpmb_buffer(ctrl, 1, &result, &result_len);
		if (ret)
			return ret;
		if (result_len != KIRIN710_RPMB_FRAME_SIZE)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	ret = rpmb_buffer(ctrl, result ? 2 : 1, &resp, &resp_len);
	if (ret)
		return ret;
	if (opcode != KIRIN710_RPMB_COUNTER) {
		ret = rpmb_check_key();
		if (ret)
			return ret;
	}

	return ufs_kirin710_rpmb_transfer(req, req_len, result, resp, resp_len);
}

static uint16_t tee_calc_crc16(uint8_t *pChar, int32_t lCount)
{
	uint16_t usCrc;
	uint16_t usTmp ;
	uint8_t *pTmp = NULL;

	if (NULL == pChar)
		return 0;

	usCrc = 0 ;
	pTmp = pChar ;

	while (--lCount >= 0) {
		usCrc = usCrc ^ ((uint16_t)(*pTmp++) << 8);

		for (usTmp = 0 ; usTmp < 8 ; ++usTmp) {
			if (usCrc & 0x8000) {
				usCrc = (usCrc << 1) ^ 0x1021 ;
			} else {
				usCrc = usCrc << 1 ;
			}
		}
	}

	return (usCrc & 0xFFFF) ;
}

static bool rpmb_check_data(struct rpmb_ctrl_t *ctrl, size_t len)
{
	size_t payload = offsetof(struct rpmb_ctrl_t, buf_start);
	size_t start = offsetofend(struct rpmb_ctrl_t, buf_crc);

	if (ctrl->magic != RPMB_CTRL_MAGIC || ctrl->buf_len > len - payload)
		return false;

	return tee_calc_crc16((u8 *)ctrl, offsetof(struct rpmb_ctrl_t, head_crc)) ==
	       ctrl->head_crc &&
	       tee_calc_crc16((u8 *)ctrl + start, payload - start + ctrl->buf_len) == ctrl->buf_crc;
}

static int rpmb_agent_dispatch(struct rpmb_ctrl_t *ctrl, unsigned int owner)
{
	int ret;

	if (rpmb_locked && rpmb_owner != owner)
		return -EBUSY;

	switch (ctrl->cmd) {
	case sec_send_ioccmd:
		if (ctrl->lock_flag & GET_RPMB_LOCK_MASK) {
			rpmb_locked = true;
			rpmb_owner = owner;
		}
		ret = rpmb_agent_transfer(ctrl);
		if (ctrl->lock_flag & FREE_RPMB_LOCK_MASK)
			rpmb_locked = false;
		return ret;
	case sec_rpmb_lock:
		rpmb_locked = true;
		rpmb_owner = owner;
		return 0;
	case sec_rpmb_unlock:
		rpmb_locked = false;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int rpmb_agent_work(struct tee_agent_kernel_ops *agent)
{
	struct rpmb_ctrl_t *shared, *ctrl;
	struct __smc_event_data *event;
	unsigned int owner;
	size_t len;

	if (!agent || !agent->agent_buffer || !agent->agent_buffer->kernel_addr)
		return -EINVAL;

	shared = agent->agent_buffer->kernel_addr;
	len = agent->agent_buffer->len;
	if (len < sizeof(*shared) || len > INT_MAX)
		return -EINVAL;

	ctrl = kmemdup(shared, len, GFP_KERNEL);
	if (!ctrl) {
		WRITE_ONCE(shared->ret, TEEC_ERROR_OUT_OF_MEMORY);
		return -ENOMEM;
	}
	if (!rpmb_check_data(ctrl, len)) {
		WRITE_ONCE(shared->ret, TEEC_ERROR_BAD_FORMAT);
		kfree_sensitive(ctrl);
		return -EBADMSG;
	}

	event = find_event_control(agent->agent_id);
	if (!event) {
		WRITE_ONCE(shared->ret, TEEC_ERROR_BAD_STATE);
		kfree_sensitive(ctrl);
		return -ENODEV;
	}
	owner = READ_ONCE(event->cmd.dev_file_id);
	put_agent_event(event);

	mutex_lock(&rpmb_agent_lock);
	ctrl->ret = rpmb_agent_dispatch(ctrl, owner);
	mutex_unlock(&rpmb_agent_lock);
	memcpy(shared->buf_start, ctrl->buf_start, ctrl->buf_len);
	WRITE_ONCE(shared->ret, ctrl->ret);
	kfree_sensitive(ctrl);
	return 0;
}

static int rpmb_agent_exit(struct tee_agent_kernel_ops *agent)
{
	mutex_lock(&rpmb_agent_lock);
	rpmb_locked = false;
	mutex_unlock(&rpmb_agent_lock);
	return 0;
}

static int rpmb_agent_crash_work(struct tee_agent_kernel_ops *agent,
				 TC_NS_ClientContext *context, unsigned int owner)
{
	mutex_lock(&rpmb_agent_lock);
	if (rpmb_locked && rpmb_owner == owner)
		rpmb_locked = false;
	mutex_unlock(&rpmb_agent_lock);
	return 0;
}

static struct tee_agent_kernel_ops rpmb_agent_ops = {
	.agent_name = "rpmb",
	.agent_id = TEE_RPMB_AGENT_ID,
	.tee_agent_init = NULL,
	.tee_agent_work = rpmb_agent_work,
	.tee_agent_exit = rpmb_agent_exit,
	.tee_agent_crash_work = rpmb_agent_crash_work,

	.list = LIST_HEAD_INIT(rpmb_agent_ops.list)
};
/*lint -restore*/

int rpmb_agent_register(void)
{
	tee_agent_kernel_register(&rpmb_agent_ops);

	return 0;
}

EXPORT_SYMBOL(rpmb_agent_register);

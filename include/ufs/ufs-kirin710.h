/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _UFS_KIRIN710_H
#define _UFS_KIRIN710_H

#include <linux/types.h>

#define KIRIN710_RPMB_FRAME_SIZE	512
#define KIRIN710_RPMB_BLOCK_COUNT	506
#define KIRIN710_RPMB_RESULT	508
#define KIRIN710_RPMB_REQUEST	510

#define KIRIN710_RPMB_COUNTER	0x0002
#define KIRIN710_RPMB_WRITE	0x0003
#define KIRIN710_RPMB_READ	0x0004
#define KIRIN710_RPMB_READ_RESULT	0x0005

int ufs_kirin710_rpmb_transfer(const void *request, unsigned int request_len,
			       const void *result_request, void *response,
			       unsigned int response_len);

#endif

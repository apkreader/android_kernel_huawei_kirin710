/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MAILBOX_KIRIN710_H
#define _LINUX_MAILBOX_KIRIN710_H

#include <linux/types.h>

struct mbox_chan;
struct mbox_client;

struct mbox_chan *kirin710_mbox_request_clk_channel(struct mbox_client *client);
struct mbox_chan *kirin710_mbox_request_channel(struct mbox_client *client, unsigned int index);
int kirin710_xfreq_send_mhz(u32 cmd0, u32 mhz);
int kirin710_ip_reg_send(u32 cmd0, u32 cmd1);

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MAILBOX_KIRIN710_H
#define _LINUX_MAILBOX_KIRIN710_H

struct mbox_chan;
struct mbox_client;

struct mbox_chan *kirin710_mbox_request_clk_channel(struct mbox_client *client);
struct mbox_chan *kirin710_mbox_request_channel(struct mbox_client *client, unsigned int index);

#endif

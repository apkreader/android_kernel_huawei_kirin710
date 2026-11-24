/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 apkreader */
#ifndef _HI1102_COMPAT_HW_CONNECTIVITY_H
#define _HI1102_COMPAT_HW_CONNECTIVITY_H

#include <linux/types.h>

#define CHIP_TYPE_HI110X "hisi"
#define CHIP_TYPE_BCM    "bcm43xx"

static inline bool isMyConnectivityChip(char *name)
{
	return true;
}

static inline bool isMyNfcChip(char *name)
{
	return false;
}

#endif

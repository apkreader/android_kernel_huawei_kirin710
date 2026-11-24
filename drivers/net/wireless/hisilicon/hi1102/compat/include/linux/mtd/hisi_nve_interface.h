/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 apkreader */
#ifndef _HI1102_COMPAT_HISI_NVE_H
#define _HI1102_COMPAT_HISI_NVE_H

#define NV_WRITE 0
#define NV_READ  1
#define NVE_NV_DATA_SIZE 104

struct hisi_nve_info_user {
	uint32_t nv_operation;
	uint32_t nv_number;
	char nv_name[8];
	uint32_t valid_size;
	u_char nv_data[NVE_NV_DATA_SIZE];
};

static inline int hisi_nve_direct_access(struct hisi_nve_info_user *user_info)
{
	return -ENODEV;
}

#endif

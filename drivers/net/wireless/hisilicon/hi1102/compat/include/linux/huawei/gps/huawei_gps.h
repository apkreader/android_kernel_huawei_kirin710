/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 apkreader */
#ifndef _HI1102_COMPAT_HUAWEI_GPS_H
#define _HI1102_COMPAT_HUAWEI_GPS_H

typedef int (*gps_modem_id_func)(int, int);
static inline void register_gps_set_ref_clk_func(void *pFunc) {}

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 apkreader */
#ifndef _HI1102_COMPAT_WIFI_FILTER_H
#define _HI1102_COMPAT_WIFI_FILTER_H

struct hw_wlan_filter_ops {
	int (*add_filter_items)(void *items, int count);
	int (*clear_filters)(void);
	int (*set_filter_enable)(int enable);
	int (*set_filter_enable_ex)(int type, int enable);
	int (*get_filter_pkg_stat)(void *stat, int count, int *out_count);
};

static inline int hw_register_wlan_filter(struct hw_wlan_filter_ops *ops) { return 0; }
static inline int hw_unregister_wlan_filter(void) { return 0; }

#endif

/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 apkreader
 *
 * Linux 6.12 compatibility layer for HiSilicon Hi1102 Wi-Fi driver.
 */
#ifndef _HI1102_COMPAT_612_H
#define _HI1102_COMPAT_612_H

#include <linux/version.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/clock.h>
#include <uapi/linux/sched/types.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <net/cfg80211.h>

/* Removed in 5.10+: set_fs / get_fs / KERNEL_DS */
typedef int hi1102_mm_segment_t;
#ifndef mm_segment_t
#define mm_segment_t hi1102_mm_segment_t
#endif
#ifndef KERNEL_DS
#define KERNEL_DS 0
#endif
#ifndef get_fs
#define get_fs() (0)
#endif
#ifndef set_fs
#define set_fs(x) ((void)(x))
#endif
#ifndef get_ds
#define get_ds() (0)
#endif

/* Removed in 5.6+: struct timeval / do_gettimeofday */
#ifndef _HI1102_COMPAT_TIMEVAL
#define _HI1102_COMPAT_TIMEVAL
struct hi1102_timeval {
	long tv_sec;
	long tv_usec;
};
#define timeval hi1102_timeval
static inline void do_gettimeofday(struct hi1102_timeval *tv)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	tv->tv_sec = (long)ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / NSEC_PER_USEC;
}
#endif

/* Removed in 4.19+: strnicmp -> strncasecmp */
#ifndef strnicmp
#define strnicmp strncasecmp
#endif

/* Removed in 5.18+: netif_rx_ni -> netif_rx */
#ifndef netif_rx_ni
#define netif_rx_ni(skb) netif_rx(skb)
#endif

/* Removed in 4.15+: IEEE80211_BAND_* -> NL80211_BAND_* */
#if 0
#define IEEE80211_BAND_2GHZ NL80211_BAND_2GHZ
#endif
#if 0
#define IEEE80211_BAND_5GHZ NL80211_BAND_5GHZ
#endif
#if 0
#define IEEE80211_NUM_BANDS NUM_NL80211_BANDS
#endif

/* Removed in 5.14+: current->state direct write */
#ifndef ACCESS_ONCE
#define ACCESS_ONCE(x) READ_ONCE(x)
#endif

/* 6.12 kernel_read/kernel_write helper matching 4.9 vfs_read/vfs_write */
static inline ssize_t hi1102_vfs_read(struct file *file, char *buf, size_t count, loff_t *pos)
{
	return kernel_read(file, buf, count, pos);
}
static inline ssize_t hi1102_vfs_write(struct file *file, const char *buf, size_t count, loff_t *pos)
{
	return kernel_write(file, buf, count, pos);
}
#define vfs_read(f, b, c, p)  hi1102_vfs_read((f), (char *)(b), (c), (p))
#define vfs_write(f, b, c, p) hi1102_vfs_write((f), (const char *)(b), (c), (p))

#endif

/* 6.12 additions: wait_queue_t, random_ether_addr, timespec, wakeup_source */
#include <linux/wait.h>
#include <linux/pm_wakeup.h>
#include <linux/proc_fs.h>

#ifndef wait_queue_t
typedef wait_queue_entry_t wait_queue_t;
#endif

#ifndef random_ether_addr
#define random_ether_addr(addr) eth_random_addr(addr)
#endif

#ifndef _HI1102_COMPAT_TIMESPEC
#define _HI1102_COMPAT_TIMESPEC
#define timespec timespec64
static inline void getnstimeofday(struct timespec64 *ts)
{
	ktime_get_real_ts64(ts);
}
#endif

static inline void wakeup_source_init(struct wakeup_source *ws, const char *name)
{
	if (!ws)
		return;
	memset(ws, 0, sizeof(*ws));
	ws->name = name ? kstrdup_const(name, GFP_KERNEL) : "hi1102";
	wakeup_source_add(ws);
}

static inline void wakeup_source_trash(struct wakeup_source *ws)
{
	if (!ws)
		return;
	wakeup_source_remove(ws);
	kfree_const(ws->name);
	ws->name = NULL;
}

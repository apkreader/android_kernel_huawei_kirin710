/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BPF_CGROUP_DEFS_H
#define _BPF_CGROUP_DEFS_H

#include <linux/types.h>
#include <linux/list.h>
#include <uapi/linux/bpf.h>

struct bpf_prog_array;

#ifdef CONFIG_CGROUP_BPF
struct cgroup_bpf {
	/* array of effective progs in this cgroup */
	struct bpf_prog_array __rcu *effective[MAX_BPF_ATTACH_TYPE];

	/* attached progs to this cgroup and attach flags
	 * when flags == 0 or BPF_F_ALLOW_OVERRIDE the progs list will
	 * have either zero or one element
	 * when BPF_F_ALLOW_MULTI the list can have up to BPF_CGROUP_MAX_PROGS
	 */
	struct list_head progs[MAX_BPF_ATTACH_TYPE];
	u32 flags[MAX_BPF_ATTACH_TYPE];

	/* list of cgroup shared storages */
	struct list_head storages;

	/* temp storage for effective prog array used by prog_attach/detach */
	struct bpf_prog_array __rcu *inactive;
};
#else
struct cgroup_bpf {};
#endif

#endif

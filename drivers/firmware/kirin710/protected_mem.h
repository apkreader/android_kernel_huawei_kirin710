/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _KIRIN710_TRUSTCORE_PROTECTED_MEM_H
#define _KIRIN710_TRUSTCORE_PROTECTED_MEM_H

#include <linux/types.h>

struct page;
struct device_node;

struct kirin710_cma_heap {
	size_t limit;
	size_t used;
	unsigned int order;
};

struct kirin710_sync_buffer {
	struct kirin710_cma_heap *heap;
	struct page *pages;
	phys_addr_t phys;
	size_t len;
};

int kirin710_cma_heap_init(u32 id, u32 attr, struct kirin710_cma_heap *heap,
			  struct device_node **heap_node);
int kirin710_cma_alloc(struct kirin710_cma_heap *heap, size_t len,
		      struct kirin710_sync_buffer *buffer);
int kirin710_sync_mem_init(void);
int kirin710_sync_mem_alloc(size_t len, struct kirin710_sync_buffer *buffer);
int kirin710_sync_mem_release(struct kirin710_sync_buffer *buffer);

#endif

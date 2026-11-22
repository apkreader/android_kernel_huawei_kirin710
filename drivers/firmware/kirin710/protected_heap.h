/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _KIRIN710_TRUSTCORE_PROTECTED_HEAP_H
#define _KIRIN710_TRUSTCORE_PROTECTED_HEAP_H

#include <linux/types.h>

struct dma_buf;

int kirin710_protected_heap_init(void);
void kirin710_protected_heap_exit(void);
struct dma_buf *kirin710_protected_get(int fd, u32 *handle, size_t *size);

#endif

// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cma.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/sizes.h>

#include <asm/cacheflush.h>
#include <asm/set_memory.h>
#include <asm/tlbflush.h>

#include "protected_mem.h"

#define KIRIN710_SYNC_HEAP_ID	14
#define KIRIN710_HEAP_NORMAL	0

static struct cma *sync_cma;
static DEFINE_MUTEX(sync_heap_lock);
static struct kirin710_cma_heap sync_heap;

static int __init kirin710_sync_cma_init(struct reserved_mem *rmem)
{
	unsigned long node = rmem->fdt_node;
	int ret;

	if (!of_flat_dt_is_compatible(0, "hisilicon,kirin710"))
		return -ENODEV;
	if (sync_cma || !rmem->base || !rmem->size ||
	    !of_get_flat_dt_prop(node, "reusable", NULL) ||
	    of_get_flat_dt_prop(node, "no-map", NULL))
		return -EINVAL;

	ret = cma_init_reserved_mem(rmem->base, rmem->size, 0,
				    rmem->name, &sync_cma, false);
	if (!ret)
		rmem->priv = sync_cma;
	return ret;
}
RESERVEDMEM_OF_DECLARE(kirin710_sync_cma, "hisi-cma-pool", kirin710_sync_cma_init);

int kirin710_cma_heap_init(u32 heap_id, u32 heap_attr, struct kirin710_cma_heap *heap,
			  struct device_node **heap_node)
{
	struct device_node *ion, *node;
	u32 id, attr, size;
	u64 align;
	int ret = -ENODEV;

	if (!sync_cma || !cma_get_size(sync_cma) || !can_set_direct_map())
		return -ENODEV;

	ion = of_find_compatible_node(NULL, NULL, "hisilicon,hisi-ion");
	if (!ion)
		return -ENODEV;

	for_each_available_child_of_node(ion, node) {
		if (of_property_read_u32(node, "heap-id", &id) || id != heap_id)
			continue;

		ret = -EINVAL;
		if (of_property_read_u32(node, "heap-attr", &attr) || attr != heap_attr ||
		    of_property_read_u32(node, "heap-size", &size) || !size ||
		    !PAGE_ALIGNED(size) ||
		    of_property_read_u64(node, "per-bit-size", &align) ||
		    align < PAGE_SIZE || align > size || !is_power_of_2(align))
			break;

		heap->limit = min_t(size_t, size, cma_get_size(sync_cma));
		heap->order = get_order(align);
		if (heap_node)
			*heap_node = of_node_get(node);
		ret = 0;
		break;
	}
	of_node_put(node);
	of_node_put(ion);

	return ret;
}

static int kirin710_sync_restore(struct page *page, unsigned long count)
{
	unsigned long start = (unsigned long)page_address(page);
	unsigned long i;
	int ret, first_error = 0;

	for (i = 0; i < count; i++) {
		ret = set_direct_map_default_noflush(page + i);
		if (!ret && !kernel_page_present(page + i))
			ret = -EIO;
		if (ret && !first_error)
			first_error = ret;
	}
	flush_tlb_kernel_range(start, start + count * PAGE_SIZE);
	return first_error;
}

int kirin710_sync_mem_init(void)
{
	return kirin710_cma_heap_init(KIRIN710_SYNC_HEAP_ID, KIRIN710_HEAP_NORMAL,
				     &sync_heap, NULL);
}

int kirin710_sync_mem_alloc(size_t size, struct kirin710_sync_buffer *buffer)
{
	return kirin710_cma_alloc(&sync_heap, size, buffer);
}

int kirin710_cma_alloc(struct kirin710_cma_heap *heap, size_t size,
		      struct kirin710_sync_buffer *buffer)
{
	unsigned long count, start, i;
	struct page *page;
	int ret;

	if (!size || size > heap->limit)
		return -EINVAL;

	size = PAGE_ALIGN(size);
	count = size >> PAGE_SHIFT;
	mutex_lock(&sync_heap_lock);
	if (size > heap->limit - heap->used) {
		mutex_unlock(&sync_heap_lock);
		return -ENOMEM;
	}
	heap->used += size;
	mutex_unlock(&sync_heap_lock);

	page = cma_alloc(sync_cma, count, heap->order, true);
	if (!page) {
		ret = -ENOMEM;
		goto uncharge;
	}

	/* These CMA pages have no other users. Clean their contents before the SMC. */
	start = (unsigned long)page_address(page);
	memzero_explicit((void *)start, size);
	dcache_clean_inval_poc(start, start + size);
	for (i = 0; i < count; i++) {
		ret = set_direct_map_invalid_noflush(page + i);
		if (!ret && kernel_page_present(page + i))
			ret = -EIO;
		if (ret)
			goto restore;
	}
	flush_tlb_kernel_range(start, start + size);

	buffer->heap = heap;
	buffer->pages = page;
	buffer->phys = page_to_phys(page);
	buffer->len = size;
	return 0;

restore:
	if (kirin710_sync_restore(page, i + 1)) {
		pr_err("TrustCore: could not restore CMA mappings after a failed allocation\n");
		return ret;
	}
	cma_release(sync_cma, page, count);
uncharge:
	mutex_lock(&sync_heap_lock);
	heap->used -= size;
	mutex_unlock(&sync_heap_lock);
	return ret;
}

int kirin710_sync_mem_release(struct kirin710_sync_buffer *buffer)
{
	unsigned long start = (unsigned long)page_address(buffer->pages);
	unsigned long count = buffer->len >> PAGE_SHIFT;
	int ret;

	ret = kirin710_sync_restore(buffer->pages, count);
	if (ret) {
		pr_err("TrustCore: retaining CMA pages with incomplete direct mappings\n");
		return ret;
	}

	dcache_inval_poc(start, start + buffer->len);
	memzero_explicit((void *)start, buffer->len);
	dcache_clean_inval_poc(start, start + buffer->len);
	if (WARN_ON_ONCE(!cma_release(sync_cma, buffer->pages, count)))
		return -EIO;

	mutex_lock(&sync_heap_lock);
	buffer->heap->used -= buffer->len;
	mutex_unlock(&sync_heap_lock);
	*buffer = (struct kirin710_sync_buffer) {};
	return 0;
}

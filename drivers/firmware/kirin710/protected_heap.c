// SPDX-License-Identifier: GPL-2.0-only
/*
 * TrustCore protected DMA buffers for Kirin 710.
 *
 * Uses the protected-memory protocol from Huawei's 4.9 ION driver.
 */
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "protected_heap.h"
#include "protected_mem.h"
#include "teek_client_api.h"
#include "teek_client_constants.h"

#define KIRIN710_PROTECT_HEAP_ID		20
#define KIRIN710_HEAP_PROTECT		1
#define TRUSTCORE_MEM_COMMAND		1
#define TRUSTCORE_MEM_PGTABLE_INIT	0
#define TRUSTCORE_MEM_ALLOC		1
#define TRUSTCORE_MEM_FREE		2
#define TRUSTCORE_MEM_DRM		0

struct trustcore_pageinfo {
	u64 phys;
	u32 pages;
} __aligned(8);

static_assert(sizeof(struct trustcore_pageinfo) == 16);

struct kirin710_protected_buf {
	unsigned int count;
	u32 handle;
	struct kirin710_sync_buffer chunks[];
};

static TEEC_UUID mem_uuid = {
	0xf8028dca, 0xaba0, 0x11e6,
	{ 0x80, 0xf5, 0x76, 0x30, 0x4d, 0xec, 0x7e, 0xb7 },
};

static DEFINE_MUTEX(media_lock);
static TEEC_Context media_context;
static TEEC_Session media_session;
static struct kirin710_cma_heap media_heap;
static bool media_online;
static bool media_session_open;
static bool media_fault;
static u32 media_pgtable_addr;
static u32 media_pgtable_size;

static int kirin710_media_invoke(TEEC_Operation *op)
{
	TEEC_Result result;
	u32 origin = 0;

	result = TEEK_InvokeCommand(&media_session, TRUSTCORE_MEM_COMMAND, op, &origin);
	if (result != TEEC_SUCCESS) {
		pr_err_ratelimited("TrustCore: protected heap command %u failed (%#x, origin %u)\n",
				   op->params[0].value.a, result, origin);
		return -EIO;
	}
	return 0;
}

static int kirin710_media_open(void)
{
	static char package[] = "sec_mem";
	u32 uid = 2000, origin = 0;
	TEEC_Operation op = {
		.started = 1,
		.paramTypes = TEEC_PARAM_TYPES(TEEC_NONE, TEEC_NONE,
					      TEEC_MEMREF_TEMP_INPUT, TEEC_MEMREF_TEMP_INPUT),
	};
	TEEC_Operation init = {
		.started = 1,
		.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_VALUE_INPUT,
					      TEEC_NONE, TEEC_NONE),
	};
	TEEC_Result result;
	int ret;

	if (media_session_open)
		return 0;

	result = TEEK_InitializeContext(NULL, &media_context);
	if (result != TEEC_SUCCESS)
		return -EIO;

	op.params[2].tmpref.buffer = &uid;
	op.params[2].tmpref.size = sizeof(uid);
	op.params[3].tmpref.buffer = package;
	op.params[3].tmpref.size = sizeof(package);
	result = TEEK_OpenSession(&media_context, &media_session, &mem_uuid,
				  TEEC_LOGIN_IDENTIFY, NULL, &op, &origin);
	if (result != TEEC_SUCCESS) {
		ret = -EIO;
		goto finish_context;
	}

	init.params[0].value.a = TRUSTCORE_MEM_PGTABLE_INIT;
	init.params[0].value.b = TRUSTCORE_MEM_DRM;
	init.params[1].value.a = media_pgtable_addr;
	init.params[1].value.b = media_pgtable_size;
	ret = kirin710_media_invoke(&init);
	if (ret) {
		TEEK_CloseSession(&media_session);
		goto finish_context;
	}
	media_session_open = true;
	return 0;
finish_context:
	TEEK_FinalizeContext(&media_context);
	return ret;
}

static void kirin710_media_restore(struct kirin710_protected_buf *buffer)
{
	unsigned int i;

	for (i = 0; i < buffer->count; i++)
		kirin710_sync_mem_release(&buffer->chunks[i]);
}

static int kirin710_media_free(u32 handle)
{
	TEEC_Operation op = {
		.started = 1,
		.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_VALUE_INPUT,
					      TEEC_NONE, TEEC_NONE),
	};

	op.params[0].value.a = TRUSTCORE_MEM_FREE;
	op.params[0].value.b = TRUSTCORE_MEM_DRM;
	op.params[1].value.a = handle;
	return kirin710_media_invoke(&op);
}

static int kirin710_media_attach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
	/* The generic DMA API cannot pass this buffer's TrustCore handle. */
	return -EOPNOTSUPP;
}

static struct sg_table *kirin710_media_map(struct dma_buf_attachment *attach,
					  enum dma_data_direction direction)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static void kirin710_media_unmap(struct dma_buf_attachment *attach,
				struct sg_table *table, enum dma_data_direction direction)
{
}

static void kirin710_media_release(struct dma_buf *dmabuf)
{
	struct kirin710_protected_buf *buffer = dmabuf->priv;

	mutex_lock(&media_lock);
	if (media_online && !kirin710_media_free(buffer->handle)) {
		kirin710_media_restore(buffer);
	} else {
		media_fault = true;
		pr_err("TrustCore: leaving protected pages reserved after a failed release\n");
	}
	mutex_unlock(&media_lock);
	kfree(buffer);
}

static int kirin710_media_begin_cpu_access(struct dma_buf *dmabuf,
					    enum dma_data_direction direction)
{
	return -EPERM;
}

static const struct dma_buf_ops kirin710_media_buf_ops = {
	.begin_cpu_access = kirin710_media_begin_cpu_access,
	.attach = kirin710_media_attach,
	.map_dma_buf = kirin710_media_map,
	.unmap_dma_buf = kirin710_media_unmap,
	.release = kirin710_media_release,
};

static struct dma_buf *kirin710_media_allocate(struct dma_heap *heap, unsigned long len,
					       u32 fd_flags, u64 heap_flags)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp);
	TEEC_Operation op = {
		.started = 1,
		.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_VALUE_INOUT,
					      TEEC_MEMREF_TEMP_INPUT, TEEC_NONE),
	};
	struct kirin710_protected_buf *buffer;
	struct trustcore_pageinfo *info;
	struct dma_buf *dmabuf;
	unsigned int chunks, i;
	int ret;

	if (!len || len > media_heap.limit || heap_flags)
		return ERR_PTR(-EINVAL);

	chunks = DIV_ROUND_UP(len, SZ_2M);
	buffer = kzalloc(struct_size(buffer, chunks, chunks), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	info = kcalloc(chunks, sizeof(*info), GFP_KERNEL);
	if (!info) {
		kfree(buffer);
		return ERR_PTR(-ENOMEM);
	}

	mutex_lock(&media_lock);
	if (!media_online || media_fault) {
		ret = -EIO;
		goto out_buffer;
	}
	ret = kirin710_media_open();
	if (ret)
		goto out_buffer;

	for (i = 0; i < chunks; i++) {
		ret = kirin710_cma_alloc(&media_heap, SZ_2M, &buffer->chunks[i]);
		if (ret)
			goto restore;
		buffer->count++;
		info[i].phys = buffer->chunks[i].phys;
		info[i].pages = buffer->chunks[i].len >> PAGE_SHIFT;
	}

	op.params[0].value.a = TRUSTCORE_MEM_ALLOC;
	op.params[0].value.b = TRUSTCORE_MEM_DRM;
	op.params[1].value.a = chunks;
	op.params[2].tmpref.buffer = info;
	op.params[2].tmpref.size = chunks * sizeof(*info);
	ret = kirin710_media_invoke(&op);
	if (ret) {
		media_fault = true;
		pr_err("TrustCore: leaving media pages reserved after an uncertain handoff\n");
		goto out_buffer;
	}
	buffer->handle = op.params[1].value.b;

	exp.ops = &kirin710_media_buf_ops;
	exp.size = len;
	exp.flags = fd_flags & O_ACCMODE;
	exp.priv = buffer;
	dmabuf = dma_buf_export(&exp);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		if (kirin710_media_free(buffer->handle)) {
			media_fault = true;
			goto out_buffer;
		}
		goto restore;
	}
	mutex_unlock(&media_lock);
	kfree(info);
	return dmabuf;
restore:
	kirin710_media_restore(buffer);
out_buffer:
	mutex_unlock(&media_lock);
	kfree(buffer);
	kfree(info);
	return ERR_PTR(ret);
}

static const struct dma_heap_ops kirin710_media_heap_ops = {
	.allocate = kirin710_media_allocate,
};

int kirin710_protected_heap_init(void)
{
	static bool registered;
	const struct dma_heap_export_info export = {
		.name = "kirin710-protected",
		.ops = &kirin710_media_heap_ops,
	};
	struct device_node *heap_node, *pgtable;
	struct reserved_mem *mem;
	struct dma_heap *heap;
	int ret;

	mutex_lock(&media_lock);
	if (registered) {
		ret = 0;
		media_online = true;
		goto unlock;
	}

	ret = kirin710_cma_heap_init(KIRIN710_PROTECT_HEAP_ID, KIRIN710_HEAP_PROTECT,
				    &media_heap, &heap_node);
	if (ret)
		goto unlock;
	if (media_heap.order != get_order(SZ_2M)) {
		of_node_put(heap_node);
		ret = -EINVAL;
		goto unlock;
	}

	pgtable = of_parse_phandle(heap_node, "memory-region", 0);
	of_node_put(heap_node);
	if (!pgtable) {
		ret = -ENODEV;
		goto unlock;
	}
	mem = of_reserved_mem_lookup(pgtable);
	if (!mem || !of_property_present(pgtable, "no-map") || !mem->size ||
	    mem->base > U32_MAX || mem->size > U32_MAX ||
	    mem->size - 1 > U32_MAX - mem->base ||
	    !PAGE_ALIGNED(mem->base) || !PAGE_ALIGNED(mem->size)) {
		of_node_put(pgtable);
		ret = -EINVAL;
		goto unlock;
	}
	of_node_put(pgtable);
	media_pgtable_addr = mem->base;
	media_pgtable_size = mem->size;

	heap = dma_heap_add(&export);
	if (IS_ERR(heap)) {
		ret = PTR_ERR(heap);
		goto unlock;
	}
	registered = true;
	media_online = true;
	ret = 0;
unlock:
	mutex_unlock(&media_lock);
	return ret;
}

void kirin710_protected_heap_exit(void)
{
	mutex_lock(&media_lock);
	media_online = false;
	if (media_session_open) {
		TEEK_CloseSession(&media_session);
		TEEK_FinalizeContext(&media_context);
		media_session_open = false;
	}
	mutex_unlock(&media_lock);
}

struct dma_buf *kirin710_protected_get(int fd, u32 *handle, size_t *size)
{
	struct dma_buf *dmabuf;
	struct kirin710_protected_buf *buffer;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return dmabuf;
	if (dmabuf->ops != &kirin710_media_buf_ops) {
		dma_buf_put(dmabuf);
		return ERR_PTR(-EINVAL);
	}

	buffer = dmabuf->priv;
	*handle = buffer->handle;
	*size = dmabuf->size;
	return dmabuf;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Legacy ION handle ioctls for Kirin 710 vendor graphics libraries.
 * DMA-BUF heaps own the pages and their CPU/DMA synchronization.
 */
#include <linux/cma.h>
#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/dma-map-ops.h>
#include <linux/file.h>
#include <linux/idr.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/refcount.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "vendor-headers/drivers/staging/android/uapi/ion.h"

#define KIRIN710_ION_SYSTEM	BIT(0)
#define KIRIN710_ION_GRALLOC	BIT(2)
#define KIRIN710_ION_PROTECTED	BIT(20)

struct kirin710_ion_handle {
	struct dma_buf *buf;
	refcount_t refs;
};

struct kirin710_ion_file {
	struct mutex lock;
	struct idr handles;
};

struct kirin710_ion_alloc32 {
	u32 len;
	u32 align;
	u32 heap_id_mask;
	u32 flags;
	s32 handle;
};

#define KIRIN710_ION_ALLOC32 _IOWR(ION_IOC_MAGIC, 0, struct kirin710_ion_alloc32)

struct kirin710_ion_custom32 {
	u32 cmd;
	u32 arg;
};

struct kirin710_ion_flush {
	s32 fd;
	unsigned long addr;
	u32 offset;
	u32 length;
};

struct kirin710_ion_flush32 {
	s32 fd;
	u32 addr;
	u32 offset;
	u32 length;
};

#define KIRIN710_ION_CUSTOM32 _IOWR(ION_IOC_MAGIC, 6, struct kirin710_ion_custom32)

static int kirin710_ion_open(struct inode *inode, struct file *file)
{
	struct kirin710_ion_file *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	mutex_init(&client->lock);
	idr_init(&client->handles);
	file->private_data = client;
	return 0;
}

static void kirin710_ion_handle_put(struct kirin710_ion_handle *handle)
{
	dma_buf_put(handle->buf);
	kfree(handle);
}

static int kirin710_ion_close(struct inode *inode, struct file *file)
{
	struct kirin710_ion_file *client = file->private_data;
	struct kirin710_ion_handle *handle;
	int id;

	idr_for_each_entry(&client->handles, handle, id)
		kirin710_ion_handle_put(handle);
	idr_destroy(&client->handles);
	kfree(client);
	return 0;
}

static int kirin710_ion_insert(struct kirin710_ion_file *client, struct dma_buf *buf)
{
	struct kirin710_ion_handle *handle, *entry;
	int id;

	handle = kmalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;
	handle->buf = buf;
	refcount_set(&handle->refs, 1);

	mutex_lock(&client->lock);
	idr_for_each_entry(&client->handles, entry, id) {
		if (entry->buf == buf) {
			refcount_inc(&entry->refs);
			mutex_unlock(&client->lock);
			kfree(handle);
			dma_buf_put(buf);
			return id;
		}
	}
	id = idr_alloc(&client->handles, handle, 1, 0, GFP_KERNEL);
	mutex_unlock(&client->lock);
	if (id < 0)
		kfree(handle);
	return id;
}

static int kirin710_ion_free(struct kirin710_ion_file *client, int id)
{
	struct kirin710_ion_handle *handle;
	bool release;

	mutex_lock(&client->lock);
	handle = idr_find(&client->handles, id);
	if (!handle) {
		mutex_unlock(&client->lock);
		return -EINVAL;
	}
	release = refcount_dec_and_test(&handle->refs);
	if (release)
		idr_remove(&client->handles, id);
	mutex_unlock(&client->lock);

	if (release)
		kirin710_ion_handle_put(handle);
	return 0;
}

static struct dma_buf *kirin710_ion_heap_alloc(const char *name, size_t size)
{
	struct dma_heap *heap;
	struct dma_buf *buf;

	heap = dma_heap_find(name);
	if (!heap)
		return ERR_PTR(-ENODEV);
	buf = dma_heap_buffer_alloc(heap, size, O_RDWR | O_CLOEXEC, 0);
	dma_heap_put(heap);
	return buf;
}

static struct dma_buf *kirin710_ion_alloc(size_t size, size_t align, u32 mask, u32 flags)
{
	struct cma *cma;
	struct dma_buf *buf = ERR_PTR(-EOPNOTSUPP);
	const char *system_heap = flags & ION_FLAG_CACHED ? "system" : "system-uncached";

	if (!size || size > SIZE_MAX - PAGE_SIZE + 1 ||
	    (align && !is_power_of_2(align)))
		return ERR_PTR(-EINVAL);

	if (flags & ION_FLAG_SECURE_BUFFER || mask & KIRIN710_ION_PROTECTED) {
		if (!(mask & KIRIN710_ION_PROTECTED) || align > SZ_2M)
			return ERR_PTR(-EOPNOTSUPP);
		return kirin710_ion_heap_alloc("kirin710-protected", size);
	}

	if (align > PAGE_SIZE)
		return ERR_PTR(-EOPNOTSUPP);

	if (mask & KIRIN710_ION_GRALLOC && flags & ION_FLAG_CACHED) {
		cma = dev_get_cma_area(NULL);
		if (cma) {
			buf = kirin710_ion_heap_alloc(cma_get_name(cma), size);
			if (!IS_ERR(buf))
				return buf;
		}
	}
	if (mask & KIRIN710_ION_SYSTEM)
		return kirin710_ion_heap_alloc(system_heap, size);
	return buf;
}

static int kirin710_ion_new_handle(struct kirin710_ion_file *client,
				   struct dma_buf *buf)
{
	int id;

	if (IS_ERR(buf))
		return PTR_ERR(buf);

	id = kirin710_ion_insert(client, buf);
	if (id < 0)
		dma_buf_put(buf);
	return id;
}

static int kirin710_ion_alloc_user(struct kirin710_ion_file *client,
				   void __user *user, bool compat)
{
	struct ion_allocation_data data = {};
	struct kirin710_ion_alloc32 data32;
	struct dma_buf *buf;
	int id;

	if (compat) {
		if (copy_from_user(&data32, user, sizeof(data32)))
			return -EFAULT;
		data.len = data32.len;
		data.align = data32.align;
		data.heap_id_mask = data32.heap_id_mask;
		data.flags = data32.flags;
	} else if (copy_from_user(&data, user, sizeof(data))) {
		return -EFAULT;
	}

	buf = kirin710_ion_alloc(data.len, data.align, data.heap_id_mask, data.flags);
	id = kirin710_ion_new_handle(client, buf);
	if (id < 0)
		return id;

	data.handle = id;
	data32.handle = id;
	if (compat ? copy_to_user(user, &data32, sizeof(data32)) :
		     copy_to_user(user, &data, sizeof(data))) {
		kirin710_ion_free(client, id);
		return -EFAULT;
	}
	return 0;
}

static int kirin710_ion_share(struct kirin710_ion_file *client,
			      struct ion_fd_data *data, void __user *user)
{
	struct kirin710_ion_handle *handle;
	struct dma_buf *buf = NULL;
	int fd;

	mutex_lock(&client->lock);
	handle = idr_find(&client->handles, data->handle);
	if (handle) {
		buf = handle->buf;
		get_dma_buf(buf);
	}
	mutex_unlock(&client->lock);
	if (!buf)
		return -EINVAL;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		dma_buf_put(buf);
		return fd;
	}
	data->fd = fd;
	if (copy_to_user(user, data, sizeof(*data))) {
		put_unused_fd(fd);
		dma_buf_put(buf);
		return -EFAULT;
	}
	fd_install(fd, buf->file);
	return 0;
}

static int kirin710_ion_sync(int fd, u32 offset, size_t length,
			     enum dma_data_direction direction)
{
	struct dma_buf *buf = dma_buf_get(fd);
	int ret;

	if (IS_ERR(buf))
		return PTR_ERR(buf);
	if (offset > buf->size || length > buf->size - offset) {
		ret = -EINVAL;
		goto put;
	}

	ret = dma_buf_begin_cpu_access(buf, direction);
	if (!ret)
		ret = dma_buf_end_cpu_access(buf, direction);
put:
	dma_buf_put(buf);
	return ret;
}

static int kirin710_ion_custom(void __user *user, bool compat)
{
	struct ion_custom_data cmd;
	struct kirin710_ion_custom32 cmd32;
	struct kirin710_ion_flush flush;
	struct kirin710_ion_flush32 flush32;
	enum dma_data_direction direction;

	if (compat) {
		if (copy_from_user(&cmd32, user, sizeof(cmd32)))
			return -EFAULT;
		cmd.cmd = cmd32.cmd;
		cmd.arg = cmd32.arg;
	} else if (copy_from_user(&cmd, user, sizeof(cmd))) {
		return -EFAULT;
	}

	switch (cmd.cmd) {
	case 1: /* ION_HISI_CLEAN_CACHES */
		direction = DMA_TO_DEVICE;
		break;
	case 2: /* ION_HISI_INV_CACHES */
		direction = DMA_FROM_DEVICE;
		break;
	case 3: /* ION_HISI_CLEAN_INV_CACHES */
		direction = DMA_BIDIRECTIONAL;
		break;
	default:
		return -ENOTTY;
	}

	if (compat) {
		if (copy_from_user(&flush32, compat_ptr(cmd.arg), sizeof(flush32)))
			return -EFAULT;
		return kirin710_ion_sync(flush32.fd, flush32.offset, flush32.length, direction);
	}
	if (copy_from_user(&flush, (void __user *)cmd.arg, sizeof(flush)))
		return -EFAULT;
	return kirin710_ion_sync(flush.fd, flush.offset, flush.length, direction);
}

static long kirin710_ion_user_ioctl(struct file *file, unsigned int cmd,
				    void __user *user, bool compat)
{
	struct kirin710_ion_file *client = file->private_data;
	struct ion_handle_data handle;
	struct ion_fd_data data;
	int id;

	if (cmd == (compat ? KIRIN710_ION_ALLOC32 : ION_IOC_ALLOC))
		return kirin710_ion_alloc_user(client, user, compat);
	if (cmd == (compat ? KIRIN710_ION_CUSTOM32 : ION_IOC_CUSTOM))
		return kirin710_ion_custom(user, compat);

	switch (cmd) {
	case ION_IOC_FREE:
		if (copy_from_user(&handle, user, sizeof(handle)))
			return -EFAULT;
		return kirin710_ion_free(client, handle.handle);

	case ION_IOC_MAP:
	case ION_IOC_SHARE:
		if (copy_from_user(&data, user, sizeof(data)))
			return -EFAULT;
		return kirin710_ion_share(client, &data, user);

	case ION_IOC_IMPORT:
		if (copy_from_user(&data, user, sizeof(data)))
			return -EFAULT;
		id = kirin710_ion_new_handle(client, dma_buf_get(data.fd));
		if (id < 0)
			return id;
		data.handle = id;
		if (copy_to_user(user, &data, sizeof(data))) {
			kirin710_ion_free(client, id);
			return -EFAULT;
		}
		return 0;

	case ION_IOC_SYNC:
	case ION_IOC_INV:
		if (copy_from_user(&data, user, sizeof(data)))
			return -EFAULT;
		return kirin710_ion_sync(data.fd, 0, 0,
			cmd == ION_IOC_INV ? DMA_FROM_DEVICE : DMA_BIDIRECTIONAL);
	default:
		return -ENOTTY;
	}
}

static long kirin710_ion_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return kirin710_ion_user_ioctl(file, cmd, (void __user *)arg, false);
}

#ifdef CONFIG_COMPAT
static long kirin710_ion_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return kirin710_ion_user_ioctl(file, cmd, compat_ptr(arg), true);
}
#endif

static const struct file_operations kirin710_ion_fops = {
	.owner = THIS_MODULE,
	.open = kirin710_ion_open,
	.release = kirin710_ion_close,
	.unlocked_ioctl = kirin710_ion_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = kirin710_ion_compat_ioctl,
#endif
};

static struct miscdevice kirin710_ion_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ion",
	.fops = &kirin710_ion_fops,
};

static int __init kirin710_ion_init(void)
{
	if (!of_machine_is_compatible("hisilicon,kirin710"))
		return -ENODEV;
	return misc_register(&kirin710_ion_device);
}
device_initcall(kirin710_ion_init);

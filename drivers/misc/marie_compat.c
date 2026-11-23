// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 apkreader
 *
 * Huawei P30 lite (marie / Kirin 710) board and Android 17 compatibility:
 *  - Early simple-framebuffer registration & deferred GPU/SMP bring-up
 *  - Jitterentropy-backed hwrng
 *  - Android /dev/ashmem miscdevice
 *  - Backlight LED classdev (/sys/class/leds/lcd_backlight0)
 *  - Minimal /dev/sensorhub + huawei_sensor sysfs stub
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/string.h>
/* Kirin 710: jitterentropy-backed hwrng + deferred simpledrm, Panfrost GPU, and 8-core SMP */
#include <linux/random.h>
#include <linux/hw_random.h>
#include <crypto/rng.h>
#include <linux/platform_device.h>
#include <linux/platform_data/simplefb.h>
#include <linux/of.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>

extern void marie_register_gpu_power_deferred(void);
extern void marie_smc_init_secondary_cpus(void);

static struct crypto_rng *marie_jent_tfm;

static int marie_jent_hwrng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	size_t len = min_t(size_t, max, 64);
	int ret;

	if (!marie_jent_tfm)
		return -ENODEV;
	ret = crypto_rng_get_bytes(marie_jent_tfm, data, len);
	if (ret == -EAGAIN)
		ret = crypto_rng_get_bytes(marie_jent_tfm, data, len);
	if (ret < 0)
		return ret;
	return len;
}

static struct hwrng marie_jent_hwrng = {
	.name = "kirin710-jitter",
	.read = marie_jent_hwrng_read,
	.quality = 512,
};

static struct delayed_work marie_drm_work;

static void marie_drm_work_fn(struct work_struct *work)
{
	unsigned int cpu;

	if (!of_machine_is_compatible("hisilicon,kirin710"))
		return;

	marie_register_gpu_power_deferred();

	for_each_present_cpu(cpu) {
		if (cpu == 0 || cpu_online(cpu))
			continue;
		add_cpu(cpu);
	}
	pr_info("MARIE_SMP: online=%*pbl\n", cpumask_pr_args(cpu_online_mask));
	marie_smc_init_secondary_cpus();
}

static int __init marie_jent_init(void)
{
	static const struct simplefb_platform_data pd = {
		.width = 1080,
		.height = 2312,
		.stride = 1080 * 4,
		.format = "x8r8g8b8",
	};
	static const struct resource res = {
		.start = 0x18c00000,
		.end = 0x18c00000 + (1080 * 2312 * 4) - 1,
		.flags = IORESOURCE_MEM,
	};
	struct platform_device *pdev;
	int ret;

	if (of_machine_is_compatible("hisilicon,kirin710")) {
		pdev = platform_device_register_resndata(NULL, "simple-framebuffer", 0,
							 &res, 1, &pd, sizeof(pd));
		pr_info("MARIE_DRM: registered early simple-framebuffer pdev=%pe\n", pdev);
	}

	marie_jent_tfm = crypto_alloc_rng("jitterentropy_rng", 0, 0);
	if (!IS_ERR(marie_jent_tfm)) {
		ret = hwrng_register(&marie_jent_hwrng);
		pr_info("MARIE_HWRNG: hwrng_register(kirin710-jitter) = %d\n", ret);
	}
	INIT_DELAYED_WORK(&marie_drm_work, marie_drm_work_fn);
	schedule_delayed_work(&marie_drm_work, msecs_to_jiffies(4000));
	return 0;
}
late_initcall(marie_jent_init);

/* Android ashmem C miscdevice (replaces Rust-only CONFIG_ASHMEM for Android 17 /dev/ashmem<boot_id>) */
#include <linux/miscdevice.h>
#include <linux/shmem_fs.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/compat.h>
#include <linux/seq_file.h>

#define MARIE_ASHMEM_NAME_LEN 256
#define MARIE_ASHMEM_IOC 0x77
#define MARIE_ASHMEM_SET_NAME        _IOW(MARIE_ASHMEM_IOC, 1, char[MARIE_ASHMEM_NAME_LEN])
#define MARIE_ASHMEM_GET_NAME        _IOR(MARIE_ASHMEM_IOC, 2, char[MARIE_ASHMEM_NAME_LEN])
#define MARIE_ASHMEM_SET_SIZE        _IOW(MARIE_ASHMEM_IOC, 3, size_t)
#define MARIE_ASHMEM_GET_SIZE        _IO(MARIE_ASHMEM_IOC, 4)
#define MARIE_ASHMEM_SET_PROT_MASK   _IOW(MARIE_ASHMEM_IOC, 5, unsigned long)
#define MARIE_ASHMEM_GET_PROT_MASK   _IO(MARIE_ASHMEM_IOC, 6)
#define MARIE_ASHMEM_PIN             _IOW(MARIE_ASHMEM_IOC, 7, u64)
#define MARIE_ASHMEM_UNPIN           _IOW(MARIE_ASHMEM_IOC, 8, u64)
#define MARIE_ASHMEM_GET_PIN_STATUS  _IO(MARIE_ASHMEM_IOC, 9)
#define MARIE_ASHMEM_PURGE_ALL       _IO(MARIE_ASHMEM_IOC, 10)
#define MARIE_ASHMEM_GET_FILE_ID     _IOR(MARIE_ASHMEM_IOC, 11, unsigned long)
#define MARIE_COMPAT_ASHMEM_SET_SIZE      _IOW(MARIE_ASHMEM_IOC, 3, u32)
#define MARIE_COMPAT_ASHMEM_SET_PROT_MASK _IOW(MARIE_ASHMEM_IOC, 5, u32)
#define MARIE_COMPAT_ASHMEM_GET_FILE_ID   _IOR(MARIE_ASHMEM_IOC, 11, u32)

struct marie_ashmem_area {
	struct mutex lock;
	char name[MARIE_ASHMEM_NAME_LEN + 16];
	struct file *file;
	size_t size;
	unsigned long prot_mask;
};

static int marie_ashmem_open(struct inode *inode, struct file *file)
{
	struct marie_ashmem_area *asma;

	asma = kzalloc(sizeof(*asma), GFP_KERNEL);
	if (!asma)
		return -ENOMEM;
	mutex_init(&asma->lock);
	strscpy(asma->name, "dev/ashmem", sizeof(asma->name));
	asma->prot_mask = PROT_READ | PROT_WRITE | PROT_EXEC;
	file->private_data = asma;
	return 0;
}

static int marie_ashmem_release(struct inode *inode, struct file *file)
{
	struct marie_ashmem_area *asma = file->private_data;

	if (asma) {
		if (asma->file)
			fput(asma->file);
		kfree(asma);
	}
	return 0;
}

static ssize_t marie_ashmem_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct marie_ashmem_area *asma = iocb->ki_filp->private_data;
	struct file *vmfile;
	ssize_t ret;

	mutex_lock(&asma->lock);
	if (!asma->size) {
		mutex_unlock(&asma->lock);
		return 0;
	}
	if (!asma->file) {
		mutex_unlock(&asma->lock);
		return -EBADF;
	}
	vmfile = get_file(asma->file);
	mutex_unlock(&asma->lock);

	ret = vfs_iter_read(vmfile, iter, &iocb->ki_pos, 0);
	fput(vmfile);
	return ret;
}

static loff_t marie_ashmem_llseek(struct file *file, loff_t offset, int origin)
{
	struct marie_ashmem_area *asma = file->private_data;
	struct file *vmfile;
	loff_t ret;

	mutex_lock(&asma->lock);
	if (!asma->size) {
		mutex_unlock(&asma->lock);
		return -EINVAL;
	}
	if (!asma->file) {
		mutex_unlock(&asma->lock);
		return -EBADF;
	}
	vmfile = get_file(asma->file);
	mutex_unlock(&asma->lock);

	ret = vfs_llseek(vmfile, offset, origin);
	if (ret >= 0)
		file->f_pos = ret;
	fput(vmfile);
	return ret;
}

static int marie_ashmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct marie_ashmem_area *asma = file->private_data;
	int ret = 0;

	mutex_lock(&asma->lock);
	if (!asma->size || asma->size >= (SIZE_MAX >> 1)) {
		ret = -EINVAL;
		goto out;
	}
	if ((vma->vm_end - vma->vm_start) > PAGE_ALIGN(asma->size)) {
		ret = -EINVAL;
		goto out;
	}
	if (!(asma->prot_mask & PROT_WRITE)) {
		if (vma->vm_flags & VM_WRITE) {
			ret = -EPERM;
			goto out;
		}
		vm_flags_clear(vma, VM_MAYWRITE);
	}
	if (!(asma->prot_mask & PROT_EXEC)) {
		if (vma->vm_flags & VM_EXEC) {
			ret = -EPERM;
			goto out;
		}
		vm_flags_clear(vma, VM_MAYEXEC);
	}
	if (!(asma->prot_mask & PROT_READ)) {
		if (vma->vm_flags & VM_READ) {
			ret = -EPERM;
			goto out;
		}
		vm_flags_clear(vma, VM_MAYREAD);
	}
	if (!asma->file) {
		struct file *vmfile = shmem_file_setup(asma->name, asma->size, vma->vm_flags);
		if (IS_ERR(vmfile)) {
			ret = PTR_ERR(vmfile);
			goto out;
		}
		vmfile->f_mode |= FMODE_LSEEK;
		asma->file = vmfile;
	}
	if (vma->vm_flags & VM_SHARED) {
		ret = shmem_zero_setup(vma);
		if (ret)
			goto out;
	} else {
		vma_set_anonymous(vma);
	}
	vma_set_file(vma, asma->file);
out:
	mutex_unlock(&asma->lock);
	return ret;
}

static long marie_ashmem_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct marie_ashmem_area *asma = file->private_data;
	long ret = -ENOTTY;

	switch (cmd) {
	case MARIE_ASHMEM_SET_NAME: {
		char local_name[MARIE_ASHMEM_NAME_LEN];
		if (strncpy_from_user(local_name, (const char __user *)arg, MARIE_ASHMEM_NAME_LEN) < 0)
			return -EFAULT;
		local_name[MARIE_ASHMEM_NAME_LEN - 1] = '\0';
		mutex_lock(&asma->lock);
		if (asma->file) {
			ret = -EINVAL;
		} else {
			snprintf(asma->name, sizeof(asma->name), "dev/ashmem/%s", local_name);
			ret = 0;
		}
		mutex_unlock(&asma->lock);
		break;
	}
	case MARIE_ASHMEM_GET_NAME: {
		char local_name[MARIE_ASHMEM_NAME_LEN];
		size_t len;
		mutex_lock(&asma->lock);
		if (str_has_prefix(asma->name, "dev/ashmem/"))
			strscpy(local_name, asma->name + 11, sizeof(local_name));
		else
			strscpy(local_name, "dev/ashmem", sizeof(local_name));
		mutex_unlock(&asma->lock);
		len = strlen(local_name) + 1;
		ret = copy_to_user((void __user *)arg, local_name, len) ? -EFAULT : 0;
		break;
	}
	case MARIE_ASHMEM_SET_SIZE:
		mutex_lock(&asma->lock);
		if (!asma->file && arg < (SIZE_MAX >> 1)) {
			asma->size = arg;
			ret = 0;
		} else {
			ret = -EINVAL;
		}
		mutex_unlock(&asma->lock);
		break;
	case MARIE_ASHMEM_GET_SIZE:
		ret = asma->size;
		break;
	case MARIE_ASHMEM_SET_PROT_MASK: {
		unsigned long prot = arg & (PROT_READ | PROT_WRITE | PROT_EXEC);
		mutex_lock(&asma->lock);
		if ((asma->prot_mask & prot) != prot) {
			ret = -EINVAL;
		} else {
			asma->prot_mask = prot;
			ret = 0;
		}
		mutex_unlock(&asma->lock);
		break;
	}
	case MARIE_ASHMEM_GET_PROT_MASK:
		ret = asma->prot_mask;
		break;
	case MARIE_ASHMEM_PIN:
	case MARIE_ASHMEM_UNPIN:
	case MARIE_ASHMEM_PURGE_ALL:
		ret = 0;
		break;
	case MARIE_ASHMEM_GET_PIN_STATUS:
		ret = 1;
		break;
	case MARIE_ASHMEM_GET_FILE_ID: {
		unsigned long id = 0;
		mutex_lock(&asma->lock);
		if (!asma->file && asma->size) {
			struct file *vmfile = shmem_file_setup(asma->name, asma->size, 0);
			if (!IS_ERR(vmfile)) {
				vmfile->f_mode |= FMODE_LSEEK;
				asma->file = vmfile;
			}
		}
		if (asma->file)
			id = file_inode(asma->file)->i_ino;
		mutex_unlock(&asma->lock);
		ret = id ? (copy_to_user((void __user *)arg, &id, sizeof(id)) ? -EFAULT : 0) : -EINVAL;
		break;
	}
	}
	return ret;
}

static long marie_ashmem_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (cmd == MARIE_COMPAT_ASHMEM_SET_SIZE)
		return marie_ashmem_ioctl(file, MARIE_ASHMEM_SET_SIZE, arg);
	if (cmd == MARIE_COMPAT_ASHMEM_SET_PROT_MASK)
		return marie_ashmem_ioctl(file, MARIE_ASHMEM_SET_PROT_MASK, arg);
	if (cmd == MARIE_COMPAT_ASHMEM_GET_FILE_ID) {
		struct marie_ashmem_area *asma = file->private_data;
		u32 id = 0;
		mutex_lock(&asma->lock);
		if (asma->file)
			id = (u32)file_inode(asma->file)->i_ino;
		mutex_unlock(&asma->lock);
		return id ? (copy_to_user(compat_ptr(arg), &id, sizeof(id)) ? -EFAULT : 0) : -EINVAL;
	}
	return marie_ashmem_ioctl(file, cmd, arg);
}

static void marie_ashmem_show_fdinfo(struct seq_file *m, struct file *file)
{
	struct marie_ashmem_area *asma = file->private_data;

	mutex_lock(&asma->lock);
	if (asma->file)
		seq_printf(m, "inode:\t%lu\n", file_inode(asma->file)->i_ino);
	seq_printf(m, "name:\t%s\n", asma->name);
	mutex_unlock(&asma->lock);
}

static const struct file_operations marie_ashmem_fops = {
	.owner = THIS_MODULE,
	.open = marie_ashmem_open,
	.release = marie_ashmem_release,
	.read_iter = marie_ashmem_read_iter,
	.llseek = marie_ashmem_llseek,
	.mmap = marie_ashmem_mmap,
	.unlocked_ioctl = marie_ashmem_ioctl,
	.compat_ioctl = marie_ashmem_compat_ioctl,
	.show_fdinfo = marie_ashmem_show_fdinfo,
};

static struct miscdevice marie_ashmem_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ashmem",
	.fops = &marie_ashmem_fops,
	.mode = 0666,
};

static int __init marie_ashmem_init(void)
{
	int ret = misc_register(&marie_ashmem_misc);
	pr_info("MARIE_ASHMEM: misc_register(ashmem) = %d\n", ret);
	return ret;
}
device_initcall(marie_ashmem_init);

/* Backlight LED classdev stub (/sys/class/leds/lcd_backlight0) for android.hardware.lights-service.marie */
#include <linux/leds.h>

extern int marie_lm36274_set_brightness(unsigned int brightness);

static int marie_backlight_set_blocking(struct led_classdev *led_cdev, enum led_brightness value)
{
	led_cdev->brightness = value;
	return marie_lm36274_set_brightness((unsigned int)value);
}

static struct led_classdev marie_backlight_led = {
	.name = "lcd_backlight0",
	.brightness = 255,
	.max_brightness = 255,
	.brightness_set_blocking = marie_backlight_set_blocking,
};

static int __init marie_backlight_init(void)
{
	int ret = led_classdev_register(NULL, &marie_backlight_led);
	pr_info("MARIE_LED: led_classdev_register(lcd_backlight0) = %d\n", ret);
	return ret;
}
device_initcall(marie_backlight_init);

/* Minimal /dev/sensorhub + huawei_sensor platform stub so vendor.huawei.hardware.sensors@1.0-service registers cleanly */
#include <linux/poll.h>
#include <linux/wait.h>

static DECLARE_WAIT_QUEUE_HEAD(marie_sensorhub_wq);

static int marie_sensorhub_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int marie_sensorhub_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t marie_sensorhub_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	if (file->f_flags & O_NONBLOCK)
		return -EAGAIN;
	if (wait_event_interruptible(marie_sensorhub_wq, 0))
		return -ERESTARTSYS;
	return 0;
}

static __poll_t marie_sensorhub_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &marie_sensorhub_wq, wait);
	return 0;
}

static long marie_sensorhub_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return 0;
}

static const struct file_operations marie_sensorhub_fops = {
	.owner = THIS_MODULE,
	.open = marie_sensorhub_open,
	.release = marie_sensorhub_release,
	.read = marie_sensorhub_read,
	.poll = marie_sensorhub_poll,
	.unlocked_ioctl = marie_sensorhub_ioctl,
	.compat_ioctl = marie_sensorhub_ioctl,
};

static struct miscdevice marie_sensorhub_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "sensorhub",
	.fops = &marie_sensorhub_fops,
	.mode = 0666,
};

static ssize_t sensor_list_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return 0;
}
static DEVICE_ATTR_RO(sensor_list_info);

static ssize_t hifi_supported_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0\n");
}
static DEVICE_ATTR_RO(hifi_supported);

static struct attribute *marie_huawei_sensor_attrs[] = {
	&dev_attr_sensor_list_info.attr,
	&dev_attr_hifi_supported.attr,
	NULL,
};
ATTRIBUTE_GROUPS(marie_huawei_sensor);

static int __init marie_sensorhub_init(void)
{
	struct platform_device *pdev;
	int ret = misc_register(&marie_sensorhub_misc);
	pr_info("MARIE_SENSORHUB: misc_register(sensorhub) = %d\n", ret);
	pdev = platform_device_register_simple("huawei_sensor", PLATFORM_DEVID_NONE, NULL, 0);
	if (!IS_ERR(pdev)) {
		int sret = device_add_groups(&pdev->dev, marie_huawei_sensor_groups);
		pr_info("MARIE_SENSORHUB: huawei_sensor sysfs = %d\n", sret);
	}
	return ret;
}
device_initcall(marie_sensorhub_init);

MODULE_AUTHOR("apkreader");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Huawei P30 lite (marie) Kirin 710 board and Android compatibility");

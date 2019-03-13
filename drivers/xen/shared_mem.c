#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/miscdevice.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/bootmem.h>
#include <linux/pagemap.h>
#include <linux/dma-buf.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/gfp.h>
#include <linux/notifier.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/percpu-defs.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>

#include <asm/xen/hypervisor.h>
#include <asm/xen/hypercall.h>

#include <xen/xen.h>
#include <xen/interface/xen.h>
#include <xen/features.h>

static unsigned long mem_addr;
static unsigned long mem_size;
static void *mem_virt;
static struct dma_buf *dmabuf;

static struct sg_table *xen_map_dma_buf(struct dma_buf_attachment *attachment,
				    enum dma_data_direction direction)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
	return NULL;
}

static void xen_unmap_dma_buf(struct dma_buf_attachment *attachment,
			      struct sg_table *table,
			      enum dma_data_direction direction)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
}

static void xen_map_vma_open(struct vm_area_struct *vma)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
}

static void xen_map_vma_close(struct vm_area_struct *vma)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
}

static struct page *xen_map_vma_find_special_page(struct vm_area_struct *vma,
						 unsigned long addr)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
	return NULL;
}

static const struct vm_operations_struct xen_map_vmops = {
	.open = xen_map_vma_open,
	.close = xen_map_vma_close,
	.find_special_page = xen_map_vma_find_special_page,
};

static int xen_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	int err, i;
	u64 start = vma->vm_pgoff << PAGE_SHIFT;
	u64 len = vma->vm_end - vma->vm_start;


	printk("DEBUG %s %d start=%llx len=%llx\n",__func__,__LINE__,start,len);

	vma->vm_ops = &xen_map_vmops;
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_PFNMAP;

	for (i = 0; i < len>>PAGE_SHIFT; i++) {
		err = vm_insert_pfn(vma, vma->vm_start + i*PAGE_SIZE, (mem_addr + i*PAGE_SIZE) >> PAGE_SHIFT);
		if (err) {
			printk("DEBUG %s %d err=%d i=%d\n",__func__,__LINE__,err,i);
		}
	}
	return 0;
}

static void xen_dma_buf_release(struct dma_buf *dmabuf)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
}

static void *xen_dma_buf_kmap(struct dma_buf *dmabuf, unsigned long offset)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
	return mem_virt;
}

static void xen_dma_buf_kunmap(struct dma_buf *dmabuf, unsigned long offset,
			       void *ptr)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
}

static int xen_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction direction)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
	return 0;
}

static int xen_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
				      enum dma_data_direction direction)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
	return 0;
}

static int xen_dma_buf_attach(struct dma_buf *dmabuf, struct device *dev,
			      struct dma_buf_attachment *attachment)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
	return -1;
}

static void xen_dma_buf_detatch(struct dma_buf *dmabuf,
				struct dma_buf_attachment *attachment)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
}

static const struct dma_buf_ops xen_dma_buf_ops = {
	.map_dma_buf = xen_map_dma_buf,
	.unmap_dma_buf = xen_unmap_dma_buf,
	.mmap = xen_mmap,
	.release = xen_dma_buf_release,
	.attach = xen_dma_buf_attach,
	.detach = xen_dma_buf_detatch,
	.begin_cpu_access = xen_dma_buf_begin_cpu_access,
	.end_cpu_access = xen_dma_buf_end_cpu_access,
	.map_atomic = xen_dma_buf_kmap,
	.unmap_atomic = xen_dma_buf_kunmap,
	.map = xen_dma_buf_kmap,
	.unmap = xen_dma_buf_kunmap,
};

static long xen_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int fd;
	
	printk("DEBUG %s %d\n",__func__,__LINE__);
	fd = dma_buf_fd(dmabuf, 0);
	if (fd < 0) {
		printk("DEBUG %s %d\n",__func__,__LINE__);
		return -1;
	}

	if (copy_to_user((void __user *)arg, &fd, 4)) {
		printk("DEBUG %s %d\n",__func__,__LINE__);
		return -EFAULT;
	}
	return 0;
}

static int xen_open(struct inode *a, struct file *b)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
	return 0;
}

static const struct file_operations xen_fops = {
	.owner          = THIS_MODULE,
	.open           = xen_open,
	.unlocked_ioctl = xen_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= xen_ioctl,
#endif
};

static struct miscdevice xen_shared_miscdev = {
	.name = "xen_mem",
	.minor = MISC_DYNAMIC_MINOR,
	.fops = &xen_fops,
	.mode = S_IRWXUGO,
};

static int __init shared_mem_init(void)
{
	int rc, ret;
	char *str;
	struct resource r;
	struct device_node *np = of_find_compatible_node(NULL, NULL, "xen,shared-memory-v1");
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	if (np == NULL) {
		printk("DEBUG %s %d\n",__func__,__LINE__);
		return -ENODEV;
	}

	rc = of_address_to_resource(np, 0, &r);
	if (rc < 0) {
		printk("DEBUG %s %d\n",__func__,__LINE__);
		return -EINVAL;
	}

	str = memremap(r.start, resource_size(&r), MEMREMAP_WB);
	if (str == NULL) {
		printk("DEBUG %s %d\n",__func__,__LINE__);
		return -EFAULT;
	}
	mem_addr = r.start;
	mem_size = resource_size(&r);
	mem_virt = str;

	exp_info.ops = &xen_dma_buf_ops;
	exp_info.size = mem_size;
	exp_info.flags = O_RDWR;
	exp_info.priv = &mem_addr;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		printk("DEBUG %s %d\n",__func__,__LINE__);
		return -EFAULT;
	}

	ret = misc_register(&xen_shared_miscdev);
	if (ret < 0) {
		printk("DEBUG %s %d\n",__func__,__LINE__);
		return -EFAULT;
	}

	return 0;
}
device_initcall(shared_mem_init);

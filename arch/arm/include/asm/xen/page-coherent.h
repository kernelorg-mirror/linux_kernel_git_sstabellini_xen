#ifndef _ASM_ARM_XEN_PAGE_COHERENT_H
#define _ASM_ARM_XEN_PAGE_COHERENT_H

#include <asm/page.h>
#include <linux/dma-attrs.h>
#include <linux/dma-mapping.h>

static inline void *xen_alloc_coherent_pages(struct device *hwdev, size_t size,
		dma_addr_t *dma_handle, gfp_t flags,
		struct dma_attrs *attrs)
{
	return arm_dma_ops.alloc(hwdev, size, dma_handle, flags, attrs);
}

static inline void xen_free_coherent_pages(struct device *hwdev, size_t size,
		void *cpu_addr, dma_addr_t dma_handle,
		struct dma_attrs *attrs)
{
	return arm_dma_ops.free(hwdev, size, cpu_addr, dma_handle, attrs);
}

#endif /* _ASM_ARM_XEN_PAGE_COHERENT_H */

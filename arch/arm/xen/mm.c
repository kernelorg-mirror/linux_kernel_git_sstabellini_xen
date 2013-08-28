#include <linux/bootmem.h>
#include <linux/gfp.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <linux/vmalloc.h>
#include <linux/swiotlb.h>

#include <xen/xen.h>
#include <xen/interface/memory.h>
#include <xen/swiotlb-xen.h>

#include <asm/cacheflush.h>
#include <asm/xen/page.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/interface.h>

static int xen_exchange_memory(xen_ulong_t extents_in,
			       unsigned int order_in,
			       xen_pfn_t *pfns_in,
			       xen_ulong_t extents_out,
			       unsigned int order_out,
			       xen_pfn_t *mfns_out,
			       unsigned int address_bits)
{
	long rc;
	int success;

	struct xen_memory_exchange exchange = {
		.in = {
			.nr_extents   = extents_in,
			.extent_order = order_in,
			.domid        = DOMID_SELF
		},
		.out = {
			.nr_extents   = extents_out,
			.extent_order = order_out,
			.address_bits = address_bits,
			.domid        = DOMID_SELF
		}
	};
	set_xen_guest_handle(exchange.in.extent_start, pfns_in);
	set_xen_guest_handle(exchange.out.extent_start, mfns_out);

	BUG_ON(extents_in << order_in != extents_out << order_out);


	rc = HYPERVISOR_memory_op(XENMEM_exchange_and_pin, &exchange);
	success = (exchange.nr_exchanged == extents_in);

	BUG_ON(!success && ((exchange.nr_exchanged != 0) || (rc == 0)));
	BUG_ON(success && (rc != 0));

	return success;
}

int xen_create_contiguous_region(phys_addr_t pstart, unsigned int order,
				 unsigned int address_bits,
				 dma_addr_t *dma_handle)
{
	xen_pfn_t in_frame, out_frame;
	int success;

	/* Get a new contiguous memory extent. */
	in_frame = out_frame = pstart >> PAGE_SHIFT;
	success = xen_exchange_memory(1, order, &in_frame,
				      1, order, &out_frame,
				      address_bits);

	if (!success)
		return -ENOMEM;

	*dma_handle = out_frame << PAGE_SHIFT;

	return success ? 0 : -ENOMEM;
}
EXPORT_SYMBOL_GPL(xen_create_contiguous_region);

void xen_destroy_contiguous_region(phys_addr_t pstart, unsigned int order)
{
	xen_pfn_t in_frame = pstart >> PAGE_SHIFT;
	struct xen_unpin unpin = {
		.in = {
			.nr_extents   = 1,
			.extent_order = order,
			.domid        = DOMID_SELF
		},
	};
	set_xen_guest_handle(unpin.in.extent_start, &in_frame);

	WARN_ON(HYPERVISOR_memory_op(XENMEM_unpin, &unpin));
}
EXPORT_SYMBOL_GPL(xen_destroy_contiguous_region);

static struct dma_map_ops xen_swiotlb_dma_ops = {
	.mapping_error = xen_swiotlb_dma_mapping_error,
	.alloc = xen_swiotlb_alloc_coherent,
	.free = xen_swiotlb_free_coherent,
	.sync_single_for_cpu = xen_swiotlb_sync_single_for_cpu,
	.sync_single_for_device = xen_swiotlb_sync_single_for_device,
	.sync_sg_for_cpu = xen_swiotlb_sync_sg_for_cpu,
	.sync_sg_for_device = xen_swiotlb_sync_sg_for_device,
	.map_sg = xen_swiotlb_map_sg_attrs,
	.unmap_sg = xen_swiotlb_unmap_sg_attrs,
	.map_page = xen_swiotlb_map_page,
	.unmap_page = xen_swiotlb_unmap_page,
	.dma_supported = xen_swiotlb_dma_supported,
	.set_dma_mask = xen_swiotlb_set_dma_mask,
};

int __init xen_mm_init(void)
{
	xen_swiotlb_init(1, true);
	dma_ops = &xen_swiotlb_dma_ops;
	return 0;
}

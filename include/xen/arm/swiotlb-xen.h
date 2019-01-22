#ifndef _ASM_ARM_XEN_SWIOTLB_XEN_H
#define _ASM_ARM_XEN_SWIOTLB_XEN_H

#ifdef CONFIG_SWIOTLB_XEN
extern int xen_swiotlb;
#else
#define xen_swiotlb (0)
#endif

#endif

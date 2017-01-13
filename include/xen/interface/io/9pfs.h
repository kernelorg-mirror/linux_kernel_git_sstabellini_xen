/*
 * 9pfs.h -- Xen 9PFS transport
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Copyright (C) 2016 Stefano Stabellini <stefano@aporeto.com>
 */

#ifndef __XEN_PUBLIC_IO_9PFS_H__
#define __XEN_PUBLIC_IO_9PFS_H__

#include "xen/interface/io/ring.h"

#define XEN_9PFS_RING_ORDER (6)
#define XEN_9PFS_RING_SIZE ((1 << ((XEN_9PFS_RING_ORDER) + XEN_PAGE_SHIFT)) / 2)
#define MASK_XEN_9PFS_IDX(idx) ((idx) & (XEN_9PFS_RING_SIZE - 1))
#define _MASK_XEN_9PFS_IDX(idx) ((idx) & (XEN_9PFS_RING_SIZE - 1))

typedef uint32_t XEN_9PFS_RING_IDX;

struct xen_9pfs_header {
	uint32_t size;
	uint8_t id;
	uint16_t tag;
} __attribute__((packed));

static inline void xen_9pfs_read_header(char *buf,
		XEN_9PFS_RING_IDX *masked_prod, XEN_9PFS_RING_IDX *masked_cons,
		struct xen_9pfs_header *h) {
	if (*masked_cons < *masked_prod) {
		memcpy(h, buf + *masked_cons, sizeof(*h));
	} else {
		if (sizeof(*h) > XEN_9PFS_RING_SIZE - *masked_cons) {
			memcpy(h, buf + *masked_cons, XEN_9PFS_RING_SIZE - *masked_cons);
			memcpy((char *)h + XEN_9PFS_RING_SIZE - *masked_cons, buf, sizeof(*h) - (XEN_9PFS_RING_SIZE - *masked_cons));
		} else {
			memcpy(h, buf + *masked_cons, sizeof(*h));
		}
	}
	*masked_cons = _MASK_XEN_9PFS_IDX(*masked_cons + sizeof(*h));
}

static inline void xen_9pfs_write_header(char *buf,
		XEN_9PFS_RING_IDX *masked_prod, XEN_9PFS_RING_IDX *masked_cons,
		struct xen_9pfs_header h) {
	if (*masked_prod < *masked_cons) {
		memcpy(buf + *masked_prod, &h, sizeof(h));
	} else {
		if (sizeof(h) > XEN_9PFS_RING_SIZE - *masked_prod) {
			memcpy(buf + *masked_prod, &h, XEN_9PFS_RING_SIZE - *masked_prod);
			memcpy(buf, (char *)(&h) + (XEN_9PFS_RING_SIZE - *masked_prod), sizeof(h) - (XEN_9PFS_RING_SIZE - *masked_prod)); 
		} else {
			memcpy(buf + *masked_prod, &h, sizeof(h)); 
		}
	}
	*masked_prod = _MASK_XEN_9PFS_IDX(*masked_prod + sizeof(h));
}

struct xen_9pfs_data {
	char *in; /* half of the allocation */
	char *out; /* half of the allocation */
};

struct xen_9pfs_data_intf {
	XEN_9PFS_RING_IDX in_cons, in_prod, in_event;
	XEN_9PFS_RING_IDX out_cons, out_prod, out_event;

	uint32_t ring_order;
	grant_ref_t ref[];
};

static inline XEN_9PFS_RING_IDX xen_9pfs_ring_queued(XEN_9PFS_RING_IDX prod,
		XEN_9PFS_RING_IDX cons)
{
	XEN_9PFS_RING_IDX size;

	if (prod == cons)
		return 0;

	prod = _MASK_XEN_9PFS_IDX(prod);
	cons = _MASK_XEN_9PFS_IDX(cons);

	if (prod == cons)
		return XEN_9PFS_RING_SIZE;

	if (prod > cons)
		size = prod - cons;
	else {
		size = XEN_9PFS_RING_SIZE - cons;
		size += prod;
	}
	return size;
}

#endif

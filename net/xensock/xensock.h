#ifndef __XEN_PUBLIC_IO_XEN_XENSOCK_H__
#define __XEN_PUBLIC_IO_XEN_XENSOCK_H__

#include <linux/net.h>

#include "xen/interface/io/ring.h"

#define XENSOCK_DATARING_ORDER 6
#define XENSOCK_DATARING_PAGES (1 << XENSOCK_DATARING_ORDER)
#define XENSOCK_DATARING_SIZE (XENSOCK_DATARING_PAGES << PAGE_SHIFT)

typedef uint32_t XENSOCK_RING_IDX;

#define MASK_XENSOCK_IDX(idx, ring) ((idx) & (sizeof(ring)-1))
#define _MASK_XENSOCK_IDX(idx, ring_size) ((idx) & (ring_size-1))

struct xensock_ring_intf {
	char in[XENSOCK_DATARING_SIZE/4];
	char out[XENSOCK_DATARING_SIZE/2];
	XENSOCK_RING_IDX in_cons, in_prod;
	XENSOCK_RING_IDX out_cons, out_prod;
	int32_t in_error, out_error;
};

static inline XENSOCK_RING_IDX xensock_ring_queued(XENSOCK_RING_IDX prod,
		XENSOCK_RING_IDX cons,
		XENSOCK_RING_IDX ring_size)
{
	XENSOCK_RING_IDX size;

	if (prod == cons)
		return 0;

	prod = _MASK_XENSOCK_IDX(prod, ring_size);
	cons = _MASK_XENSOCK_IDX(cons, ring_size);

	if (prod == cons)
		return ring_size;

	if (prod > cons)
		size = prod - cons;
	else {
		size = ring_size - cons;
		size += prod;
	}
	return size;
}

#define XENSOCK_CONNECT        0
#define XENSOCK_RELEASE        3
#define XENSOCK_BIND           4
#define XENSOCK_LISTEN         5
#define XENSOCK_ACCEPT         6
#define XENSOCK_POLL           7

struct xen_xensock_request {
	uint32_t id; /* private to guest, echoed in response */
	uint32_t cmd; /* command to execute */
	uint64_t sockid;
	union {
		struct xen_xensock_connect {
			uint8_t addr[28]; /* ipv6 ready */
			uint32_t len;
			uint32_t flags;
			grant_ref_t ref[XENSOCK_DATARING_PAGES];
			uint32_t evtchn;
		} connect;
		struct xen_xensock_bind {
			uint8_t addr[28]; /* ipv6 ready */
			uint32_t len;
		} bind;
		struct xen_xensock_accept {
			uint64_t sockid;
			grant_ref_t ref[XENSOCK_DATARING_PAGES];
			uint32_t evtchn;
		} accept;
	} u;
};

struct xen_xensock_response {
	uint32_t id;
	uint32_t cmd;
	uint64_t sockid;
	int32_t ret;
};

DEFINE_RING_TYPES(xen_xensock, struct xen_xensock_request,
		  struct xen_xensock_response);

#endif

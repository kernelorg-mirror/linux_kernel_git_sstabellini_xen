#ifndef __XEN_PUBLIC_IO_XEN_XENSOCK_H__
#define __XEN_PUBLIC_IO_XEN_XENSOCK_H__

#include <linux/net.h>

#include "xen/interface/io/ring.h"

#define XENSOCK_RING_SIZE(ring_order) ((1 << ((ring_order) + XEN_PAGE_SHIFT)) / 2)
#define MASK_XENSOCK_IDX(idx, ring_order) ((idx) & ((XENSOCK_RING_SIZE(ring_order))-1))
#define _MASK_XENSOCK_IDX(idx, ring_size) ((idx) & (ring_size-1))

typedef uint32_t XENSOCK_RING_IDX;

struct xensock_data {
	char *in; /* half of the allocation */
	char *out; /* half of the allocation */
};

struct xensock_data_intf {
	XENSOCK_RING_IDX in_cons, in_prod;
	XENSOCK_RING_IDX out_cons, out_prod;
	int32_t in_error, out_error;

	uint32_t ring_order;
	grant_ref_t ref[];
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

#define XENSOCK_SOCKET         0
#define XENSOCK_CONNECT        1
#define XENSOCK_RELEASE        2
#define XENSOCK_BIND           3
#define XENSOCK_LISTEN         4
#define XENSOCK_ACCEPT         5
#define XENSOCK_POLL           6

struct xen_xensock_request {
	uint32_t id; /* private to guest, echoed in response */
	uint32_t cmd; /* command to execute */
	uint64_t sockid;
	union {
		struct xen_xensock_socket {
			uint32_t domain;
			uint32_t type;
			uint32_t protocol;
		} socket;
		struct xen_xensock_connect {
			uint8_t addr[28]; /* ipv6 ready */
			uint32_t len;
			uint32_t flags;
			grant_ref_t ref;
			uint32_t evtchn;
		} connect;
		struct xen_xensock_bind {
			uint8_t addr[28]; /* ipv6 ready */
			uint32_t len;
		} bind;
		struct xen_xensock_listen {
			uint32_t backlog;
		} listen;
		struct xen_xensock_accept {
			uint64_t sockid;
			grant_ref_t ref;
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

#ifndef __XEN_PUBLIC_IO_XEN_PVCALLS_H__
#define __XEN_PUBLIC_IO_XEN_PVCALLS_H__

#include <linux/net.h>

#include "xen/interface/io/ring.h"

#define PVCALLS_RING_SIZE(ring_order) ((1 << ((ring_order) + XEN_PAGE_SHIFT)) / 2)
#define MASK_PVCALLS_IDX(idx, ring_order) ((idx) & ((PVCALLS_RING_SIZE(ring_order))-1))
#define _MASK_PVCALLS_IDX(idx, ring_size) ((idx) & (ring_size-1))

typedef uint32_t PVCALLS_RING_IDX;

struct pvcalls_data {
	char *in; /* half of the allocation */
	char *out; /* half of the allocation */
};

struct pvcalls_data_intf {
	PVCALLS_RING_IDX in_cons, in_prod;
	int32_t in_error;

	uint8_t pad[52];

	PVCALLS_RING_IDX out_cons, out_prod;
	int32_t out_error;

	uint32_t ring_order;
	grant_ref_t ref[];
};

static inline PVCALLS_RING_IDX pvcalls_ring_unconsumed(PVCALLS_RING_IDX prod,
		PVCALLS_RING_IDX cons,
		PVCALLS_RING_IDX ring_size)
{
	PVCALLS_RING_IDX size;

	if (prod == cons)
		return 0;

	prod = _MASK_PVCALLS_IDX(prod, ring_size);
	cons = _MASK_PVCALLS_IDX(cons, ring_size);

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

#define PVCALLS_SOCKET         0
#define PVCALLS_CONNECT        1
#define PVCALLS_RELEASE        2
#define PVCALLS_BIND           3
#define PVCALLS_LISTEN         4
#define PVCALLS_ACCEPT         5
#define PVCALLS_POLL           6

struct xen_pvcalls_request {
	uint32_t req_id; /* private to guest, echoed in response */
	uint32_t cmd;    /* command to execute */
	union {
		struct xen_pvcalls_socket {
			uint64_t id;
			uint32_t domain;
			uint32_t type;
			uint32_t protocol;
		} socket;
		struct xen_pvcalls_connect {
			uint64_t id;
			uint8_t addr[28]; /* ipv6 ready */
			uint32_t len;
			uint32_t flags;
			grant_ref_t ref;
			uint32_t evtchn;
		} connect;
		struct xen_pvcalls_release {
			uint64_t id;
		} release;
		struct xen_pvcalls_bind {
			uint64_t id;
			uint8_t addr[28]; /* ipv6 ready */
			uint32_t len;
		} bind;
		struct xen_pvcalls_listen {
			uint64_t id;
			uint32_t backlog;
		} listen;
		struct xen_pvcalls_accept {
			uint64_t id;
			uint64_t id_new;
			grant_ref_t ref;
			uint32_t evtchn;
		} accept;
		struct xen_pvcalls_poll {
			uint64_t id;
		} poll;
		/* dummy member to force sizeof(struct xen_pvcalls_request) to match across archs */
		struct xen_pvcalls_dummy {
			uint8_t dummy[56];
		} dummy;
	} u;
};

struct xen_pvcalls_response {
	uint32_t req_id;
	uint32_t cmd;
	int32_t ret;
	uint32_t pad;
	union {
		struct _xen_pvcalls_socket {
			uint64_t id;
		} socket;
		struct _xen_pvcalls_connect {
			uint64_t id;
		} connect;
		struct _xen_pvcalls_release {
			uint64_t id;
		} release;
		struct _xen_pvcalls_bind {
			uint64_t id;
		} bind;
		struct _xen_pvcalls_listen {
			uint64_t id;
		} listen;
		struct _xen_pvcalls_accept {
			uint64_t id;
		} accept;
		struct _xen_pvcalls_poll {
			uint64_t id;
		} poll;
		struct _xen_pvcalls_dummy {
			uint8_t dummy[8];
		} dummy;
	} u;
};

DEFINE_RING_TYPES(xen_pvcalls, struct xen_pvcalls_request,
		  struct xen_pvcalls_response);

#endif

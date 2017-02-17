/******************************************************************************
 * ring.h
 *
 * Shared producer-consumer ring macros.
 *
 * Tim Deegan and Andrew Warfield November 2004.
 */

#ifndef __XEN_PUBLIC_IO_RING_H__
#define __XEN_PUBLIC_IO_RING_H__

typedef unsigned int RING_IDX;

/* Round a 32-bit unsigned constant down to the nearest power of two. */
#define __RD2(_x)  (((_x) & 0x00000002) ? 0x2		       : ((_x) & 0x1))
#define __RD4(_x)  (((_x) & 0x0000000c) ? __RD2((_x)>>2)<<2    : __RD2(_x))
#define __RD8(_x)  (((_x) & 0x000000f0) ? __RD4((_x)>>4)<<4    : __RD4(_x))
#define __RD16(_x) (((_x) & 0x0000ff00) ? __RD8((_x)>>8)<<8    : __RD8(_x))
#define __RD32(_x) (((_x) & 0xffff0000) ? __RD16((_x)>>16)<<16 : __RD16(_x))

/*
 * Calculate size of a shared ring, given the total available space for the
 * ring and indexes (_sz), and the name tag of the request/response structure.
 * A ring contains as many entries as will fit, rounded down to the nearest
 * power of two (so we can mask with (size-1) to loop around).
 */
#define __CONST_RING_SIZE(_s, _sz)				\
	(__RD32(((_sz) - offsetof(struct _s##_sring, ring)) /	\
		sizeof(((struct _s##_sring *)0)->ring[0])))

/*
 * The same for passing in an actual pointer instead of a name tag.
 */
#define __RING_SIZE(_s, _sz)						\
	(__RD32(((_sz) - (long)&(_s)->ring + (long)(_s)) / sizeof((_s)->ring[0])))

/*
 * Macros to make the correct C datatypes for a new kind of ring.
 *
 * To make a new ring datatype, you need to have two message structures,
 * let's say struct request, and struct response already defined.
 *
 * In a header where you want the ring datatype declared, you then do:
 *
 *     DEFINE_RING_TYPES(mytag, struct request, struct response);
 *
 * These expand out to give you a set of types, as you can see below.
 * The most important of these are:
 *
 *     struct mytag_sring      - The shared ring.
 *     struct mytag_front_ring - The 'front' half of the ring.
 *     struct mytag_back_ring  - The 'back' half of the ring.
 *
 * To initialize a ring in your code you need to know the location and size
 * of the shared memory area (PAGE_SIZE, for instance). To initialise
 * the front half:
 *
 *     struct mytag_front_ring front_ring;
 *     SHARED_RING_INIT((struct mytag_sring *)shared_page);
 *     FRONT_RING_INIT(&front_ring, (struct mytag_sring *)shared_page,
 *		       PAGE_SIZE);
 *
 * Initializing the back follows similarly (note that only the front
 * initializes the shared ring):
 *
 *     struct mytag_back_ring back_ring;
 *     BACK_RING_INIT(&back_ring, (struct mytag_sring *)shared_page,
 *		      PAGE_SIZE);
 */

#define DEFINE_RING_TYPES(__name, __req_t, __rsp_t)			\
									\
/* Shared ring entry */							\
union __name##_sring_entry {						\
    __req_t req;							\
    __rsp_t rsp;							\
};									\
									\
/* Shared ring page */							\
struct __name##_sring {							\
    RING_IDX req_prod, req_event;					\
    RING_IDX rsp_prod, rsp_event;					\
    uint8_t  pad[48];							\
    union __name##_sring_entry ring[1]; /* variable-length */		\
};									\
									\
/* "Front" end's private variables */					\
struct __name##_front_ring {						\
    RING_IDX req_prod_pvt;						\
    RING_IDX rsp_cons;							\
    unsigned int nr_ents;						\
    struct __name##_sring *sring;					\
};									\
									\
/* "Back" end's private variables */					\
struct __name##_back_ring {						\
    RING_IDX rsp_prod_pvt;						\
    RING_IDX req_cons;							\
    unsigned int nr_ents;						\
    struct __name##_sring *sring;					\
};

/*
 * Macros for manipulating rings.
 *
 * FRONT_RING_whatever works on the "front end" of a ring: here
 * requests are pushed on to the ring and responses taken off it.
 *
 * BACK_RING_whatever works on the "back end" of a ring: here
 * requests are taken off the ring and responses put on.
 *
 * N.B. these macros do NO INTERLOCKS OR FLOW CONTROL.
 * This is OK in 1-for-1 request-response situations where the
 * requestor (front end) never has more than RING_SIZE()-1
 * outstanding requests.
 */

/* Initialising empty rings */
#define SHARED_RING_INIT(_s) do {					\
    (_s)->req_prod  = (_s)->rsp_prod  = 0;				\
    (_s)->req_event = (_s)->rsp_event = 1;				\
    memset((_s)->pad, 0, sizeof((_s)->pad));				\
} while(0)

#define FRONT_RING_INIT(_r, _s, __size) do {				\
    (_r)->req_prod_pvt = 0;						\
    (_r)->rsp_cons = 0;							\
    (_r)->nr_ents = __RING_SIZE(_s, __size);				\
    (_r)->sring = (_s);							\
} while (0)

#define BACK_RING_INIT(_r, _s, __size) do {				\
    (_r)->rsp_prod_pvt = 0;						\
    (_r)->req_cons = 0;							\
    (_r)->nr_ents = __RING_SIZE(_s, __size);				\
    (_r)->sring = (_s);							\
} while (0)

/* Initialize to existing shared indexes -- for recovery */
#define FRONT_RING_ATTACH(_r, _s, __size) do {				\
    (_r)->sring = (_s);							\
    (_r)->req_prod_pvt = (_s)->req_prod;				\
    (_r)->rsp_cons = (_s)->rsp_prod;					\
    (_r)->nr_ents = __RING_SIZE(_s, __size);				\
} while (0)

#define BACK_RING_ATTACH(_r, _s, __size) do {				\
    (_r)->sring = (_s);							\
    (_r)->rsp_prod_pvt = (_s)->rsp_prod;				\
    (_r)->req_cons = (_s)->req_prod;					\
    (_r)->nr_ents = __RING_SIZE(_s, __size);				\
} while (0)

/* How big is this ring? */
#define RING_SIZE(_r)							\
    ((_r)->nr_ents)

/* Number of free requests (for use on front side only). */
#define RING_FREE_REQUESTS(_r)						\
    (RING_SIZE(_r) - ((_r)->req_prod_pvt - (_r)->rsp_cons))

/* Test if there is an empty slot available on the front ring.
 * (This is only meaningful from the front. )
 */
#define RING_FULL(_r)							\
    (RING_FREE_REQUESTS(_r) == 0)

/* Test if there are outstanding messages to be processed on a ring. */
#define RING_HAS_UNCONSUMED_RESPONSES(_r)				\
    ((_r)->sring->rsp_prod - (_r)->rsp_cons)

#define RING_HAS_UNCONSUMED_REQUESTS(_r)				\
    ({									\
	unsigned int req = (_r)->sring->req_prod - (_r)->req_cons;	\
	unsigned int rsp = RING_SIZE(_r) -				\
			   ((_r)->req_cons - (_r)->rsp_prod_pvt);	\
	req < rsp ? req : rsp;						\
    })

/* Direct access to individual ring elements, by index. */
#define RING_GET_REQUEST(_r, _idx)					\
    (&((_r)->sring->ring[((_idx) & (RING_SIZE(_r) - 1))].req))

/*
 * Get a local copy of a request.
 *
 * Use this in preference to RING_GET_REQUEST() so all processing is
 * done on a local copy that cannot be modified by the other end.
 *
 * Note that https://gcc.gnu.org/bugzilla/show_bug.cgi?id=58145 may cause this
 * to be ineffective where _req is a struct which consists of only bitfields.
 */
#define RING_COPY_REQUEST(_r, _idx, _req) do {				\
	/* Use volatile to force the copy into _req. */			\
	*(_req) = *(volatile typeof(_req))RING_GET_REQUEST(_r, _idx);	\
} while (0)

#define RING_GET_RESPONSE(_r, _idx)					\
    (&((_r)->sring->ring[((_idx) & (RING_SIZE(_r) - 1))].rsp))

/* Loop termination condition: Would the specified index overflow the ring? */
#define RING_REQUEST_CONS_OVERFLOW(_r, _cons)				\
    (((_cons) - (_r)->rsp_prod_pvt) >= RING_SIZE(_r))

/* Ill-behaved frontend determination: Can there be this many requests? */
#define RING_REQUEST_PROD_OVERFLOW(_r, _prod)               \
    (((_prod) - (_r)->rsp_prod_pvt) > RING_SIZE(_r))


#define RING_PUSH_REQUESTS(_r) do {					\
    virt_wmb(); /* back sees requests /before/ updated producer index */	\
    (_r)->sring->req_prod = (_r)->req_prod_pvt;				\
} while (0)

#define RING_PUSH_RESPONSES(_r) do {					\
    virt_wmb(); /* front sees responses /before/ updated producer index */	\
    (_r)->sring->rsp_prod = (_r)->rsp_prod_pvt;				\
} while (0)

/*
 * Notification hold-off (req_event and rsp_event):
 *
 * When queueing requests or responses on a shared ring, it may not always be
 * necessary to notify the remote end. For example, if requests are in flight
 * in a backend, the front may be able to queue further requests without
 * notifying the back (if the back checks for new requests when it queues
 * responses).
 *
 * When enqueuing requests or responses:
 *
 *  Use RING_PUSH_{REQUESTS,RESPONSES}_AND_CHECK_NOTIFY(). The second argument
 *  is a boolean return value. True indicates that the receiver requires an
 *  asynchronous notification.
 *
 * After dequeuing requests or responses (before sleeping the connection):
 *
 *  Use RING_FINAL_CHECK_FOR_REQUESTS() or RING_FINAL_CHECK_FOR_RESPONSES().
 *  The second argument is a boolean return value. True indicates that there
 *  are pending messages on the ring (i.e., the connection should not be put
 *  to sleep).
 *
 *  These macros will set the req_event/rsp_event field to trigger a
 *  notification on the very next message that is enqueued. If you want to
 *  create batches of work (i.e., only receive a notification after several
 *  messages have been enqueued) then you will need to create a customised
 *  version of the FINAL_CHECK macro in your own code, which sets the event
 *  field appropriately.
 */

#define RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(_r, _notify) do {		\
    RING_IDX __old = (_r)->sring->req_prod;				\
    RING_IDX __new = (_r)->req_prod_pvt;				\
    virt_wmb(); /* back sees requests /before/ updated producer index */	\
    (_r)->sring->req_prod = __new;					\
    virt_mb(); /* back sees new requests /before/ we check req_event */	\
    (_notify) = ((RING_IDX)(__new - (_r)->sring->req_event) <		\
		 (RING_IDX)(__new - __old));				\
} while (0)

#define RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(_r, _notify) do {		\
    RING_IDX __old = (_r)->sring->rsp_prod;				\
    RING_IDX __new = (_r)->rsp_prod_pvt;				\
    virt_wmb(); /* front sees responses /before/ updated producer index */	\
    (_r)->sring->rsp_prod = __new;					\
    virt_mb(); /* front sees new responses /before/ we check rsp_event */	\
    (_notify) = ((RING_IDX)(__new - (_r)->sring->rsp_event) <		\
		 (RING_IDX)(__new - __old));				\
} while (0)

#define RING_FINAL_CHECK_FOR_REQUESTS(_r, _work_to_do) do {		\
    (_work_to_do) = RING_HAS_UNCONSUMED_REQUESTS(_r);			\
    if (_work_to_do) break;						\
    (_r)->sring->req_event = (_r)->req_cons + 1;			\
    virt_mb();								\
    (_work_to_do) = RING_HAS_UNCONSUMED_REQUESTS(_r);			\
} while (0)

#define RING_FINAL_CHECK_FOR_RESPONSES(_r, _work_to_do) do {		\
    (_work_to_do) = RING_HAS_UNCONSUMED_RESPONSES(_r);			\
    if (_work_to_do) break;						\
    (_r)->sring->rsp_event = (_r)->rsp_cons + 1;			\
    virt_mb();								\
    (_work_to_do) = RING_HAS_UNCONSUMED_RESPONSES(_r);			\
} while (0)


/*
 * DEFINE_XEN_FLEX_RING defines two monodirectional rings and functions
 * to check if there is data on the ring, and to read and write to them.
 *
 * XEN_FLEX_RING_SIZE
 *   Convenience macro to calculate the size of one of the two rings
 *   from the overall order.
 *
 * $NAME_mask
 *   Function to apply the size mask to an index, to reduce the index
 *   within the range [0-size].
 *
 * $NAME_read_packet
 *   Function to read a defined amount of data from the ring. The amount
 *   of data read is sizeof(__packet_t).
 *
 * $NAME_write_packet
 *   Function to write a defined amount of data to the ring. The amount
 *   of data to write is sizeof(__packet_t).
 *
 * $NAME_data_intf
 *   Indexes page, shared between frontend and backend. It also
 *   contains the array of grant refs. Different protocols can have
 *   extensions to the basic format, in such cases please define your
 *   own data_intf struct.
 *
 * $NAME_queued
 *   Function to calculate how many bytes are currently on the ring,
 *   read to be read. It can also be used to calculate how much free
 *   space is currently on the ring (ring_size - $NAME_queued()).
 */
#define XEN_FLEX_RING_SIZE(__order)                                           \
	((1 << ((__order) + XEN_PAGE_SHIFT)) / 2)

#define DEFINE_XEN_FLEX_RING(__name, __packet_t)                              \
                                                                              \
static inline RING_IDX __name##_mask(RING_IDX idx, RING_IDX ring_size)        \
{                                                                             \
	return ((idx) & (ring_size - 1));                                         \
}                                                                             \
                                                                              \
static inline RING_IDX __name##_mask_order(RING_IDX idx, RING_IDX ring_order) \
{                                                                             \
	return ((idx) & (XEN_FLEX_RING_SIZE(ring_order) - 1));                    \
}                                                                             \
                                                                              \
static inline void __name##_read_packet(char *buf,                            \
        RING_IDX *masked_prod, RING_IDX *masked_cons,                         \
        RING_IDX ring_size, __packet_t *h) {                                  \
    if (*masked_cons < *masked_prod) {                                        \
        memcpy(h, buf + *masked_cons, sizeof(*h));                            \
    } else {                                                                  \
        if (sizeof(*h) > ring_size - *masked_cons) {                          \
            memcpy(h, buf + *masked_cons, ring_size - *masked_cons);          \
            memcpy((char *)h + ring_size - *masked_cons, buf,                 \
                    sizeof(*h) - (ring_size - *masked_cons));                 \
        } else {                                                              \
            memcpy(h, buf + *masked_cons, sizeof(*h));                        \
        }                                                                     \
    }                                                                         \
    *masked_cons = __name##_mask(*masked_cons + sizeof(*h), ring_size);       \
}                                                                             \
                                                                              \
static inline void __name##_write_packet(char *buf,                           \
        RING_IDX *masked_prod, RING_IDX *masked_cons,                         \
        RING_IDX ring_size, __packet_t h) {                                   \
    if (*masked_prod < *masked_cons) {                                        \
        memcpy(buf + *masked_prod, &h, sizeof(h));                            \
    } else {                                                                  \
        if (sizeof(h) > ring_size - *masked_prod) {                           \
            memcpy(buf + *masked_prod, &h, ring_size - *masked_prod);         \
            memcpy(buf, (char *)(&h) + (ring_size - *masked_prod),            \
                    sizeof(h) - (ring_size - *masked_prod));                  \
        } else {                                                              \
            memcpy(buf + *masked_prod, &h, sizeof(h));                        \
        }                                                                     \
    }                                                                         \
    *masked_prod = __name##_mask(*masked_prod + sizeof(h), ring_size);        \
}                                                                             \
                                                                              \
struct __name##_data {                                                        \
    char *in; /* half of the allocation */                                    \
    char *out; /* half of the allocation */                                   \
};                                                                            \
                                                                              \
struct __name##_data_intf {                                                   \
    RING_IDX in_cons, in_prod;                                                \
                                                                              \
    uint8_t pad1[56];                                                         \
                                                                              \
    RING_IDX out_cons, out_prod;                                              \
                                                                              \
    uint8_t pad2[56];                                                         \
                                                                              \
	RING_IDX ring_order;                                                      \
    grant_ref_t ref[];                                                        \
};                                                                            \
                                                                              \
static inline RING_IDX __name##_queued(RING_IDX prod,                         \
        RING_IDX cons, RING_IDX ring_size)                                    \
{                                                                             \
    RING_IDX size;                                                            \
                                                                              \
    if (prod == cons)                                                         \
        return 0;                                                             \
                                                                              \
    prod = __name##_mask(prod, ring_size);                                    \
    cons = __name##_mask(cons, ring_size);                                    \
                                                                              \
    if (prod == cons)                                                         \
        return ring_size;                                                     \
                                                                              \
    if (prod > cons)                                                          \
        size = prod - cons;                                                   \
    else {                                                                    \
        size = ring_size - cons;                                              \
        size += prod;                                                         \
    }                                                                         \
    return size;                                                              \
};

#endif /* __XEN_PUBLIC_IO_RING_H__ */

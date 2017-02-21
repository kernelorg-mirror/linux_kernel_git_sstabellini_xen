#ifndef _XEN_PVCALLS_H
#define _XEN_PVCALLS_H

#include <linux/net.h>

#ifdef CONFIG_XEN_PVCALLS
extern bool pvcalls;
#else
#define pvcalls (0)
#endif
extern const struct proto_ops pvcalls_stream_ops;

#endif

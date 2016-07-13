#ifndef __LINUX_NET_XENSOCK_H
#define __LINUX_NET_XENSOCK_H

#include <linux/net.h>

#ifdef CONFIG_XENSOCK
extern bool xensock;
#else
#define xensock (0)
#endif
extern const struct proto_ops xensock_stream_ops;

#endif

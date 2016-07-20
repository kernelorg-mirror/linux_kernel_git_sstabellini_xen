#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/cred.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/poll.h>
#include <linux/skbuff.h>
#include <linux/smp.h>
#include <linux/socket.h>
#include <linux/stddef.h>
#include <linux/unistd.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <net/sock.h>
#include <net/inet_common.h>

#include "xensock_internal.h"

static int
xensock_bind(struct socket *sock, struct sockaddr *addr, int addr_len)
{
	int ret;
	ret = xensock_front_socket(sock);
	if (ret < 0)
		return ret;
	return xensock_front_bind(sock, addr, addr_len);
}

static int xensock_stream_connect(struct socket *sock, struct sockaddr *addr,
				int addr_len, int flags)
{
	int ret;
	ret = xensock_front_socket(sock);
	if (ret < 0)
		return ret;
	return xensock_front_connect(sock, addr, addr_len, flags);
}

static int xensock_accept(struct socket *sock, struct socket *newsock, int flags)
{
	return xensock_front_accept(sock, newsock, flags);
}

static int xensock_getname(struct socket *sock,
			 struct sockaddr *uaddr, int *uaddr_len, int peer)
{
	DECLARE_SOCKADDR(struct sockaddr_in *, sin, uaddr);

	sin->sin_family = AF_INET;
	memset(sin->sin_zero, 0, sizeof(sin->sin_zero));
	*uaddr_len = sizeof(*sin);
	return 0;
}

static unsigned int xensock_poll(struct file *file, struct socket *sock,
			       poll_table *wait)
{
	return xensock_front_poll(file, sock, wait);
}

static int xensock_listen(struct socket *sock, int backlog)
{
	return xensock_front_listen(sock, backlog);
}

static int xensock_stream_sendmsg(struct socket *sock, struct msghdr *msg,
				size_t len)
{
	return xensock_front_sendmsg(sock, msg, len);
}

static int
xensock_stream_recvmsg(struct socket *sock, struct msghdr *msg, size_t len,
		     int flags)
{
	return xensock_front_recvmsg(sock, msg, len, flags);
}

static int xensock_release(struct socket *s)
{
	return xensock_front_release(s);
}

static int xensock_shutdown(struct socket *s, int h)
{
	return -ENOTSUPP;
}

const struct proto_ops xensock_stream_ops = {
	.family = PF_INET,
	.owner = THIS_MODULE,
	.release = xensock_release,
	.bind = xensock_bind,
	.connect = xensock_stream_connect,
	.socketpair = sock_no_socketpair,
	.accept = xensock_accept,
	.getname = xensock_getname,
	.poll = xensock_poll,
	.ioctl = sock_no_ioctl,
	.listen = xensock_listen,
	.shutdown = xensock_shutdown,
	.setsockopt = sock_no_setsockopt,
	.getsockopt = sock_no_getsockopt,
	.sendmsg = xensock_stream_sendmsg,
	.recvmsg = xensock_stream_recvmsg,
	.mmap = sock_no_mmap,
	.sendpage = sock_no_sendpage,
};

bool xensock = false;
static __init int xen_parse_xensock(char *arg)
{
       xensock = true;
       return 0;
}
early_param("xensock", xen_parse_xensock);

/*
 * (c) 2016 Stefano Stabellini <sstabellini@kernel.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <linux/module.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/poll.h>

#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/xen.h>
#include <xen/xenbus.h>

#include <net/sock.h>

#include "xensock.h"

#define NR_REQ_PER_CONN_RING 3 /* XXX: calculate properly */
#define XENSOCK_FRON_MAX_SPIN 5000
#define XENSOCK_INVALID_ID (UINT_MAX)
#define RING_ORDER 6

struct sockpass_mapping {
	uint8_t active;
	struct list_head list;
	struct socket *sock;
	/* Socket status */
#define XENSOCK_STATUS_UNINITALIZED 0
#define XENSOCK_STATUS_BIND         1
#define XENSOCK_STATUS_LISTEN       2
	uint8_t status;
	/* 
	 * Internal state-machine flags.
	 * Only one accept operation can be inflight for a given socket.
	 * Only one poll operation can be inflight for a given socket.
	 */
#define XENSOCK_FLAG_ACCEPT_INFLIGHT 0
#define XENSOCK_FLAG_POLL_INFLIGHT 1
#define XENSOCK_FLAG_POLL_RET 2
	uint8_t flags;
	wait_queue_head_t inflight_accept_req;
};

struct sock_mapping {
	uint8_t active;
	struct list_head list;
	struct socket *sock;
	int irq;
	grant_ref_t ref;
	struct xensock_data_intf *ring;
	void *bytes;
	struct xensock_data data;
	
	wait_queue_head_t inflight_conn_req;
};

#define XENSOCK_NR_REQ_PER_RING __CONST_RING_SIZE(xen_xensock, XEN_PAGE_SIZE)

struct xensock_front_priv {
	struct xen_xensock_front_ring ring;
	grant_ref_t ref;
	int irq;

	struct list_head socket_mappings;
	struct list_head socketpass_mappings;
	spinlock_t xensocks_lock;

	wait_queue_head_t inflight_req;
	struct xen_xensock_response rsp[XENSOCK_NR_REQ_PER_RING];
};
struct xenbus_device *xensock_front_dev;

static irqreturn_t xensock_front_conn_handler(int irq, void *sock_map);

static int xensock_front_read_todo(struct sock_mapping *map)
{
	struct xensock_data_intf *intf = map->ring;
	XENSOCK_RING_IDX cons, prod;
	int32_t error;

	cons = intf->in_cons;
	prod = intf->in_prod;
	error = intf->in_error;
	mb();
	return (error != 0 || xensock_ring_queued(prod, cons, XENSOCK_RING_SIZE(intf->ring_order))) != 0;
}

static int xensock_front_write_todo(struct sock_mapping *map)
{
	struct xensock_data_intf *intf = map->ring;
	XENSOCK_RING_IDX cons, prod, size = XENSOCK_RING_SIZE(intf->ring_order);
	int32_t error;

	cons = intf->out_cons;
	prod = intf->out_prod;
	error = intf->out_error;
	mb();
	if (error == -ENOTCONN)
		return 0;
	if (error != 0)
		return error;
	return size - xensock_ring_queued(prod, cons, size);
}

static int xensock_front_write_wait(struct sock_mapping *map)
{
	struct xensock_data_intf *intf = map->ring;
	XENSOCK_RING_IDX cons, prod, size = XENSOCK_RING_SIZE(intf->ring_order);
	int32_t error;

	cons = intf->out_cons;
	prod = intf->out_prod;
	error = intf->out_error;
	mb();
	if (error == -ENOTCONN)
		return 0;
	if (error != 0)
		return error;
	return xensock_ring_queued(prod, cons, size);
}

static void xensock_front_free_map(struct xensock_front_priv *priv, struct sock_mapping *map)
{
	int i;

	if (waitqueue_active(&map->inflight_conn_req))
		return;
	spin_lock(&priv->xensocks_lock);
	if (!list_empty(&map->list))
		list_del_init(&map->list);
	spin_unlock(&priv->xensocks_lock);
	unbind_from_irqhandler(map->irq, map);
	/* what if the thread waiting still need access? */
	for (i = 0; i < (1 << map->ring->ring_order); i++)
		gnttab_end_foreign_access(map->ring->ref[i], 0, 0);
	gnttab_end_foreign_access(map->ref, 0, 0);
	free_page((unsigned long)map->ring);
	kfree(map);
}

int xensock_front_socket(struct socket *sock)
{
	struct xensock_front_priv *priv;
	struct xen_xensock_request *req;
	int notify, id, ret;

	if (!xensock_front_dev)
		return -EACCES;
	priv = dev_get_drvdata(&xensock_front_dev->dev);

	spin_lock(&priv->xensocks_lock);
	id = priv->ring.req_prod_pvt & (RING_SIZE(&priv->ring) - 1);
	BUG_ON(id >= XENSOCK_NR_REQ_PER_RING);
	if (RING_FULL(&priv->ring) || priv->rsp[id].id != XENSOCK_INVALID_ID) {
		spin_unlock(&priv->xensocks_lock);
		return -EAGAIN;
	}
	req = RING_GET_REQUEST(&priv->ring, id);
	req->id = id;
	req->cmd = XENSOCK_SOCKET;
	req->sockid = (uint64_t) sock;
	req->u.socket.domain = AF_INET;
	req->u.socket.type = SOCK_STREAM;
	req->u.socket.protocol = 0;

	priv->ring.req_prod_pvt++;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&priv->ring, notify);
	spin_unlock(&priv->xensocks_lock);
	if (notify)
	   notify_remote_via_irq(priv->irq);

	if (wait_event_interruptible(priv->inflight_req, priv->rsp[id].id == id) != 0)
		return -EINTR;

	ret = priv->rsp[id].ret;
	priv->rsp[id].id = XENSOCK_INVALID_ID;

	return ret;
}
int xensock_front_connect(struct socket *sock, struct sockaddr *addr,
				int addr_len, int flags)
{
	struct xensock_front_priv *priv;
	struct sock_mapping *map = NULL;
	struct xen_xensock_request *req;
	int notify, id, ret, evtchn = -1, irq = -1, i;

	if (!xensock_front_dev)
		return -ENETUNREACH;
	priv = dev_get_drvdata(&xensock_front_dev->dev);

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (map == NULL) {
		ret = -ENOMEM;
		goto out_error;
	}

	init_waitqueue_head(&map->inflight_conn_req);

	map->ring = (struct xensock_data_intf *) __get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (map->ring == NULL) {
		ret = -ENOMEM;
		goto out_error;
	}
	memset(map->ring, 0, XEN_PAGE_SIZE);
	map->ring->ring_order = RING_ORDER;
	map->bytes = (void*)__get_free_pages(GFP_KERNEL | __GFP_ZERO, map->ring->ring_order);
	if (map->bytes == NULL) {
		ret = -ENOMEM;
		goto out_error;
	}
	map->data.in = map->bytes;
	map->data.out = map->bytes + XENSOCK_RING_SIZE(map->ring->ring_order);

	ret = xenbus_alloc_evtchn(xensock_front_dev, &evtchn);
	if (ret)
		goto out_error;
	irq = bind_evtchn_to_irqhandler(evtchn, xensock_front_conn_handler,
					0, "xensock-frontend", map);
	if (irq < 0)
		goto out_error;

	spin_lock(&priv->xensocks_lock);
	id = priv->ring.req_prod_pvt & (RING_SIZE(&priv->ring) - 1);
	BUG_ON(id >= XENSOCK_NR_REQ_PER_RING);
	if (RING_FULL(&priv->ring) || priv->rsp[id].id != XENSOCK_INVALID_ID) {
		spin_unlock(&priv->xensocks_lock);
		return -EAGAIN;
	}
	req = RING_GET_REQUEST(&priv->ring, id);
	req->id = id;
	map->sock = sock;
	req->sockid = (uint64_t)sock;
	req->cmd = XENSOCK_CONNECT;
	memcpy(req->u.connect.addr, addr, sizeof(*addr));
	((struct sockaddr *)&req->u.connect.addr)->sa_family = AF_INET; /* force to AF_INET */
	req->u.connect.len = addr_len;
	req->u.connect.flags = flags;

	for (i = 0; i < (1 << map->ring->ring_order); i++)
		map->ring->ref[i] = gnttab_grant_foreign_access(xensock_front_dev->otherend_id, pfn_to_gfn(virt_to_pfn((void*)map->bytes) + i), 0);

	req->u.connect.ref = gnttab_grant_foreign_access(xensock_front_dev->otherend_id, pfn_to_gfn(virt_to_pfn((void*)map->ring)), 0);
	map->ref = req->u.connect.ref;
	req->u.connect.evtchn = evtchn;
	map->irq = irq;

	list_add_tail(&map->list, &priv->socket_mappings);
	sock->sk->sk_send_head = (void*)map;
	map->active = 1;

	priv->ring.req_prod_pvt++;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&priv->ring, notify);
	spin_unlock(&priv->xensocks_lock);

	if (notify)
	   notify_remote_via_irq(priv->irq);

	if (wait_event_interruptible(priv->inflight_req, priv->rsp[id].id == id) != 0)
		return -EINTR;
	
	ret = priv->rsp[id].ret;
	priv->rsp[id].id = XENSOCK_INVALID_ID;
	return ret;
	
out_error:
	if (irq >= 0)
		unbind_from_irqhandler(irq, map);
	else if (evtchn >= 0)
		xenbus_free_evtchn(xensock_front_dev, evtchn);
	kfree(map->bytes);
	kfree(map->ring);
	kfree(map);
	return ret;
}

static int __write_ring(struct xensock_data_intf *intf,
		struct xensock_data *data, struct iov_iter	*msg_iter, size_t len)
{
	XENSOCK_RING_IDX cons, prod, size, masked_prod, masked_cons;
	XENSOCK_RING_IDX array_size = XENSOCK_RING_SIZE(intf->ring_order);
	int32_t error;

	cons = intf->out_cons;
	prod = intf->out_prod;
	error = intf->out_error;
	mb();			/* update queue values before going on */

	if (error < 0)
		return error;

	size = xensock_ring_queued(prod, cons, array_size);
	if (size >= array_size)
		return 0;
	if (len > array_size - size)
		len = array_size - size;

	masked_prod = _MASK_XENSOCK_IDX(prod, array_size);
	masked_cons = _MASK_XENSOCK_IDX(cons, array_size);

	if (masked_prod < masked_cons) {
		copy_from_iter(data->out + masked_prod, len, msg_iter);
	} else {
		if (len > array_size - masked_prod) {
			copy_from_iter(data->out + masked_prod, array_size - masked_prod, msg_iter);
			copy_from_iter(data->out, len - (array_size - masked_prod), msg_iter);
		} else {
			copy_from_iter(data->out + masked_prod, len, msg_iter);
		}
	}

	wmb();			/* write ring before updating pointer */
	intf->out_prod += len;

	return len;
}

int xensock_front_sendmsg(struct socket *sock, struct msghdr *msg,
				size_t len)
{
	struct xensock_front_priv *priv;
	struct sock_mapping *map;
	int sent = 0, tot_sent = 0;
	int count = 0, flags;

	if (!xensock_front_dev)
		return -ENOTCONN;
	priv = dev_get_drvdata(&xensock_front_dev->dev);

	map = (struct sock_mapping *) sock->sk->sk_send_head;
	if (!map)
		return -ENOTSOCK;

	flags = msg->msg_flags;
	if (flags & (MSG_CONFIRM|MSG_DONTROUTE|MSG_EOR|MSG_OOB) )
		return -EOPNOTSUPP;
	if ((flags & MSG_DONTWAIT) && !xensock_front_write_todo(map))
		return -EAGAIN;

again:
	count++;
	sent = __write_ring(map->ring, &map->data, &msg->msg_iter, len);
	if (sent > 0) {
		len -= sent;
		tot_sent += sent;
		notify_remote_via_irq(map->irq);
	}
	if (sent >= 0 && len > 0 && count < XENSOCK_FRON_MAX_SPIN)
		goto again;
	if (sent < 0)
		tot_sent = sent;

	return tot_sent;
}

static int __read_ring(struct xensock_data_intf *intf, 
		struct xensock_data *data, struct iov_iter	*msg_iter,
		size_t len, int flags)
{
	XENSOCK_RING_IDX cons, prod, size, masked_prod, masked_cons;
	XENSOCK_RING_IDX array_size = XENSOCK_RING_SIZE(intf->ring_order);
	int32_t error;

	cons = intf->in_cons;
	prod = intf->in_prod;
	error = intf->in_error;
	mb();			/* get pointers before reading ring */
	if (error < 0)
		return error;

	size = xensock_ring_queued(prod, cons, array_size);
	masked_prod = _MASK_XENSOCK_IDX(prod, array_size);
	masked_cons = _MASK_XENSOCK_IDX(cons, array_size);
	
	if (size == 0)
		return 0;

	if (len > size)
		len = size;

	if (masked_prod > masked_cons) {
		copy_to_iter(data->in + masked_cons, len, msg_iter);
	} else {
		if (len > (array_size - masked_cons)) {
			copy_to_iter(data->in + masked_cons, array_size - masked_cons, msg_iter);
			copy_to_iter(data->in, len - (array_size - masked_cons), msg_iter);
		} else {
			copy_to_iter(data->in + masked_cons, len, msg_iter);
		}
	}

	mb();			/* read ring before consuming */
	if (!(flags & MSG_PEEK))
		intf->in_cons += len;

	return len;
}

int xensock_front_recvmsg(struct socket *sock, struct msghdr *msg, size_t len,
		     int flags)
{
	struct xensock_front_priv *priv;
	int ret = -EAGAIN;
	struct sock_mapping *map;
	int count = 0;

	if (!xensock_front_dev)
		return -ENOTCONN;
	priv = dev_get_drvdata(&xensock_front_dev->dev);

	map = (struct sock_mapping *) sock->sk->sk_send_head;
	if (!map)
		return -ENOTSOCK;

	if (flags & (MSG_CMSG_CLOEXEC|MSG_ERRQUEUE|MSG_OOB|MSG_TRUNC))
		return -EOPNOTSUPP;

	if (len > XENSOCK_RING_SIZE(map->ring->ring_order))
		len = XENSOCK_RING_SIZE(map->ring->ring_order);

	while (!(flags & MSG_DONTWAIT) && !xensock_front_read_todo(map)) {
		if (count < XENSOCK_FRON_MAX_SPIN)
			count++;
		else
			wait_event_interruptible(map->inflight_conn_req, xensock_front_read_todo(map));
	}
	ret = __read_ring(map->ring, &map->data, &msg->msg_iter, len, flags);

	if (ret > 0)
		notify_remote_via_irq(map->irq);
	if (ret == 0)
		ret = -EAGAIN;
	if (ret == -ENOTCONN)
		ret = 0;

	return ret;
}

static irqreturn_t xensock_front_conn_handler(int irq, void *sock_map)
{
	struct sock_mapping *map = sock_map;

	if (map == NULL)
		return IRQ_HANDLED;

	wake_up_interruptible(&map->inflight_conn_req);

	return IRQ_HANDLED;
}

static irqreturn_t xensock_front_event_handler(int irq, void *dev_id)
{
	struct xenbus_device *dev = dev_id;
	struct xensock_front_priv *priv;
	struct xen_xensock_response *rsp;
	int id = 0, more = 0;

	if (dev == NULL)
		return IRQ_HANDLED;

	priv = dev_get_drvdata(&dev->dev);
	if (priv == NULL)
		return IRQ_HANDLED;

again:
	while(RING_HAS_UNCONSUMED_RESPONSES(&priv->ring)) {
		rsp = RING_GET_RESPONSE(&priv->ring, priv->ring.rsp_cons); 

		id = rsp->id;
		if (rsp->cmd == XENSOCK_POLL) {
			struct socket *sock = (struct socket *) rsp->sockid;
			struct sockpass_mapping *map = (struct sockpass_mapping *) sock->sk->sk_send_head;

			set_bit(XENSOCK_FLAG_POLL_RET, (void*)&map->flags);
			clear_bit(XENSOCK_FLAG_POLL_INFLIGHT, (void*)&map->flags);
		} else
			priv->rsp[id] = *rsp;
		priv->ring.rsp_cons++;
		wake_up(&priv->inflight_req);
	}

	RING_FINAL_CHECK_FOR_RESPONSES(&priv->ring, more);
	if (more)
		goto again;
	return IRQ_HANDLED;
}

int xensock_front_release(struct socket *sock)
{
	struct xensock_front_priv *priv;
	struct sock_mapping *map;
	struct sockpass_mapping *mappass;
	int id, notify;
	struct xen_xensock_request *req;

	if (!xensock_front_dev)
		return -EIO;
	priv = dev_get_drvdata(&xensock_front_dev->dev);
	if (!priv)
		return -EIO;

	if (sock->sk == NULL)
		return 0;

	map = (struct sock_mapping *) sock->sk->sk_send_head;
	if (map == NULL)
		return 0;
	sock->sk->sk_send_head = NULL;

	if (map->active) {
		while (wait_event_interruptible(map->inflight_conn_req, xensock_front_write_wait(map) <= 0) != 0);
	}

	spin_lock(&priv->xensocks_lock);
	id = priv->ring.req_prod_pvt & (RING_SIZE(&priv->ring) - 1);
	BUG_ON(id >= XENSOCK_NR_REQ_PER_RING);
	if (RING_FULL(&priv->ring) || priv->rsp[id].id != XENSOCK_INVALID_ID) {
		spin_unlock(&priv->xensocks_lock);
		return -EAGAIN;
	}
	req = RING_GET_REQUEST(&priv->ring, id);
	req->id = id;
	req->cmd = XENSOCK_RELEASE;
	req->sockid = (uint64_t)sock;

	priv->ring.req_prod_pvt++;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&priv->ring, notify);
	spin_unlock(&priv->xensocks_lock);
	if (notify)
	   notify_remote_via_irq(priv->irq);

	/* XXX: we might want to return EINTR eventually */
	while (wait_event_interruptible(priv->inflight_req, priv->rsp[id].id == id) != 0);

	if (map->active) {
		map->ring->in_error = -EBADF;
		mb();
		wake_up_interruptible(&map->inflight_conn_req);
		xensock_front_free_map(priv, map);
	} else {
		mappass = (struct sockpass_mapping *) map;
		spin_lock(&priv->xensocks_lock);
		list_del_init(&mappass->list);
		kfree(mappass);
		spin_unlock(&priv->xensocks_lock);
	}
	priv->rsp[id].id = XENSOCK_INVALID_ID;

	return 0;
}

int xensock_front_bind(struct socket *sock, struct sockaddr *addr, int addr_len)
{
	struct xensock_front_priv *priv;
	struct sockpass_mapping *map = NULL;
	struct xen_xensock_request *req;
	int notify, id, ret;

	if (!xensock_front_dev)
		return -ENOTCONN;
	priv = dev_get_drvdata(&xensock_front_dev->dev);

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (map == NULL) {
		ret = -ENOMEM;
		goto out_error;
	}

	spin_lock(&priv->xensocks_lock);
	id = priv->ring.req_prod_pvt & (RING_SIZE(&priv->ring) - 1);
	BUG_ON(id >= XENSOCK_NR_REQ_PER_RING);
	if (RING_FULL(&priv->ring) || priv->rsp[id].id != XENSOCK_INVALID_ID) {
		kfree(map);
		spin_unlock(&priv->xensocks_lock);
		return -EAGAIN;
	}
	req = RING_GET_REQUEST(&priv->ring, id);
	req->id = id;
	map->sock = sock;
	req->cmd = XENSOCK_BIND;
	req->sockid = (uint64_t) sock;
	memcpy(req->u.bind.addr, addr, sizeof(*addr));
	((struct sockaddr *)&req->u.bind.addr)->sa_family = AF_INET; /* force to AF_INET */
	req->u.bind.len = addr_len;

	init_waitqueue_head(&map->inflight_accept_req);

	list_add_tail(&map->list, &priv->socketpass_mappings);
	sock->sk->sk_send_head = (void*)map;
	map->active = 0;

	priv->ring.req_prod_pvt++;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&priv->ring, notify);
	spin_unlock(&priv->xensocks_lock);
	if (notify)
	   notify_remote_via_irq(priv->irq);

	if (wait_event_interruptible(priv->inflight_req, priv->rsp[id].id == id) != 0)
		return -EINTR;

	map->status = XENSOCK_STATUS_BIND;
	ret = priv->rsp[id].ret;
	priv->rsp[id].id = XENSOCK_INVALID_ID;

out_error:
	return ret;
}

static unsigned int xensock_front_poll_passive(struct file *file, struct xensock_front_priv *priv,
		struct sockpass_mapping *map, poll_table *wait)
{
	int notify, id;
	struct xen_xensock_request *req;

	if (test_bit(XENSOCK_FLAG_ACCEPT_INFLIGHT, (void*)&map->flags)) {
		poll_wait(file, &map->inflight_accept_req, wait);
		return 0;
	}

	if (test_and_clear_bit(XENSOCK_FLAG_POLL_RET, (void*)&map->flags))
		return POLLIN;

	if (test_and_set_bit(XENSOCK_FLAG_POLL_INFLIGHT, (void*)&map->flags)) {
		poll_wait(file, &priv->inflight_req, wait);
		return 0;
	}

	spin_lock(&priv->xensocks_lock);
	id = priv->ring.req_prod_pvt & (RING_SIZE(&priv->ring) - 1);
	BUG_ON(id >= XENSOCK_NR_REQ_PER_RING);
	if (RING_FULL(&priv->ring) || priv->rsp[id].id != XENSOCK_INVALID_ID) {
		spin_unlock(&priv->xensocks_lock);
		return -EAGAIN;
	}
	req = RING_GET_REQUEST(&priv->ring, id);
	req->id = id;
	req->cmd = XENSOCK_POLL;
	req->sockid = (uint64_t) map->sock;

	priv->ring.req_prod_pvt++;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&priv->ring, notify);
	spin_unlock(&priv->xensocks_lock);
	if (notify)
	   notify_remote_via_irq(priv->irq);

	poll_wait(file, &priv->inflight_req, wait);
	return 0;
}

static unsigned int xensock_front_poll_active(struct file *file, struct xensock_front_priv *priv,
		struct sock_mapping *map, poll_table *wait)
{
	unsigned int mask = 0;
	int32_t in_error, out_error;
	struct xensock_data_intf *intf = map->ring;

	out_error = intf->out_error;
	in_error = intf->in_error;
	mb();

	poll_wait(file, &map->inflight_conn_req, wait);
	if (xensock_front_write_todo(map))
		mask |= POLLOUT | POLLWRNORM;
	if (xensock_front_read_todo(map))
		mask |= POLLIN | POLLRDNORM;
	if (in_error != 0 || out_error != 0)
		mask |= POLLERR;

	return mask;
}

unsigned int xensock_front_poll(struct file *file, struct socket *sock,
			       poll_table *wait)
{
	struct xensock_front_priv *priv;
	struct sockpass_mapping *mappass;
	struct sock_mapping *map;

	if (!xensock_front_dev)
		return POLLNVAL;
	priv = dev_get_drvdata(&xensock_front_dev->dev);

	map = (struct sock_mapping *) sock->sk->sk_send_head;
	if (!map)
		return POLLNVAL;
	if (map->active)
		return xensock_front_poll_active(file, priv, map, wait);
	mappass = (struct sockpass_mapping *) sock->sk->sk_send_head;
	return xensock_front_poll_passive(file, priv, mappass, wait);
}

int xensock_front_listen(struct socket *sock, int backlog)
{
	struct xensock_front_priv *priv;
	struct sockpass_mapping *map;
	struct xen_xensock_request *req;
	int notify, id, ret;

	if (!xensock_front_dev)
		return -ENOTCONN;
	priv = dev_get_drvdata(&xensock_front_dev->dev);

	map = (struct sockpass_mapping *) sock->sk->sk_send_head;
	if (!map)
		return -ENOTSOCK;

	if (map->status != XENSOCK_STATUS_BIND)
		return -EOPNOTSUPP;

	spin_lock(&priv->xensocks_lock);
	id = priv->ring.req_prod_pvt & (RING_SIZE(&priv->ring) - 1);
	BUG_ON(id >= XENSOCK_NR_REQ_PER_RING);
	if (RING_FULL(&priv->ring) || priv->rsp[id].id != XENSOCK_INVALID_ID) {
		spin_unlock(&priv->xensocks_lock);
		return -EAGAIN;
	}
	req = RING_GET_REQUEST(&priv->ring, id);
	req->id = id;
	req->cmd = XENSOCK_LISTEN;
	req->sockid = (uint64_t) sock;
	req->u.listen.backlog = backlog;

	priv->ring.req_prod_pvt++;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&priv->ring, notify);
	spin_unlock(&priv->xensocks_lock);
	if (notify)
	   notify_remote_via_irq(priv->irq);

	if (wait_event_interruptible(priv->inflight_req, priv->rsp[id].id == id) != 0)
		return -EINTR;

	map->status = XENSOCK_STATUS_LISTEN;
	ret = priv->rsp[id].ret;
	priv->rsp[id].id = XENSOCK_INVALID_ID;
	return ret;
}

int xensock_front_accept(struct socket *sock, struct socket *newsock, int flags)
{
	struct xensock_front_priv *priv;
	struct sockpass_mapping *mappass;
	struct sock_mapping *map = NULL;
	struct xen_xensock_request *req;
	int notify, id, i, ret, irq = -1, evtchn = -1;

	if (!xensock_front_dev)
		return -ENOTCONN;
	priv = dev_get_drvdata(&xensock_front_dev->dev);

	mappass = (struct sockpass_mapping *) sock->sk->sk_send_head;
	if (!mappass)
		return -ENOTSOCK;

	if (mappass->status != XENSOCK_STATUS_LISTEN)
		return -EINVAL;

	/* backend only supports 1 inflight accept request, will return
	 * errors for the others */
	if (test_and_set_bit(XENSOCK_FLAG_ACCEPT_INFLIGHT, (void*)&mappass->flags)) {
		if (wait_event_interruptible(mappass->inflight_accept_req, !test_and_set_bit(XENSOCK_FLAG_ACCEPT_INFLIGHT, (void*)&mappass->flags)) != 0)
			return -EINTR;
	}

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (map == NULL) {
		ret = -ENOMEM;
		goto out_error;
	}

	for (i = 0; i < NR_REQ_PER_CONN_RING; i++)
		init_waitqueue_head(&map->inflight_conn_req);

	map->ring = (struct xensock_data_intf *) __get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (map->ring == NULL) {
		ret = -ENOMEM;
		goto out_error;
	}
	memset(map->ring, 0, XEN_PAGE_SIZE);
	map->ring->ring_order = RING_ORDER;
	map->bytes = (void*)__get_free_pages(GFP_KERNEL | __GFP_ZERO, map->ring->ring_order);
	if (map->bytes == NULL) {
		ret = -ENOMEM;
		goto out_error;
	}
	map->data.in = map->bytes;
	map->data.out = map->bytes + XENSOCK_RING_SIZE(map->ring->ring_order);
	ret = xenbus_alloc_evtchn(xensock_front_dev, &evtchn);
	if (ret)
		goto out_error;
	irq = bind_evtchn_to_irqhandler(evtchn, xensock_front_conn_handler,
					0, "xensock-frontend", map);
	if (irq < 0)
		goto out_error;

	newsock->sk = kzalloc(sizeof(*newsock->sk), GFP_KERNEL);
	if (newsock->sk == NULL)
		goto out_error;
	newsock->sk->sk_send_head = (void*)map;

	map->sock = newsock;

	spin_lock(&priv->xensocks_lock);
	id = priv->ring.req_prod_pvt & (RING_SIZE(&priv->ring) - 1);
	BUG_ON(id >= XENSOCK_NR_REQ_PER_RING);
	if (RING_FULL(&priv->ring) || priv->rsp[id].id != XENSOCK_INVALID_ID) {
		spin_unlock(&priv->xensocks_lock);
		return -EAGAIN;
	}
	req = RING_GET_REQUEST(&priv->ring, id);
	req->id = id;
	req->cmd = XENSOCK_ACCEPT;
	req->sockid = (uint64_t) sock;

	for (i = 0; i < (1 << map->ring->ring_order); i++)
		map->ring->ref[i] = gnttab_grant_foreign_access(xensock_front_dev->otherend_id, pfn_to_gfn(virt_to_pfn((void*)map->bytes) + i), 0);

	req->u.accept.ref = gnttab_grant_foreign_access(xensock_front_dev->otherend_id, pfn_to_gfn(virt_to_pfn((void*)map->ring)), 0);
	map->ref = req->u.accept.ref;
	req->u.accept.sockid = (uint64_t) newsock;

	req->u.accept.evtchn = evtchn;
	map->irq = irq;

	list_add_tail(&map->list, &priv->socket_mappings);
	map->active = 1;

	priv->ring.req_prod_pvt++;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&priv->ring, notify);
	spin_unlock(&priv->xensocks_lock);
	if (notify)
	   notify_remote_via_irq(priv->irq);

	if (wait_event_interruptible(priv->inflight_req, priv->rsp[id].id == id) != 0)
		return -EINTR;

	clear_bit(XENSOCK_FLAG_ACCEPT_INFLIGHT, (void*)&mappass->flags);
	wake_up(&mappass->inflight_accept_req);

	ret = priv->rsp[id].ret;
	priv->rsp[id].id = XENSOCK_INVALID_ID;
	return ret;

out_error:
	if (irq >= 0)
		unbind_from_irqhandler(irq, map);
	else if (evtchn >= 0)
		xenbus_free_evtchn(xensock_front_dev, evtchn);
	kfree(sock->sk);
	if (map) {
		kfree(map->bytes);
		kfree(map->ring);
	}
	kfree(map);
	return ret;
}

static const struct xenbus_device_id xensock_front_ids[] = {
	{ "xensock" },
	{ "" }
};

static int xensock_front_remove(struct xenbus_device *dev)
{
	struct xensock_front_priv *priv;
	struct sock_mapping *map = NULL, *n;
	struct sock_mapping *mappass = NULL, *npass;

	priv = dev_get_drvdata(&xensock_front_dev->dev);
	
	list_for_each_entry_safe(map, n, &priv->socket_mappings, list) {
		xensock_front_free_map(priv, map);
	}
	list_for_each_entry_safe(mappass, npass, &priv->socketpass_mappings, list) {
		spin_lock(&priv->xensocks_lock);
		list_del_init(&mappass->list);
		spin_unlock(&priv->xensocks_lock);
		kfree(mappass);
	}
	if (priv->irq > 0)
		unbind_from_irqhandler(priv->irq, dev);
	if (priv->ref >= 0)
		gnttab_end_foreign_access(priv->ref, 0, 0);
	kfree(priv->ring.sring);
	kfree(priv);
	dev_set_drvdata(&dev->dev, NULL);
	xenbus_switch_state(dev, XenbusStateClosed);
	xensock_front_dev = NULL;
	return 0;
}

static int xensock_front_probe(struct xenbus_device *dev,
			  const struct xenbus_device_id *id)
{
	int ret = -EFAULT, evtchn, ref = -1, i, max_page_order;
	grant_ref_t gref_head = 0;
	struct xenbus_transaction xbt;
	struct xensock_front_priv *priv = NULL;
	struct xen_xensock_sring *sring;

	if (xensock_front_dev != NULL) {
		dev_err(&dev->dev, "only one XenSock connection supported\n");
		return -EINVAL;
	}

	ret = xenbus_scanf(XBT_NIL, dev->otherend,
			   "max-dataring-page-order", "%u", &max_page_order);
	if (ret <= 0)
		return -EINVAL;
	if (max_page_order < RING_ORDER)
		return -ENOTSUPP;
	pr_info("%s max-dataring-page-order is %u\n", __func__, max_page_order);

	priv = kzalloc(sizeof(struct xensock_front_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	
	init_waitqueue_head(&priv->inflight_req);
	for (i = 0; i < XENSOCK_NR_REQ_PER_RING; i++)
		priv->rsp[i].id = XENSOCK_INVALID_ID;

	sring = (struct xen_xensock_sring *) __get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!sring)
		goto error;
	SHARED_RING_INIT(sring);
	FRONT_RING_INIT(&priv->ring, sring, XEN_PAGE_SIZE);

	ret = xenbus_alloc_evtchn(dev, &evtchn);
	if (ret)
		goto error;
	
	priv->irq = bind_evtchn_to_irqhandler(evtchn, xensock_front_event_handler,
					0, "xensock-frontend", dev);
	if (priv->irq < 0) {
		ret = priv->irq;
		goto error;
	}

	ret = gnttab_alloc_grant_references(1, &gref_head);
	if (ret < 0)
		goto error;
	priv->ref = ref = gnttab_claim_grant_reference(&gref_head);
	if (ref < 0)
		goto error;
	gnttab_grant_foreign_access_ref(ref, dev->otherend_id, virt_to_gfn((void*)sring), 0);

 again:
	ret = xenbus_transaction_start(&xbt);
	if (ret) {
		xenbus_dev_fatal(dev, ret, "starting transaction");
		goto error;
	}
	ret = xenbus_printf(xbt, dev->nodename, "ring-ref", "%d", ref);
	if (ret)
		goto error_xenbus;
	ret = xenbus_printf(xbt, dev->nodename, "port", "%u",
			    evtchn);
	if (ret)
		goto error_xenbus;
	ret = xenbus_transaction_end(xbt, 0);
	if (ret) {
		if (ret == -EAGAIN)
			goto again;
		xenbus_dev_fatal(dev, ret, "completing transaction");
		goto error;
	}

	INIT_LIST_HEAD(&priv->socket_mappings);
	INIT_LIST_HEAD(&priv->socketpass_mappings);
	spin_lock_init(&priv->xensocks_lock);
	dev_set_drvdata(&dev->dev, priv);
	xensock_front_dev = dev;
	xenbus_switch_state(dev, XenbusStateInitialised);

	return 0;

 error_xenbus:
	xenbus_transaction_end(xbt, 1);
	xenbus_dev_fatal(dev, ret, "writing xenstore");
 error:
	xensock_front_remove(dev);
	return ret;
}

static int xensock_front_resume(struct xenbus_device *dev)
{
	dev_warn(&dev->dev, "suspsend/resume unsupported\n");
	return 0;
}

static void xensock_front_changed(struct xenbus_device *dev,
			    enum xenbus_state backend_state)
{
	switch (backend_state) {
	case XenbusStateReconfiguring:
	case XenbusStateReconfigured:
	case XenbusStateInitialising:
	case XenbusStateInitialised:
	case XenbusStateUnknown:
		break;

	case XenbusStateInitWait:
		break;

	case XenbusStateConnected:
		xenbus_switch_state(dev, XenbusStateConnected);
		break;

	case XenbusStateClosed:
		if (dev->state == XenbusStateClosed)
			break;
		/* Missed the backend's CLOSING state -- fallthrough */
	case XenbusStateClosing:
		xenbus_frontend_closed(dev);
		break;
	}
}

static struct xenbus_driver xensock_front_driver = {
	.ids = xensock_front_ids,
	.probe = xensock_front_probe,
	.remove = xensock_front_remove,
	.resume = xensock_front_resume,
	.otherend_changed = xensock_front_changed,
};

static int __init xensock_frontend_init(void)
{
	if (!xen_domain())
		return -ENODEV;

	pr_info("Initialising Xen virtual socket driver\n");

	return xenbus_register_frontend(&xensock_front_driver);
}

module_init(xensock_frontend_init);

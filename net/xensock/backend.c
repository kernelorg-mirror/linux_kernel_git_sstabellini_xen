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

#include <linux/kthread.h>
#include <linux/radix-tree.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/inet.h>
#include <net/sock.h>
#include <net/inet_common.h>
#include <net/inet_connection_sock.h>
#include <net/request_sock.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/poll.h>
#include <linux/fdtable.h>

#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/xen.h>
#include <xen/xenbus.h>

#include "xensock.h"

struct iothread_data {
	int num;
};

struct xensock_iothread {
	struct iothread_data data;
	struct task_struct *iothread;
	atomic_t io;
	wait_queue_head_t queue_io;
	struct list_head wqs;
	spinlock_t lock;
};

struct xensock_back_global {
	struct xensock_iothread *iothreads;
	int nr_iothreads;
	struct list_head privs;
	struct rw_semaphore privs_lock;
};
struct xensock_back_global *__xensock;

struct xensock_back_priv {
	struct list_head list;
	struct xenbus_device *dev;
	struct xen_xensock_sring *sring;
	struct xen_xensock_back_ring ring;
	int irq;
	struct list_head socket_mappings;
	struct radix_tree_root socketpass_mappings;
	struct rw_semaphore xensocks_lock;
	wait_queue_head_t queue_work;
	atomic_t work;
	struct task_struct *thread;
};

struct sock_mapping {
	struct list_head list;
	struct list_head queue;
	struct xensock_back_priv *priv;
	struct sockpass_mapping *sockpass;
	struct socket *sock;
	int data_kthread;
	uint64_t sockid;
	struct xensock_ring_intf *ring;
	int irq;
	atomic_t read;
	atomic_t write;
	atomic_t release;
	void (*saved_data_ready)(struct sock *sk);
};

struct sockpass_mapping {
	struct list_head list;
	struct xensock_back_priv *priv;
	struct socket *sock;
	uint64_t sockid;
	struct xen_xensock_request reqcopy;
	struct workqueue_struct *wq;
	struct work_struct register_work;
	void (*saved_data_ready)(struct sock *sk);
};

static struct sock_mapping *accept_data;

static irqreturn_t xensock_back_conn_event(int irq, void *map);
static void xensock_sk_data_ready(struct sock *sock);
static void xensock_pass_sk_data_ready(struct sock *sock);
static void __xensock_back_accept(struct work_struct *work);
static int xensock_back_release_active(struct xenbus_device *dev,
		struct xensock_back_priv *priv,
		struct sock_mapping *map);
static int backend_disconnect(struct xenbus_device *dev);
static struct xenbus_driver xensock_back_driver;

static int xensock_back_bind(struct xenbus_device *dev,
		struct xen_xensock_request *req)
{
	struct xensock_back_priv *priv;
	int ret, err;
	struct socket *sock;
	struct sockpass_mapping *map = NULL;
	struct xen_xensock_response *rsp;

	if (dev == NULL)
		return 0;
	priv = dev_get_drvdata(&dev->dev);

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (map == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	INIT_WORK(&map->register_work, __xensock_back_accept);
	map->wq = alloc_workqueue("xensock_wq", WQ_UNBOUND, 1);
	if (!map->wq) {
		ret = -ENOMEM;
		kfree(map);
		goto out;
	}

	ret = sock_create(AF_INET, SOCK_STREAM, 0, &sock);
	if (ret < 0) {
		destroy_workqueue(map->wq);
		kfree(map);
		goto out;
	}

	ret = inet_bind(sock, (struct sockaddr *)&req->u.bind.addr,
			req->u.bind.len);
	if (ret < 0) {
		destroy_workqueue(map->wq);
		kfree(map);
		goto out;
	}

	map->priv = priv;
	map->sock = sock;
	map->sockid = req->sockid;

	down_write(&priv->xensocks_lock);
	err = radix_tree_insert(&priv->socketpass_mappings, map->sockid,
			map);
	up_write(&priv->xensocks_lock);
	if (err) {
		ret = err;
		destroy_workqueue(map->wq);
		kfree(map);
		goto out;
	}

	map->saved_data_ready = sock->sk->sk_data_ready;
	sock->sk->sk_user_data = map;
	sock->sk->sk_data_ready = xensock_pass_sk_data_ready;

out:
	rsp = RING_GET_RESPONSE(&priv->ring, priv->ring.rsp_prod_pvt++);
	rsp->id = req->id;
	rsp->cmd = req->cmd;
	rsp->sockid = req->sockid;
	rsp->ret = ret;
	return 1;
}

static int xensock_back_listen(struct xenbus_device *dev,
		struct xen_xensock_request *req)
{
	struct xensock_back_priv *priv;
	int ret = -EINVAL;
	struct sockpass_mapping *map;
	struct xen_xensock_response *rsp;

	if (dev == NULL)
		return 0;
	priv = dev_get_drvdata(&dev->dev);

	map = radix_tree_lookup(&priv->socketpass_mappings, req->sockid);
	if (map == NULL)
		goto out;

	ret = inet_listen(map->sock, 10);

out:
	rsp = RING_GET_RESPONSE(&priv->ring, priv->ring.rsp_prod_pvt++);
	rsp->id = req->id;
	rsp->cmd = req->cmd;
	rsp->sockid = req->sockid;
	rsp->ret = ret;
	return 1;
}

static int xensock_back_poll(struct xenbus_device *dev,
		struct xen_xensock_request *req)
{
	struct xensock_back_priv *priv;
	struct sockpass_mapping *mappass;
	struct xen_xensock_response *rsp;
	struct inet_connection_sock *icsk;
	struct request_sock_queue *queue;

	if (dev == NULL)
		return 0;
	priv = dev_get_drvdata(&dev->dev);

	mappass = radix_tree_lookup(&priv->socketpass_mappings, req->sockid);
	if (mappass == NULL)
		return 0;

	/* ignore poll requests when one accept is inflight */
	if (mappass->reqcopy.cmd == XENSOCK_ACCEPT || accept_data != NULL)
		return 0;

	mappass->reqcopy = *req;

	icsk = inet_csk(mappass->sock->sk);
	queue = &icsk->icsk_accept_queue;
	if (queue->rskq_accept_head != NULL) {
		rsp = RING_GET_RESPONSE(&priv->ring, priv->ring.rsp_prod_pvt++);
		rsp->id = req->id;
		rsp->cmd = req->cmd;
		rsp->sockid = req->sockid;
		rsp->ret = 0;
		mappass->reqcopy.cmd = 0;

		return 1;
	}

	return 0;
}

static int xensock_back_accept(struct xenbus_device *dev,
		struct xen_xensock_request *req)
{
	struct xensock_back_priv *priv;
	struct sockpass_mapping *mappass;
	struct sock_mapping *map = NULL;
	int ret = -EINVAL;
	void *page = NULL;
	struct xen_xensock_response *rsp;

	if (dev == NULL)
		return 0;
	priv = dev_get_drvdata(&dev->dev);
 
	mappass = radix_tree_lookup(&priv->socketpass_mappings, req->sockid);
	if (mappass == NULL)
		goto out_error;

	if (mappass->reqcopy.cmd == XENSOCK_ACCEPT || accept_data != NULL) {
		ret = -EINTR; /* try again */
		goto out_error;
	}

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (map == NULL) {
		ret = -ENOMEM;
		goto out_error;
	}

	INIT_LIST_HEAD(&map->queue);
	map->sock = sock_alloc();
	if (!map->sock)
		goto out_error;

	map->data_kthread = get_random_int() % __xensock->nr_iothreads;
	printk("DEBUG %s %d data_kthread=%d\n",__func__,__LINE__,map->data_kthread);


	ret = xenbus_map_ring_valloc(priv->dev, req->u.accept.ref, XENSOCK_DATARING_PAGES, &page);
	if (ret < 0)
		goto out_error;
	ret = bind_interdomain_evtchn_to_irqhandler(dev->otherend_id, req->u.accept.evtchn,
						    xensock_back_conn_event, 0,
						    "xensock-backend", map);
	if (ret < 0)
		goto out_error;
	map->irq = ret;

	map->priv = priv;
	map->ring = page;
	map->sockpass = mappass;
	map->sock->type = mappass->sock->type;
	map->sock->ops = mappass->sock->ops;
	map->sockid = req->u.accept.sockid;

	mappass->reqcopy = *req;
	accept_data = map;

	down_write(&priv->xensocks_lock);
	list_add_tail(&map->list, &priv->socket_mappings);
	up_write(&priv->xensocks_lock);

	queue_work(mappass->wq, &mappass->register_work);
	return 0;

out_error:
	if (map && map->sock)
		sock_release(map->sock);
	if (map)
		kfree(map);
	if (page != NULL)
		xenbus_unmap_ring_vfree(dev, page);

	rsp = RING_GET_RESPONSE(&priv->ring, priv->ring.rsp_prod_pvt++);
	rsp->id = req->id;
	rsp->cmd = req->cmd;
	rsp->sockid = req->sockid;
	rsp->ret = ret;
	return 1;
}

static void __xensock_back_accept(struct work_struct *work)
{
	struct sockpass_mapping *mappass = container_of(work, struct sockpass_mapping, register_work);
	struct sock_mapping *map = accept_data;
	struct xensock_back_priv *priv;
	int ret = -EINVAL;
	struct xen_xensock_response *rsp;
	int notify;
	unsigned long flags;

	if (map == NULL)
		return;
	mappass = map->sockpass;
	priv = mappass->priv;
	if (mappass->reqcopy.cmd != XENSOCK_ACCEPT) /* nothing to do */
		return;
	ret = inet_accept(mappass->sock, map->sock, O_NONBLOCK);
	if (ret == -EAGAIN)
		return;

	rsp = RING_GET_RESPONSE(&priv->ring, priv->ring.rsp_prod_pvt++);
	rsp->id = mappass->reqcopy.id;
	rsp->sockid = mappass->reqcopy.sockid;
	rsp->cmd = mappass->reqcopy.cmd;
	rsp->ret = ret;

	mappass->reqcopy.cmd = 0;
	accept_data = NULL;

	RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&priv->ring, notify);
	if (notify)
		notify_remote_via_irq(priv->irq);

	if (ret < 0) {
		xensock_back_release_active(priv->dev, priv, map);
		return;
	}

	map->saved_data_ready = map->sock->sk->sk_data_ready;
	map->sock->sk->sk_user_data = map;
	map->sock->sk->sk_data_ready = xensock_sk_data_ready;

	spin_lock_irqsave(&__xensock->iothreads[map->data_kthread].lock, flags);
	atomic_inc(&map->read);
	if (list_empty(&map->queue))
		list_add_tail(&map->queue, &__xensock->iothreads[map->data_kthread].wqs);
	spin_unlock_irqrestore(&__xensock->iothreads[map->data_kthread].lock, flags);
	atomic_inc(&__xensock->iothreads[map->data_kthread].io);
	wake_up_interruptible(&__xensock->iothreads[map->data_kthread].queue_io);
}

static int xensock_back_connect(struct xenbus_device *dev,
		struct xen_xensock_request *req)
{
	struct xensock_back_priv *priv;
	int ret;
	struct socket *sock;
	struct sock_mapping *map = NULL;
	void *page;
	struct xen_xensock_response *rsp;

	if (dev == NULL)
		return 0;
	priv = dev_get_drvdata(&dev->dev);

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (map == NULL) {
		ret = -ENOMEM;
		goto out;
	}
	ret = sock_create(AF_INET, SOCK_STREAM, 0, &sock);
	if (ret < 0) {
		kfree(map);
		goto out;
	}
	INIT_LIST_HEAD(&map->queue);
	map->data_kthread = get_random_int() % __xensock->nr_iothreads;
	printk("DEBUG %s %d data_kthread=%d\n",__func__,__LINE__,map->data_kthread);

	map->priv = priv;
	map->sock = sock;
	map->sockid = req->sockid;
	ret = xenbus_map_ring_valloc(dev, req->u.connect.ref, XENSOCK_DATARING_PAGES, &page);
	if (ret < 0) {
		sock_release(map->sock);
		kfree(map);
		goto out;
	}
	map->ring = page;
	ret = bind_interdomain_evtchn_to_irqhandler(priv->dev->otherend_id, req->u.connect.evtchn,
						    xensock_back_conn_event, 0,
						    "xensock-backend", map);
	if (ret < 0) {
		sock_release(map->sock);
		kfree(map);
		goto out;
	}
	map->irq = ret;

	down_write(&priv->xensocks_lock);
	list_add_tail(&map->list, &priv->socket_mappings);
	up_write(&priv->xensocks_lock);

	ret = inet_stream_connect(sock, (struct sockaddr *)&req->u.connect.addr,
			req->u.connect.len,	req->u.connect.flags);
	if (ret < 0) {
		xensock_back_release_active(dev, priv, map);
	} else {
		map->saved_data_ready = sock->sk->sk_data_ready;
		sock->sk->sk_user_data = map;
		sock->sk->sk_data_ready = xensock_sk_data_ready;
	}

out:
	rsp = RING_GET_RESPONSE(&priv->ring, priv->ring.rsp_prod_pvt++);
	rsp->id = req->id;
	rsp->cmd = req->cmd;
	rsp->sockid = req->sockid;
	rsp->ret = ret;

	return 1;
}

static int xensock_back_release_active(struct xenbus_device *dev,
		struct xensock_back_priv *priv,
		struct sock_mapping *map)
{
	unsigned long flags;
	int in_loop;


	disable_irq(map->irq);
	if (map->sock->sk != NULL) {
		bh_lock_sock(map->sock->sk);
		map->sock->sk->sk_user_data = NULL;
		map->sock->sk->sk_data_ready = map->saved_data_ready;
		bh_unlock_sock(map->sock->sk);
	}

	spin_lock_irqsave(&__xensock->iothreads[map->data_kthread].lock, flags);
	atomic_inc(&map->release);
	in_loop = !list_empty(&map->queue);
	if (in_loop)
		atomic_inc(&__xensock->iothreads[map->data_kthread].io);
	spin_unlock_irqrestore(&__xensock->iothreads[map->data_kthread].lock, flags);

	if (in_loop)
		wake_up_interruptible(&__xensock->iothreads[map->data_kthread].queue_io);

	while (in_loop) {
		cond_resched();
		spin_lock_irqsave(&__xensock->iothreads[map->data_kthread].lock, flags);
		in_loop = !list_empty(&map->queue);
		spin_unlock_irqrestore(&__xensock->iothreads[map->data_kthread].lock, flags);
	}

	down_write(&priv->xensocks_lock);
	list_del(&map->list);
	up_write(&priv->xensocks_lock);
	unbind_from_irqhandler(map->irq, map);
	sock_release(map->sock);
	xenbus_unmap_ring_vfree(dev, (void*)map->ring);
	kfree(map);

	return 0;
}

static int xensock_back_release_passive(struct xenbus_device *dev,
		struct xensock_back_priv *priv,
		struct sockpass_mapping *mappass)
{
	if (mappass->sock->sk != NULL) {
		bh_lock_sock(mappass->sock->sk);
		mappass->sock->sk->sk_user_data = NULL;
		mappass->sock->sk->sk_data_ready = mappass->saved_data_ready;
		bh_unlock_sock(mappass->sock->sk);
	}
	down_write(&priv->xensocks_lock);
	radix_tree_delete(&priv->socketpass_mappings, mappass->sockid);
	sock_release(mappass->sock);
	flush_workqueue(mappass->wq);
	destroy_workqueue(mappass->wq);
	kfree(mappass);
	up_write(&priv->xensocks_lock);

	return 0;
}

static int xensock_back_release(struct xenbus_device *dev,
		struct xen_xensock_request *req)
{
	struct xensock_back_priv *priv;
	struct sock_mapping *map, *n;
	struct sockpass_mapping *mappass;
	int ret = -EINVAL;
	struct xen_xensock_response *rsp;

	priv = dev_get_drvdata(&dev->dev);

	list_for_each_entry_safe(map, n, &priv->socket_mappings, list) {
		if (map->sockid == req->sockid) {
			ret = xensock_back_release_active(dev, priv, map);
			goto out;
		}
	}
	mappass = radix_tree_lookup(&priv->socketpass_mappings, req->sockid);
	if (mappass != NULL) {
		ret = xensock_back_release_passive(dev, priv, mappass);
		goto out;
	}

out:
	rsp = RING_GET_RESPONSE(&priv->ring, priv->ring.rsp_prod_pvt++);
	rsp->id = req->id;
	rsp->sockid = req->sockid;
	rsp->cmd = req->cmd;
	rsp->ret = ret;
	return 1;
}

static int xensock_conn_back_write(struct sock_mapping *map)
{
	struct xensock_ring_intf *intf = map->ring;
	struct msghdr msg;
	struct kvec vec[2];
	XENSOCK_RING_IDX cons, prod, size;
	int ret;

	cons = intf->out_cons;
	prod = intf->out_prod;
	mb();
	
	size = xensock_ring_queued(prod, cons, sizeof(intf->out));
	if (size == 0)
		return 0;

	memset(&msg, 0, sizeof(msg));
	msg.msg_flags |= MSG_DONTWAIT;
	msg.msg_iter.type = ITER_KVEC|READ;
	msg.msg_iter.count = size;
	if (MASK_XENSOCK_IDX(prod, intf->out) > MASK_XENSOCK_IDX(cons, intf->out)) {
		vec[0].iov_base = intf->out + MASK_XENSOCK_IDX(cons, intf->out);
		vec[0].iov_len = size;
		msg.msg_iter.kvec = vec;
		msg.msg_iter.nr_segs = 1;
	} else {
		vec[0].iov_base = intf->out + MASK_XENSOCK_IDX(cons, intf->out);
		vec[0].iov_len = sizeof(intf->out) - MASK_XENSOCK_IDX(cons, intf->out);
		vec[1].iov_base = intf->out;
		vec[1].iov_len = size - vec[0].iov_len;
		msg.msg_iter.kvec = vec;
		msg.msg_iter.nr_segs = 2;
	}

	atomic_set(&map->write, 0);
	ret = inet_sendmsg(map->sock, &msg, size);
	if (ret == -EAGAIN || ret < size) {
		atomic_inc(&map->write);
		atomic_inc(&__xensock->iothreads[map->data_kthread].io);
	}
	if (ret == -EAGAIN)
		return ret;

	wmb();			/* write ring before updating pointer */
	if (ret < 0) {
		intf->out_error = ret;
	} else {
		intf->out_error = 0;
		intf->out_cons = cons + ret;
		prod = intf->out_prod;
	}
	wmb();
	if (prod != cons + ret)
		atomic_inc(&map->write);
	notify_remote_via_irq(map->irq);

	return ret;
}

static void xensock_conn_back_read(unsigned long data)
{
	struct sock_mapping *map = (struct sock_mapping *)data;
	struct msghdr msg;
	struct kvec vec[2];
	XENSOCK_RING_IDX cons, prod, size, wanted;
	struct xensock_ring_intf *intf = map->ring;
	int ret;

	cons = intf->in_cons;
	prod = intf->in_prod;
	mb();

	size = xensock_ring_queued(prod, cons, sizeof(intf->in));
	if (size >= sizeof(intf->in))
		return;
	if (skb_queue_empty(&map->sock->sk->sk_receive_queue)) {
		atomic_set(&map->read, 0);
		return;
	}
	wanted = sizeof(intf->in) - size;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iter.type = ITER_KVEC|WRITE;
	msg.msg_iter.count = wanted;
	if (MASK_XENSOCK_IDX(prod, intf->in) < MASK_XENSOCK_IDX(cons, intf->in)) {
		vec[0].iov_base = intf->in + MASK_XENSOCK_IDX(prod, intf->in);
		vec[0].iov_len = wanted;
		msg.msg_iter.kvec = vec;
		msg.msg_iter.nr_segs = 1;
	} else {
		vec[0].iov_base = intf->in + MASK_XENSOCK_IDX(prod, intf->in);
		vec[0].iov_len = sizeof(intf->in) - MASK_XENSOCK_IDX(prod, intf->in);
		vec[1].iov_base = intf->in;
		vec[1].iov_len = wanted - vec[0].iov_len;
		msg.msg_iter.kvec = vec;
		msg.msg_iter.nr_segs = 2;
	}

	atomic_set(&map->read, 0);
	ret = inet_recvmsg(map->sock, &msg, wanted, MSG_DONTWAIT);
	BUG_ON(ret > 0 && ret > wanted);
	if (ret == -EAGAIN) /* shouldn't happen */
		return;
	if (!ret)
		ret = -ENOTCONN;
	if (ret > 0 && !skb_queue_empty(&map->sock->sk->sk_receive_queue))
		atomic_inc(&map->read);

	wmb();			/* write ring before updating pointer */
	if (ret < 0)
		intf->in_error = ret;
	else {
		intf->in_prod = prod + ret;
	}
	wmb();
	notify_remote_via_irq(map->irq);

	return;
}

static void xensock_pass_sk_data_ready(struct sock *sock)
{
	struct sockpass_mapping *mappass = sock->sk_user_data;
	struct xensock_back_priv *priv;
	struct xen_xensock_response *rsp;
	int notify;

	if (mappass == NULL)
		return;

	priv = mappass->priv;
	if (mappass->reqcopy.cmd == XENSOCK_POLL) {
		rsp = RING_GET_RESPONSE(&priv->ring, priv->ring.rsp_prod_pvt++);
		rsp->id = mappass->reqcopy.id;
		rsp->sockid = mappass->reqcopy.sockid;
		rsp->cmd = mappass->reqcopy.cmd;
		rsp->ret = 0;

		mappass->reqcopy.cmd = 0;
		RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&priv->ring, notify);
		if (notify)
			notify_remote_via_irq(mappass->priv->irq);
	} else
		queue_work(mappass->wq, &mappass->register_work);
}

static void xensock_sk_data_ready(struct sock *sock)
{
	struct sock_mapping *map = sock->sk_user_data;
	unsigned long flags;

	if (map == NULL)
		return;

	spin_lock_irqsave(&__xensock->iothreads[map->data_kthread].lock, flags);
	atomic_inc(&map->read);
	if (list_empty(&map->queue))
		list_add_tail(&map->queue, &__xensock->iothreads[map->data_kthread].wqs);
	spin_unlock_irqrestore(&__xensock->iothreads[map->data_kthread].lock, flags);
	atomic_inc(&__xensock->iothreads[map->data_kthread].io);
	wake_up_interruptible(&__xensock->iothreads[map->data_kthread].queue_io);
}

static int xensock_back_handle_cmd(struct xenbus_device *dev,
		struct xen_xensock_request *req)
{
	int ret = 0;
	printk("DEBUG %s %d cmd=%x\n",__func__,__LINE__,req->cmd);

	switch (req->cmd) {
	case XENSOCK_CONNECT:
		ret = xensock_back_connect(dev, req);
		break;
	case XENSOCK_RELEASE:
		ret = xensock_back_release(dev, req);
		break;
	case XENSOCK_BIND:
		ret = xensock_back_bind(dev, req);
		break;
	case XENSOCK_LISTEN:
		ret = xensock_back_listen(dev, req);
		break;
	case XENSOCK_ACCEPT:
		ret = xensock_back_accept(dev, req);
		break;
	case XENSOCK_POLL:
		ret = xensock_back_poll(dev, req);
		break;
	}
	return ret;
}

static irqreturn_t xensock_back_event(int irq, void *dev_id)
{
	struct xenbus_device *dev = dev_id;
	struct xensock_back_priv *priv = NULL;

	if (dev == NULL)
		return IRQ_HANDLED;

	priv = dev_get_drvdata(&dev->dev);
	if (priv == NULL)
		return IRQ_HANDLED;

	atomic_inc(&priv->work);
	wake_up(&priv->queue_work);

	return IRQ_HANDLED;
}

static irqreturn_t xensock_back_conn_event(int irq, void *sock_map)
{
	struct sock_mapping *map = sock_map;
	unsigned long flags;

	if (map == NULL || map->sock == NULL || map->sock->sk == NULL ||
			map->sock->sk->sk_user_data != map)
		return IRQ_HANDLED;

	spin_lock_irqsave(&__xensock->iothreads[map->data_kthread].lock, flags);
	atomic_inc(&map->write);
	if (list_empty(&map->queue))
		list_add_tail(&map->queue, &__xensock->iothreads[map->data_kthread].wqs);
	spin_unlock_irqrestore(&__xensock->iothreads[map->data_kthread].lock, flags);
	atomic_inc(&__xensock->iothreads[map->data_kthread].io);
	wake_up_interruptible(&__xensock->iothreads[map->data_kthread].queue_io);

	return IRQ_HANDLED;
}

static int xensock_back_iothread(void *arg)
{
	struct iothread_data *data = arg;
	int num = data->num;
	struct sock_mapping *map, *n;
	unsigned long flags;

	while (1) {
		if (atomic_sub_and_test(1, &__xensock->iothreads[num].io))
			wait_event_interruptible(__xensock->iothreads[num].queue_io, atomic_read(&__xensock->iothreads[num].io) > 0);

		if (kthread_should_stop())
			break;

		spin_lock_irqsave(&__xensock->iothreads[num].lock, flags);
		list_for_each_entry_safe(map, n, &__xensock->iothreads[num].wqs, queue) {
			if (map->data_kthread != num)
				continue;
			
			if (atomic_read(&map->release) > 0) {
				list_del_init(&map->queue);
				continue;
			}

			spin_unlock_irqrestore(&__xensock->iothreads[num].lock, flags);
			if (atomic_read(&map->read) > 0)
				xensock_conn_back_read((unsigned long)map);
			if (atomic_read(&map->write) > 0)
				xensock_conn_back_write(map);
			spin_lock_irqsave(&__xensock->iothreads[num].lock, flags);

			if (atomic_read(&map->read) == 0 && atomic_read(&map->write) == 0)
				list_del_init(&map->queue);
		}
		spin_unlock_irqrestore(&__xensock->iothreads[num].lock, flags);
	}

	return 0;
}

static int xensock_back_thread(void *data)
{
	int notify, notify_all = 0, more = 0;
	struct xen_xensock_request req;
	struct xenbus_device *dev = data;
	struct xensock_back_priv *priv = NULL;

	priv = dev_get_drvdata(&dev->dev);
	atomic_set(&priv->work, 1);

	while (1) {
		if (kthread_should_stop())
			break;

		while(RING_HAS_UNCONSUMED_REQUESTS(&priv->ring)) {
			RING_COPY_REQUEST(&priv->ring, priv->ring.req_cons++, &req);

			if (xensock_back_handle_cmd(dev, &req) > 0) {
				RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&priv->ring, notify);
				notify_all += notify;
			}
		}

		if (notify_all)
			notify_remote_via_irq(priv->irq);

		RING_FINAL_CHECK_FOR_REQUESTS(&priv->ring, more);

		if (!more && atomic_dec_and_test(&priv->work))
			wait_event(priv->queue_work, atomic_read(&priv->work) > 0);
	}

	return 0;
}

static int xensock_back_probe(struct xenbus_device *dev,
			 const struct xenbus_device_id *id)
{
	int err;

	err = xenbus_switch_state(dev, XenbusStateInitWait);
	if (err)
		return err;

	return 0;
}

static int xensock_back_remove(struct xenbus_device *dev)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
	return 0;
}

static int xensock_back_uevent(struct xenbus_device *xdev,
			  struct kobj_uevent_env *env)
{
	printk("DEBUG %s %d\n",__func__,__LINE__);
	return 0;
}

static int backend_connect(struct xenbus_device *dev)
{
	int err, evtchn;
	grant_ref_t ring_ref;
	void *addr = NULL;
	struct xensock_back_priv *priv = NULL;

	priv = kzalloc(sizeof(struct xensock_back_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	err = xenbus_scanf(XBT_NIL, dev->otherend, "port", "%u",
			&evtchn);
	if (err != 1) {
		err = -EINVAL;
		xenbus_dev_fatal(dev, err, "reading %s/event-channel", dev->otherend);
		goto error;
	}

	err = xenbus_scanf(XBT_NIL, dev->otherend, "ring-ref", "%u", &ring_ref);
	if (err != 1) {
		err = -EINVAL;
		xenbus_dev_fatal(dev, err, "reading %s/ring-ref", dev->otherend);
		goto error;
	}

	err = xenbus_map_ring_valloc(dev, &ring_ref, 1, &addr);
	if (err < 0)
		goto error;

	err = bind_interdomain_evtchn_to_irqhandler(dev->otherend_id, evtchn,
						    xensock_back_event, 0,
						    "xensock-backend", dev);
	if (err < 0)
		goto error;

	priv->dev = dev;
	priv->sring = addr;
	BACK_RING_INIT(&priv->ring, priv->sring, XEN_PAGE_SIZE * 1);
	priv->irq = err;
	INIT_LIST_HEAD(&priv->socket_mappings);
	INIT_RADIX_TREE(&priv->socketpass_mappings, GFP_KERNEL);
	init_rwsem(&priv->xensocks_lock);
	init_waitqueue_head(&priv->queue_work);
	priv->thread = kthread_create(xensock_back_thread, (void *)dev, "xensock-back");
	dev_set_drvdata(&dev->dev, priv);
	down_write(&__xensock->privs_lock);
	list_add_tail(&priv->list, &__xensock->privs);
	up_write(&__xensock->privs_lock);
	wake_up_process(priv->thread);

	return 0;

 error:
	if (addr != NULL)
		xenbus_unmap_ring_vfree(dev, addr);
	unbind_from_irqhandler(priv->irq, dev);
	kfree(priv);
	return err;
}

static int backend_disconnect(struct xenbus_device *dev)
{
	struct xensock_back_priv *priv;
	struct sock_mapping *map, *n;
	struct sockpass_mapping *mappass;
	struct radix_tree_iter iter;
	void **slot;
	

	priv = dev_get_drvdata(&dev->dev);

	list_for_each_entry_safe(map, n, &priv->socket_mappings, list) {
		xensock_back_release_active(dev, priv, map);
	}
	radix_tree_for_each_slot(slot, &priv->socketpass_mappings, &iter, 0) {
		mappass = radix_tree_deref_slot(slot);
		if (!mappass || radix_tree_exception(mappass)) {
			if (radix_tree_deref_retry(mappass)) {
				slot = radix_tree_iter_retry(&iter);
				continue;
			}
		} else
			xensock_back_release_passive(dev, priv, mappass);
	}
	xenbus_unmap_ring_vfree(dev, (void*)priv->sring);
	unbind_from_irqhandler(priv->irq, dev);
	list_del(&priv->list);
	kfree(priv);
	dev_set_drvdata(&dev->dev, NULL);

	return 0;
}

static void set_backend_state(struct xenbus_device *dev,
			      enum xenbus_state state)
{
	while (dev->state != state) {
		switch (dev->state) {
		case XenbusStateClosed:
			switch (state) {
			case XenbusStateInitWait:
			case XenbusStateConnected:
				xenbus_switch_state(dev, XenbusStateInitWait);
				break;
			case XenbusStateClosing:
				xenbus_switch_state(dev, XenbusStateClosing);
				break;
			default:
				BUG();
			}
			break;
		case XenbusStateInitWait:
		case XenbusStateInitialised:
			switch (state) {
			case XenbusStateConnected:
				backend_connect(dev);
				xenbus_switch_state(dev, XenbusStateConnected);
				break;
			case XenbusStateClosing:
			case XenbusStateClosed:
				xenbus_switch_state(dev, XenbusStateClosing);
				break;
			default:
				BUG();
			}
			break;
		case XenbusStateConnected:
			switch (state) {
			case XenbusStateInitWait:
			case XenbusStateClosing:
			case XenbusStateClosed:
				down_write(&__xensock->privs_lock);
				backend_disconnect(dev);
				up_write(&__xensock->privs_lock);
				xenbus_switch_state(dev, XenbusStateClosing);
				break;
			default:
				BUG();
			}
			break;
		case XenbusStateClosing:
			switch (state) {
			case XenbusStateInitWait:
			case XenbusStateConnected:
			case XenbusStateClosed:
				xenbus_switch_state(dev, XenbusStateClosed);
				break;
			default:
				BUG();
			}
			break;
		default:
			BUG();
		}
	}
}

static void xensock_back_changed(struct xenbus_device *dev,
			     enum xenbus_state frontend_state)
{
	switch (frontend_state) {
	case XenbusStateInitialising:
		set_backend_state(dev, XenbusStateInitWait);
		break;

	case XenbusStateInitialised:
	case XenbusStateConnected:
		set_backend_state(dev, XenbusStateConnected);
		break;

	case XenbusStateClosing:
		set_backend_state(dev, XenbusStateClosing);
		break;

	case XenbusStateClosed:
		set_backend_state(dev, XenbusStateClosed);
		if (xenbus_dev_is_online(dev))
			break;
		/* fall through if not online */
	case XenbusStateUnknown:
		set_backend_state(dev, XenbusStateClosed);
		device_unregister(&dev->dev);
		break;

	default:
		xenbus_dev_fatal(dev, -EINVAL, "saw state %d at frontend",
				 frontend_state);
		break;
	}
}

static const struct xenbus_device_id xensock_back_ids[] = {
	{ "xensock" },
	{ "" }
};

static struct xenbus_driver xensock_back_driver = {
	.ids = xensock_back_ids,
	.probe = xensock_back_probe,
	.remove = xensock_back_remove,
	.uevent = xensock_back_uevent,
	.otherend_changed = xensock_back_changed,
};

static int __init xensock_back_init(void)
{
	int ret, i, cpu;

	if (!xen_domain())
		return -ENODEV;

	ret = xenbus_register_backend(&xensock_back_driver);
	if (ret < 0)
		return ret;

	__xensock = kzalloc(sizeof(*__xensock), GFP_KERNEL);
	if (!__xensock)
		goto error;

	init_rwsem(&__xensock->privs_lock);
	INIT_LIST_HEAD(&__xensock->privs);
	__xensock->nr_iothreads = num_online_cpus();
	__xensock->iothreads = kzalloc(sizeof(*__xensock->iothreads) * __xensock->nr_iothreads, GFP_KERNEL);
	if (!__xensock->iothreads)
		goto error;
	i = 0;
	for_each_online_cpu(cpu) {
		atomic_set(&__xensock->iothreads[i].io, 1);
		init_waitqueue_head(&__xensock->iothreads[i].queue_io);
		spin_lock_init(&__xensock->iothreads[i].lock);
		INIT_LIST_HEAD(&__xensock->iothreads[i].wqs);
		__xensock->iothreads[i].data.num = i;
		__xensock->iothreads[i].iothread = kthread_create(xensock_back_iothread, (void *)&__xensock->iothreads[i].data, "xensock-back-io-%u", i);
		kthread_bind(__xensock->iothreads[i].iothread, cpu);
		wake_up_process(__xensock->iothreads[i].iothread);
		i++;
	}
	return 0;

error:
	xenbus_unregister_driver(&xensock_back_driver);
	kfree(__xensock->iothreads);
	kfree(__xensock);
	return -ENOMEM;
}

module_init(xensock_back_init);

static void __exit xensock_back_fin(void)
{
	int i;
	struct xensock_back_priv *priv, *npriv;

	down_write(&__xensock->privs_lock);
	list_for_each_entry_safe(priv, npriv, &__xensock->privs, list) {
		backend_disconnect(priv->dev);
	}
	up_write(&__xensock->privs_lock);

	for (i = 0; i < __xensock->nr_iothreads; i++) {
		atomic_set(&__xensock->iothreads[i].io, 1);
		kthread_stop(__xensock->iothreads[i].iothread);
		wake_up_process(__xensock->iothreads[i].iothread);
	}

	xenbus_unregister_driver(&xensock_back_driver);
	kfree(__xensock->iothreads);
	kfree(__xensock);
	return;
}

module_exit(xensock_back_fin);

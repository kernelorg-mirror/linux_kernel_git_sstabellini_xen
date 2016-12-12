/*
 * linux/fs/9p/trans_xen
 *
 * Xen transport layer.
 *
 * Copyright (C) 2016 by Stefano Stabellini <stefano@aporeto.com>
 */ 

#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/xen.h>
#include <xen/xenbus.h>
#include <xen/interface/io/9pfs.h>

#include <linux/module.h>
#include <linux/spinlock.h>
#include <net/9p/9p.h>
#include <net/9p/client.h>
#include <net/9p/transport.h>

#define XEN_9PFS_NUM_RINGS 2

struct xen_9pfs_dataring {
	struct xen_9pfs_front_priv *priv;

	struct xen_9pfs_data_intf *intf;
	grant_ref_t ref;
	int evtchn;
	int irq;
	spinlock_t lock;

	void *bytes;
	struct xen_9pfs_data ring;
	wait_queue_head_t wq;
	struct work_struct work;
};

struct xen_9pfs_front_priv {
	struct list_head list;
	struct xenbus_device *dev;
	char *tag;
	struct p9_client *client;

	int num_rings;
	struct xen_9pfs_dataring *rings;
};
static LIST_HEAD(xen_9pfs_devs);


/* We don't currently allow canceling of virtio requests */
static int p9_xen_cancel(struct p9_client *client, struct p9_req_t *req)
{
	return 1;
}

static int p9_xen_create(struct p9_client *client, const char *addr, char *args)
{
	struct xen_9pfs_front_priv *priv = NULL;

	list_for_each_entry(priv, &xen_9pfs_devs, list) {
		if (!strcmp(priv->tag, addr))
			break;
	}
	if (strcmp(priv->tag, addr))
		return -EINVAL;

	priv->client = client; 
	return 0;
}

static void p9_xen_close(struct p9_client *client)
{
}

static int p9_xen_write_todo(struct xen_9pfs_dataring *ring, XEN_9PFS_RING_IDX size)
{
	XEN_9PFS_RING_IDX cons, prod;

	cons = ring->intf->out_cons;
	prod = ring->intf->out_prod;
	mb();

	if (XEN_9PFS_RING_SIZE - xen_9pfs_ring_queued(prod, cons) >= size)
		return 1;
	else
		return 0;
}

static int p9_xen_request(struct p9_client *client, struct p9_req_t *p9_req)
{
	struct xen_9pfs_front_priv *priv = NULL;
	XEN_9PFS_RING_IDX cons, cons2, prod, masked_cons, masked_prod;
	unsigned long flags;
	uint32_t size = p9_req->tc->size;
	struct xen_9pfs_dataring *ring;
	int num, notify = 0;

	list_for_each_entry(priv, &xen_9pfs_devs, list) {
		if (priv->client == client)
			break;
	}
	if (priv == NULL || priv->client != client)
		return -EINVAL;

	num = p9_req->tc->tag % priv->num_rings;
	ring = &priv->rings[num];

again:
	ring->intf->out_event = 1;
	mb();
	while (wait_event_interruptible(ring->wq,
				p9_xen_write_todo(ring, size) > 0) != 0);

	ring->intf->out_event = 0;
	mb();
	spin_lock_irqsave(&ring->lock, flags);
	cons = ring->intf->out_cons;
	prod = ring->intf->out_prod;
	rmb();

	if (XEN_9PFS_RING_SIZE - xen_9pfs_ring_queued(prod, cons) < size) {
		spin_unlock_irqrestore(&ring->lock, flags);
		goto again;
	}

	masked_prod = _MASK_XEN_9PFS_IDX(prod);
	masked_cons = _MASK_XEN_9PFS_IDX(cons);

	if (masked_prod < masked_cons) {
		memcpy(ring->ring.out + masked_prod, p9_req->tc->sdata, size);
	} else {
		if (size > XEN_9PFS_RING_SIZE - masked_prod) {
			memcpy(ring->ring.out + masked_prod, p9_req->tc->sdata, XEN_9PFS_RING_SIZE - masked_prod);
			memcpy(ring->ring.out, p9_req->tc->sdata + XEN_9PFS_RING_SIZE - masked_prod,
					size - (XEN_9PFS_RING_SIZE - masked_prod));
		} else {
			memcpy(ring->ring.out + masked_prod, p9_req->tc->sdata, size );
		}
	}

	p9_req->status = REQ_STATUS_SENT;
	wmb();			/* write ring before updating pointer */
	ring->intf->out_prod += size;
	mb();
	cons2 = ring->intf->out_cons;
	if (cons2 == cons || cons2 == prod)
		notify = 1;
	spin_unlock_irqrestore(&ring->lock, flags);
	if (notify)
		notify_remote_via_irq(ring->irq);

	return 0;
}

static void p9_xen_response(struct work_struct *work)
{
	struct xen_9pfs_front_priv *priv;
	struct xen_9pfs_dataring *ring;
	XEN_9PFS_RING_IDX cons, prod, masked_cons, masked_prod;
	struct xen_9pfs_header h;
	struct p9_req_t *req;
	int status = REQ_STATUS_ERROR;

	ring = container_of(work, struct xen_9pfs_dataring, work);
	priv = ring->priv;

	while (1) {
		cons = ring->intf->in_cons;
		prod = ring->intf->in_prod;
		rmb();

		if (xen_9pfs_ring_queued(prod, cons) < sizeof(h)) {
			mb();
			if (ring->intf->in_event)
				notify_remote_via_irq(ring->irq);
			return;
		}

		masked_prod = _MASK_XEN_9PFS_IDX(prod);
		masked_cons = _MASK_XEN_9PFS_IDX(cons);

		xen_9pfs_read_header(ring->ring.in, &masked_prod, &masked_cons, &h);

		req = p9_tag_lookup(priv->client, h.tag);
		if (!req || req->status != REQ_STATUS_SENT) {
			printk("DEBUG %s %d wrong req tag=%x\n",__func__,__LINE__,h.tag);
			cons += h.size;
			wmb();
			ring->intf->in_cons = cons;
			continue;
		}

		memcpy(req->rc, &h, sizeof(h));
		req->rc->offset = 0;

		masked_cons = _MASK_XEN_9PFS_IDX(cons);

		if (masked_prod > masked_cons) {
			memcpy(req->rc->sdata, ring->ring.in + masked_cons, h.size);
		} else {
			if (h.size > (XEN_9PFS_RING_SIZE - masked_cons)) {
				memcpy(req->rc->sdata, ring->ring.in + masked_cons, XEN_9PFS_RING_SIZE - masked_cons);
				memcpy(req->rc->sdata + XEN_9PFS_RING_SIZE - masked_cons, ring->ring.in, h.size - (XEN_9PFS_RING_SIZE - masked_cons));
			} else {
				memcpy(req->rc->sdata, ring->ring.in + masked_cons, h.size);
			}
		}
		wmb();
		ring->intf->in_cons += h.size;

		if (req->status != REQ_STATUS_ERROR)
			status = REQ_STATUS_RCVD;

		p9_client_cb(priv->client, req, status);
	}
}

static irqreturn_t xen_9pfs_front_event_handler(int irq, void *r)
{
	struct xen_9pfs_dataring *ring = r;

	if (!ring)
		return IRQ_HANDLED;
	if (!ring->priv->client)
		return IRQ_HANDLED;

	wake_up_interruptible(&ring->wq);

	schedule_work(&ring->work);

	return IRQ_HANDLED;
}

static struct p9_trans_module p9_xen_trans = {
	.name = "xen",
	.maxsize = XEN_9PFS_RING_SIZE,
	.def = 1,
	.create = p9_xen_create,
	.close = p9_xen_close,
	.request = p9_xen_request,
	.cancel = p9_xen_cancel,
	.owner = THIS_MODULE,
};

static const struct xenbus_device_id xen_9pfs_front_ids[] = {
	{ "9pfs" },
	{ "" }
};

static int xen_9pfs_front_remove(struct xenbus_device *dev)
{
	return -1;
}


static int xen_9pfs_front_alloc_dataring(struct xenbus_device *dev, struct xen_9pfs_dataring *ring)
{
	int i;
	int ret = -EFAULT;

	init_waitqueue_head(&ring->wq);
	spin_lock_init(&ring->lock);
	INIT_WORK(&ring->work, p9_xen_response);

	ring->intf = (struct xen_9pfs_data_intf *) __get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!ring->intf)
		goto error;
	memset(ring->intf, 0, XEN_PAGE_SIZE);
	ring->bytes = (void*)__get_free_pages(GFP_KERNEL | __GFP_ZERO, XEN_9PFS_RING_ORDER);
	if (ring->bytes == NULL) {
		ret = -ENOMEM;
		goto error;
	}
	for (i = 0; i < (1 << XEN_9PFS_RING_ORDER); i++)
		ring->intf->ref[i] = gnttab_grant_foreign_access(dev->otherend_id, pfn_to_gfn(virt_to_pfn((void*)ring->bytes) + i), 0);
	ring->ref = gnttab_grant_foreign_access(dev->otherend_id, pfn_to_gfn(virt_to_pfn((void*)ring->intf)), 0);
	ring->ring.in = ring->bytes;
	ring->ring.out = ring->bytes + XEN_9PFS_RING_SIZE;

	ret = xenbus_alloc_evtchn(dev, &ring->evtchn);
	if (ret)
		goto error;
	ring->irq = bind_evtchn_to_irqhandler(ring->evtchn, xen_9pfs_front_event_handler,
					0, "xen_9pfs-frontend", ring);
	if (ring->irq < 0) {
		ret = ring->irq;
		goto error;
	}

error:
	return ret;
}

static int xen_9pfs_front_probe(struct xenbus_device *dev,
			  const struct xenbus_device_id *id)
{
	int ret = -EFAULT, i;
	struct xenbus_transaction xbt;
	struct xen_9pfs_front_priv *priv = NULL;

	priv = kzalloc(sizeof(struct xen_9pfs_front_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	
	priv->dev = dev;
	priv->num_rings = XEN_9PFS_NUM_RINGS;
	priv->rings = kzalloc(sizeof(struct xen_9pfs_dataring) * priv->num_rings, GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
 again:
	ret = xenbus_transaction_start(&xbt);
	if (ret) {
		xenbus_dev_fatal(dev, ret, "starting transaction");
		goto error;
	}
	ret = xenbus_printf(xbt, dev->nodename, "num-rings", "%u", priv->num_rings);
	if (ret)
		goto error_xenbus;
	for (i = 0; i < priv->num_rings; i++) {
		char str[16];

		priv->rings[i].priv = priv;
		ret = xen_9pfs_front_alloc_dataring(dev, &priv->rings[i]);
		if (ret < 0)
			goto error_xenbus;

		sprintf(str, "ring-ref-%u", i);
		ret = xenbus_printf(xbt, dev->nodename, str, "%d", priv->rings[i].ref);
		if (ret)
			goto error_xenbus;

		sprintf(str, "port-%u", i);
		ret = xenbus_printf(xbt, dev->nodename, str, "%u", priv->rings[i].evtchn);
		if (ret)
			goto error_xenbus;
	}
	priv->tag = xenbus_read(xbt, dev->nodename, "tag", NULL);
	if (ret)
		goto error_xenbus;
	ret = xenbus_transaction_end(xbt, 0);
	if (ret) {
		if (ret == -EAGAIN)
			goto again;
		xenbus_dev_fatal(dev, ret, "completing transaction");
		goto error;
	}

	v9fs_register_trans(&p9_xen_trans);

	list_add_tail(&priv->list, &xen_9pfs_devs);
	dev_set_drvdata(&dev->dev, priv);
	xenbus_switch_state(dev, XenbusStateInitialised);

	return 0;

 error_xenbus:
	xenbus_transaction_end(xbt, 1);
	xenbus_dev_fatal(dev, ret, "writing xenstore");
 error:
	xen_9pfs_front_remove(dev);
	return ret;
}

static int xen_9pfs_front_resume(struct xenbus_device *dev)
{
	dev_warn(&dev->dev, "suspsend/resume unsupported\n");
	return 0;
}

static void xen_9pfs_front_changed(struct xenbus_device *dev,
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
static struct xenbus_driver xen_9pfs_front_driver = {
	.ids = xen_9pfs_front_ids,
	.probe = xen_9pfs_front_probe,
	.remove = xen_9pfs_front_remove,
	.resume = xen_9pfs_front_resume,
	.otherend_changed = xen_9pfs_front_changed,
};

int p9_trans_xen_init(void)
{
	if (!xen_domain())
		return -ENODEV;

	pr_info("Initialising Xen virtual socket driver\n");

	return xenbus_register_frontend(&xen_9pfs_front_driver);
}
module_init(p9_trans_xen_init);

void p9_trans_xen_exit(void)
{
	v9fs_unregister_trans(&p9_xen_trans);
}
module_exit(p9_trans_xen_exit);

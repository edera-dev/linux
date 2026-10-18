// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/fs/9p/trans_xen
 *
 * Xen transport layer.
 *
 * Copyright (C) 2017 by Stefano Stabellini <stefano@aporeto.com>
 */

#include <xen/events.h>
#include <xen/grant_table.h>
#include <xen/xen.h>
#include <xen/xenbus.h>
#include <xen/interface/io/9pfs.h>

#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/fs_context.h>
#include <net/9p/9p.h>
#include <net/9p/client.h>
#include <net/9p/transport.h>

#define XEN_9PFS_NUM_RINGS 2
#define XEN_9PFS_RING_ORDER 9
#define XEN_9PFS_RING_SIZE(ring)  XEN_FLEX_RING_SIZE(ring->intf->ring_order)

struct xen_9pfs_header {
	uint32_t size;
	uint8_t id;
	uint16_t tag;

	/* uint8_t sdata[]; */
} __attribute__((packed));

/* One per ring, more than one per 9pfs share */
struct xen_9pfs_dataring {
	struct xen_9pfs_front_priv *priv;

	struct xen_9pfs_data_intf *intf;
	grant_ref_t ref;
	int evtchn;
	int irq;
	/* protect a ring from concurrent accesses */
	spinlock_t lock;

	struct xen_9pfs_data data;
	wait_queue_head_t wq;
	struct work_struct work;
};

/* One per 9pfs share */
struct xen_9pfs_front_priv {
	struct list_head list;
	struct xenbus_device *dev;
	char *tag;
	struct p9_client *client;

	struct xen_9pfs_dataring *rings;
	unsigned int ring_order;
	/* Requests hold it for read while they use the rings; a restore
	 * holds it for write to replace them.
	 */
	struct rw_semaphore rings_sem;
	bool connected;
};

static LIST_HEAD(xen_9pfs_devs);
static DEFINE_RWLOCK(xen_9pfs_lock);
/* Requests that find their frontend disconnected wait here for it. */
static DECLARE_WAIT_QUEUE_HEAD(xen_9pfs_connect_wq);

/* We don't currently allow canceling of requests */
static int p9_xen_cancel(struct p9_client *client, struct p9_req_t *req)
{
	return 1;
}

static int p9_xen_create(struct p9_client *client, struct fs_context *fc)
{
	struct v9fs_context *ctx = fc->fs_private;
	/* Select the device by its tag. An explicit tag= lets several mounts
	 * of one frontend carry distinct source strings (distinct
	 * /proc/mounts device names); without it the source string is the tag,
	 * as before.
	 */
	const char *addr = (ctx && ctx->tag) ? ctx->tag : fc->source;
	struct xen_9pfs_front_priv *priv;

	if (addr == NULL)
		return -EINVAL;

	read_lock(&xen_9pfs_lock);
	list_for_each_entry(priv, &xen_9pfs_devs, list) {
		if (!strcmp(priv->tag, addr)) {
			priv->client = client;
			read_unlock(&xen_9pfs_lock);
			return 0;
		}
	}
	read_unlock(&xen_9pfs_lock);
	return -EINVAL;
}

static void p9_xen_close(struct p9_client *client)
{
	struct xen_9pfs_front_priv *priv;

	read_lock(&xen_9pfs_lock);
	list_for_each_entry(priv, &xen_9pfs_devs, list) {
		if (priv->client == client) {
			priv->client = NULL;
			read_unlock(&xen_9pfs_lock);
			return;
		}
	}
	read_unlock(&xen_9pfs_lock);
}

static bool p9_xen_write_todo(struct xen_9pfs_dataring *ring, RING_IDX size)
{
	RING_IDX cons, prod;

	cons = ring->intf->out_cons;
	prod = ring->intf->out_prod;
	virt_mb();

	return XEN_9PFS_RING_SIZE(ring) -
		xen_9pfs_queued(prod, cons, XEN_9PFS_RING_SIZE(ring)) >= size;
}

/* True once @client's frontend is connected, or gone. */
static bool p9_xen_ready(struct p9_client *client)
{
	struct xen_9pfs_front_priv *priv;
	bool ready = true;

	read_lock(&xen_9pfs_lock);
	list_for_each_entry(priv, &xen_9pfs_devs, list) {
		if (priv->client == client) {
			ready = READ_ONCE(priv->connected);
			break;
		}
	}
	read_unlock(&xen_9pfs_lock);
	return ready;
}

static int p9_xen_request(struct p9_client *client, struct p9_req_t *p9_req)
{
	struct xen_9pfs_front_priv *priv;
	RING_IDX cons, prod, masked_cons, masked_prod;
	unsigned long flags;
	u32 size = p9_req->tc.size;
	struct xen_9pfs_dataring *ring;
	int num;

lookup:
	read_lock(&xen_9pfs_lock);
	list_for_each_entry(priv, &xen_9pfs_devs, list) {
		if (priv->client == client)
			break;
	}
	read_unlock(&xen_9pfs_lock);
	if (list_entry_is_head(priv, &xen_9pfs_devs, list))
		return -EINVAL;

	down_read(&priv->rings_sem);
	if (!READ_ONCE(priv->connected)) {
		up_read(&priv->rings_sem);
		if (wait_event_killable(xen_9pfs_connect_wq,
					p9_xen_ready(client)))
			return -ERESTARTSYS;
		goto lookup;
	}

	num = p9_req->tc.tag % XEN_9PFS_NUM_RINGS;
	ring = &priv->rings[num];

again:
	while (io_wait_event_killable(ring->wq,
				      !READ_ONCE(priv->connected) ||
				      p9_xen_write_todo(ring, size)) != 0)
		;
	if (!READ_ONCE(priv->connected)) {
		up_read(&priv->rings_sem);
		goto lookup;
	}

	spin_lock_irqsave(&ring->lock, flags);
	cons = ring->intf->out_cons;
	prod = ring->intf->out_prod;
	virt_mb();

	if (XEN_9PFS_RING_SIZE(ring) -
	    xen_9pfs_queued(prod, cons, XEN_9PFS_RING_SIZE(ring)) < size) {
		spin_unlock_irqrestore(&ring->lock, flags);
		goto again;
	}

	masked_prod = xen_9pfs_mask(prod, XEN_9PFS_RING_SIZE(ring));
	masked_cons = xen_9pfs_mask(cons, XEN_9PFS_RING_SIZE(ring));

	xen_9pfs_write_packet(ring->data.out, p9_req->tc.sdata, size,
			      &masked_prod, masked_cons,
			      XEN_9PFS_RING_SIZE(ring));

	WRITE_ONCE(p9_req->status, REQ_STATUS_SENT);
	virt_wmb();			/* write ring before updating pointer */
	prod += size;
	ring->intf->out_prod = prod;
	spin_unlock_irqrestore(&ring->lock, flags);
	notify_remote_via_irq(ring->irq);
	up_read(&priv->rings_sem);
	p9_req_put(client, p9_req);

	return 0;
}

static void p9_xen_response(struct work_struct *work)
{
	struct xen_9pfs_front_priv *priv;
	struct xen_9pfs_dataring *ring;
	RING_IDX cons, prod, masked_cons, masked_prod;
	struct xen_9pfs_header h;
	struct p9_req_t *req;
	int status;

	ring = container_of(work, struct xen_9pfs_dataring, work);
	priv = ring->priv;

	while (1) {
		cons = ring->intf->in_cons;
		prod = ring->intf->in_prod;
		virt_rmb();

		if (xen_9pfs_queued(prod, cons, XEN_9PFS_RING_SIZE(ring)) <
		    sizeof(h)) {
			notify_remote_via_irq(ring->irq);
			return;
		}

		masked_prod = xen_9pfs_mask(prod, XEN_9PFS_RING_SIZE(ring));
		masked_cons = xen_9pfs_mask(cons, XEN_9PFS_RING_SIZE(ring));

		/* First, read just the header */
		xen_9pfs_read_packet(&h, ring->data.in, sizeof(h),
				     masked_prod, &masked_cons,
				     XEN_9PFS_RING_SIZE(ring));

		req = p9_tag_lookup(priv->client, h.tag);
		if (!req || req->status != REQ_STATUS_SENT) {
			dev_warn(&priv->dev->dev, "Wrong req tag=%x\n", h.tag);
			cons += h.size;
			virt_mb();
			ring->intf->in_cons = cons;
			continue;
		}

		if (h.size > req->rc.capacity) {
			dev_warn(&priv->dev->dev,
				 "requested packet size too big: %d for tag %d with capacity %zd\n",
				 h.size, h.tag, req->rc.capacity);
			WRITE_ONCE(req->status, REQ_STATUS_ERROR);
			goto recv_error;
		}

		req->rc.size = h.size;
		req->rc.id = h.id;
		req->rc.tag = h.tag;
		req->rc.offset = 0;

		masked_cons = xen_9pfs_mask(cons, XEN_9PFS_RING_SIZE(ring));
		/* Then, read the whole packet (including the header) */
		xen_9pfs_read_packet(req->rc.sdata, ring->data.in, h.size,
				     masked_prod, &masked_cons,
				     XEN_9PFS_RING_SIZE(ring));

recv_error:
		virt_mb();
		cons += h.size;
		ring->intf->in_cons = cons;

		status = (req->status != REQ_STATUS_ERROR) ?
			REQ_STATUS_RCVD : REQ_STATUS_ERROR;

		p9_client_cb(priv->client, req, status);
	}
}

static irqreturn_t xen_9pfs_front_event_handler(int irq, void *r)
{
	struct xen_9pfs_dataring *ring = r;

	if (!ring || !ring->priv->client) {
		/* ignore spurious interrupt */
		return IRQ_HANDLED;
	}

	wake_up(&ring->wq);
	schedule_work(&ring->work);

	return IRQ_HANDLED;
}

static struct p9_trans_module p9_xen_trans = {
	.name = "xen",
	.maxsize = 1 << (XEN_9PFS_RING_ORDER + XEN_PAGE_SHIFT - 2),
	.pooled_rbuffers = false,
	.def = true,
	.supports_vmalloc = false,
	.share_client = true,
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

static void xen_9pfs_front_free_rings(struct xen_9pfs_front_priv *priv)
{
	int i, j;

	for (i = 0; i < XEN_9PFS_NUM_RINGS; i++) {
		struct xen_9pfs_dataring *ring = &priv->rings[i];

		cancel_work_sync(&ring->work);

		if (!ring->intf)
			break;
		if (ring->irq >= 0) {
			unbind_from_irqhandler(ring->irq, ring);
			ring->irq = -1;
		}
		if (ring->data.in) {
			for (j = 0; j < (1 << ring->intf->ring_order); j++) {
				grant_ref_t ref;

				ref = ring->intf->ref[j];
				gnttab_end_foreign_access(ref, NULL);
				ring->intf->ref[j] = INVALID_GRANT_REF;
			}
			free_pages_exact(ring->data.in,
					 1UL << (ring->intf->ring_order +
						 XEN_PAGE_SHIFT));
			ring->data.in = NULL;
			ring->data.out = NULL;
		}
		if (ring->ref != INVALID_GRANT_REF) {
			gnttab_end_foreign_access(ring->ref, NULL);
			ring->ref = INVALID_GRANT_REF;
		}
		free_page((unsigned long)ring->intf);
		ring->intf = NULL;
	}
}

static void xen_9pfs_front_free(struct xen_9pfs_front_priv *priv)
{
	if (priv->rings) {
		xen_9pfs_front_free_rings(priv);
		kfree(priv->rings);
	}
	kfree(priv->tag);
	kfree(priv);
}

static void xen_9pfs_front_remove(struct xenbus_device *dev)
{
	struct xen_9pfs_front_priv *priv;

	write_lock(&xen_9pfs_lock);
	priv = dev_get_drvdata(&dev->dev);
	if (priv == NULL) {
		write_unlock(&xen_9pfs_lock);
		return;
	}
	dev_set_drvdata(&dev->dev, NULL);
	list_del(&priv->list);
	write_unlock(&xen_9pfs_lock);
	wake_up_all(&xen_9pfs_connect_wq);

	xen_9pfs_front_free(priv);
}

/* Fail the requests the backend held when the rings went away; nothing
 * will answer them now, and whether they took effect is unknown.
 */
static void xen_9pfs_front_fail_sent(struct p9_client *client)
{
	struct p9_req_t *req, *next;
	unsigned long flags;
	LIST_HEAD(sent);
	int id;

	spin_lock_irqsave(&client->lock, flags);
	idr_for_each_entry(&client->reqs, req, id) {
		if (READ_ONCE(req->status) == REQ_STATUS_SENT &&
		    p9_req_try_get(req))
			list_add_tail(&req->req_list, &sent);
	}
	spin_unlock_irqrestore(&client->lock, flags);

	list_for_each_entry_safe(req, next, &sent, req_list) {
		list_del(&req->req_list);
		req->t_err = -EIO;
		p9_client_cb(client, req, REQ_STATUS_ERROR);
	}
}

static void xen_9pfs_front_disconnect(struct xen_9pfs_front_priv *priv)
{
	int i;

	WRITE_ONCE(priv->connected, false);
	for (i = 0; i < XEN_9PFS_NUM_RINGS; i++) {
		if (priv->rings[i].intf)
			wake_up(&priv->rings[i].wq);
	}

	down_write(&priv->rings_sem);
	if (priv->client) {
		/* Collect the replies the backend wrote before it went away. */
		for (i = 0; i < XEN_9PFS_NUM_RINGS; i++) {
			struct xen_9pfs_dataring *ring = &priv->rings[i];

			if (!ring->intf)
				break;
			schedule_work(&ring->work);
			flush_work(&ring->work);
		}
	}
	xen_9pfs_front_free_rings(priv);
	up_write(&priv->rings_sem);

	if (priv->client)
		xen_9pfs_front_fail_sent(priv->client);
}

static int xen_9pfs_front_alloc_dataring(struct xenbus_device *dev,
					 struct xen_9pfs_dataring *ring,
					 unsigned int order)
{
	int i = 0;
	int ret = -ENOMEM;
	void *bytes = NULL;

	ring->intf = NULL;
	ring->data.in = NULL;
	ring->data.out = NULL;
	ring->ref = INVALID_GRANT_REF;
	ring->irq = -1;

	init_waitqueue_head(&ring->wq);
	spin_lock_init(&ring->lock);
	INIT_WORK(&ring->work, p9_xen_response);

	ring->intf = (struct xen_9pfs_data_intf *)get_zeroed_page(GFP_KERNEL);
	if (!ring->intf)
		return ret;
	ret = gnttab_grant_foreign_access(dev->otherend_id,
					  virt_to_gfn(ring->intf), 0);
	if (ret < 0)
		goto out;
	ring->ref = ret;
	bytes = alloc_pages_exact(1UL << (order + XEN_PAGE_SHIFT),
				  GFP_KERNEL | __GFP_ZERO);
	if (!bytes) {
		ret = -ENOMEM;
		goto out;
	}
	for (; i < (1 << order); i++) {
		ret = gnttab_grant_foreign_access(
				dev->otherend_id, virt_to_gfn(bytes) + i, 0);
		if (ret < 0)
			goto out;
		ring->intf->ref[i] = ret;
	}
	ring->intf->ring_order = order;
	ring->data.in = bytes;
	ring->data.out = bytes + XEN_FLEX_RING_SIZE(order);

	ret = xenbus_alloc_evtchn(dev, &ring->evtchn);
	if (ret)
		goto out;
	ring->irq = bind_evtchn_to_irqhandler(ring->evtchn,
					      xen_9pfs_front_event_handler,
					      0, "xen_9pfs-frontend", ring);
	if (ring->irq >= 0)
		return 0;

	xenbus_free_evtchn(dev, ring->evtchn);
	ret = ring->irq;
out:
	if (bytes) {
		for (i--; i >= 0; i--)
			gnttab_end_foreign_access(ring->intf->ref[i], NULL);
		free_pages_exact(bytes, 1UL << (order + XEN_PAGE_SHIFT));
		ring->data.in = NULL;
		ring->data.out = NULL;
	}
	if (ring->ref != INVALID_GRANT_REF) {
		gnttab_end_foreign_access(ring->ref, NULL);
		ring->ref = INVALID_GRANT_REF;
	}
	if (ring->intf) {
		free_page((unsigned long)ring->intf);
		ring->intf = NULL;
	}
	ring->irq = -1;
	return ret;
}

static int xen_9pfs_front_publish(struct xenbus_device *dev,
				  struct xen_9pfs_front_priv *priv)
{
	int ret, i;
	struct xenbus_transaction xbt;

 again:
	ret = xenbus_transaction_start(&xbt);
	if (ret) {
		xenbus_dev_fatal(dev, ret, "starting transaction");
		return ret;
	}
	ret = xenbus_printf(xbt, dev->nodename, "version", "%u", 1);
	if (ret)
		goto error_xenbus;
	ret = xenbus_printf(xbt, dev->nodename, "num-rings", "%u",
			    XEN_9PFS_NUM_RINGS);
	if (ret)
		goto error_xenbus;

	for (i = 0; i < XEN_9PFS_NUM_RINGS; i++) {
		char str[16];

		BUILD_BUG_ON(XEN_9PFS_NUM_RINGS > 9);
		sprintf(str, "ring-ref%d", i);
		ret = xenbus_printf(xbt, dev->nodename, str, "%d",
				    priv->rings[i].ref);
		if (ret)
			goto error_xenbus;

		sprintf(str, "event-channel-%d", i);
		ret = xenbus_printf(xbt, dev->nodename, str, "%u",
				    priv->rings[i].evtchn);
		if (ret)
			goto error_xenbus;
	}
	if (!priv->tag) {
		priv->tag = xenbus_read(xbt, dev->nodename, "tag", NULL);
		if (IS_ERR(priv->tag)) {
			ret = PTR_ERR(priv->tag);
			priv->tag = NULL;
			goto error_xenbus;
		}
	}
	ret = xenbus_transaction_end(xbt, 0);
	if (ret) {
		if (ret == -EAGAIN)
			goto again;
		xenbus_dev_fatal(dev, ret, "completing transaction");
		return ret;
	}
	return 0;

 error_xenbus:
	xenbus_transaction_end(xbt, 1);
	xenbus_dev_fatal(dev, ret, "writing xenstore");
	return ret;
}

static int xen_9pfs_front_init(struct xenbus_device *dev)
{
	int ret, i;
	struct xen_9pfs_front_priv *priv;
	char *versions, *v, *token;
	bool version_1 = false;
	unsigned int max_rings, max_ring_order, len = 0, version;

	versions = xenbus_read(XBT_NIL, dev->otherend, "versions", &len);
	if (IS_ERR(versions))
		return PTR_ERR(versions);
	for (v = versions; (token = strsep(&v, ",")); ) {
		if (!*token)
			continue;

		ret = kstrtouint(token, 10, &version);
		if (ret) {
			kfree(versions);
			return ret;
		}
		if (version == 1)
			version_1 = true;
	}
	kfree(versions);
	if (!version_1)
		return -EINVAL;

	max_rings = xenbus_read_unsigned(dev->otherend, "max-rings", 0);
	if (max_rings < XEN_9PFS_NUM_RINGS)
		return -EINVAL;
	max_ring_order = xenbus_read_unsigned(dev->otherend,
					      "max-ring-page-order", 0);
	if (max_ring_order > XEN_9PFS_RING_ORDER)
		max_ring_order = XEN_9PFS_RING_ORDER;
	if (p9_xen_trans.maxsize > XEN_FLEX_RING_SIZE(max_ring_order))
		p9_xen_trans.maxsize = XEN_FLEX_RING_SIZE(max_ring_order) / 2;

	priv = kzalloc_obj(*priv);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;
	priv->ring_order = max_ring_order;
	init_rwsem(&priv->rings_sem);
	priv->rings = kzalloc_objs(*priv->rings, XEN_9PFS_NUM_RINGS);
	if (!priv->rings) {
		kfree(priv);
		return -ENOMEM;
	}

	for (i = 0; i < XEN_9PFS_NUM_RINGS; i++) {
		priv->rings[i].priv = priv;
		ret = xen_9pfs_front_alloc_dataring(dev, &priv->rings[i],
						    max_ring_order);
		if (ret < 0)
			goto error;
	}

	ret = xen_9pfs_front_publish(dev, priv);
	if (ret)
		goto error;

	write_lock(&xen_9pfs_lock);
	dev_set_drvdata(&dev->dev, priv);
	list_add_tail(&priv->list, &xen_9pfs_devs);
	write_unlock(&xen_9pfs_lock);

	xenbus_switch_state(dev, XenbusStateInitialised);
	return 0;

 error:
	xen_9pfs_front_free(priv);
	return ret;
}

/* Give a restored frontend new rings, keeping its clients and their fids. */
static int xen_9pfs_front_reconnect(struct xenbus_device *dev,
				    struct xen_9pfs_front_priv *priv)
{
	int ret, i;

	if (xenbus_read_unsigned(dev->otherend, "max-rings", 0) <
	    XEN_9PFS_NUM_RINGS ||
	    xenbus_read_unsigned(dev->otherend, "max-ring-page-order", 0) <
	    priv->ring_order) {
		xenbus_dev_fatal(dev, -EINVAL,
				 "backend cannot serve the rings in use");
		return -EINVAL;
	}

	down_write(&priv->rings_sem);
	for (i = 0; i < XEN_9PFS_NUM_RINGS; i++) {
		ret = xen_9pfs_front_alloc_dataring(dev, &priv->rings[i],
						    priv->ring_order);
		if (ret < 0)
			goto error;
	}
	ret = xen_9pfs_front_publish(dev, priv);
	if (ret)
		goto error;
	up_write(&priv->rings_sem);

	xenbus_switch_state(dev, XenbusStateInitialised);
	return 0;

 error:
	xen_9pfs_front_free_rings(priv);
	up_write(&priv->rings_sem);
	return ret;
}

static int xen_9pfs_front_probe(struct xenbus_device *dev,
				const struct xenbus_device_id *id)
{
	return 0;
}

static int xen_9pfs_front_resume(struct xenbus_device *dev)
{
	struct xen_9pfs_front_priv *priv = dev_get_drvdata(&dev->dev);
	int ret;

	if (!priv)
		return 0;

	xen_9pfs_front_disconnect(priv);
	/* xenbus set dev->state back to Initialising without publishing it,
	 * and a backend that still serves the old rings only goes back to
	 * InitWait once it sees that.
	 */
	ret = xenbus_printf(XBT_NIL, dev->nodename, "state", "%d",
			    XenbusStateInitialising);
	if (ret)
		dev_warn(&dev->dev, "publishing Initialising failed: %d\n",
			 ret);
	return 0;
}

static void xen_9pfs_front_changed(struct xenbus_device *dev,
				   enum xenbus_state backend_state)
{
	struct xen_9pfs_front_priv *priv;

	switch (backend_state) {
	case XenbusStateReconfiguring:
	case XenbusStateReconfigured:
	case XenbusStateInitialising:
	case XenbusStateInitialised:
	case XenbusStateUnknown:
		break;

	case XenbusStateInitWait:
		if (dev->state != XenbusStateInitialising)
			break;

		priv = dev_get_drvdata(&dev->dev);
		if (priv)
			xen_9pfs_front_reconnect(dev, priv);
		else
			xen_9pfs_front_init(dev);
		break;

	case XenbusStateConnected:
		/* Until we publish rings, a Connected backend is one still
		 * serving the rings a resume dropped.
		 */
		if (dev->state != XenbusStateInitialised)
			break;

		priv = dev_get_drvdata(&dev->dev);
		if (priv) {
			WRITE_ONCE(priv->connected, true);
			wake_up_all(&xen_9pfs_connect_wq);
		}
		xenbus_switch_state(dev, XenbusStateConnected);
		break;

	case XenbusStateClosed:
		if (dev->state == XenbusStateClosed)
			break;
		fallthrough;	/* Missed the backend's CLOSING state */
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

static int __init p9_trans_xen_init(void)
{
	int rc;

	if (!xen_domain())
		return -ENODEV;

	pr_info("Initialising Xen transport for 9pfs\n");

	v9fs_register_trans(&p9_xen_trans);
	rc = xenbus_register_frontend(&xen_9pfs_front_driver);
	if (rc)
		v9fs_unregister_trans(&p9_xen_trans);

	return rc;
}
module_init(p9_trans_xen_init);
MODULE_ALIAS_9P("xen");

static void __exit p9_trans_xen_exit(void)
{
	v9fs_unregister_trans(&p9_xen_trans);
	return xenbus_unregister_driver(&xen_9pfs_front_driver);
}
module_exit(p9_trans_xen_exit);

MODULE_ALIAS("xen:9pfs");
MODULE_AUTHOR("Stefano Stabellini <stefano@aporeto.com>");
MODULE_DESCRIPTION("Xen Transport for 9P");
MODULE_LICENSE("GPL");

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
#include <linux/delay.h>
#include <linux/unaligned.h>
#include <net/9p/9p.h>
#include <net/9p/client.h>
#include <net/9p/transport.h>

#define XEN_9PFS_NUM_RINGS 2
#define XEN_9PFS_RING_ORDER 9
#define XEN_9PFS_RING_SIZE(ring)  XEN_FLEX_RING_SIZE(ring->intf->ring_order)

/* Edera backend extension that hands the fid table to the guest at suspend
 * and takes it back after a restore, advertised as feature-session-transfer.
 * The ids sit outside the 9P2000.L range. Tsession_save offset[4] answers
 * Rsession_save total[4] next[4] count[4] entry[count]; Tsession_restore
 * msize[4] flags[4] count[4] entry[count] answers Rsession_restore
 * restored[4]. The frontend stores the count[4] entry[count] tails without
 * looking inside them.
 */
#define P9_XEN_TSESSION_SAVE		200
#define P9_XEN_RSESSION_SAVE		201
#define P9_XEN_TSESSION_RESTORE		202
#define P9_XEN_RSESSION_RESTORE		203
#define P9_XEN_SESSION_RESET		1
#define XEN_9PFS_CTL_TIMEOUT		(5 * HZ)
#define XEN_9PFS_DRAIN_TIMEOUT		(2 * HZ)

struct xen_9pfs_session_chunk {
	struct list_head list;
	u32 len;
	u8 data[];
};

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

	/* Session chunks saved at the last suspend, in guest memory so that
	 * they survive into a restored or forked domain.
	 */
	struct list_head session;
	/* A control request on ring 0, answered with tag P9_NOTAG. */
	bool ctl_pending;
	struct completion ctl_done;
	u8 *ctl_reply;
	u32 ctl_reply_cap;
	u32 ctl_reply_len;
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

/* Value of the "tag=" mount option in @args, copied into @buf, or NULL when
 * absent. Matched only at an option boundary. The device is selected by tag,
 * so several mounts of one frontend can carry distinct source strings.
 */
static const char *p9_xen_opt_tag(const char *args, char *buf, size_t buflen)
{
	const char *p = args;

	while (p && *p) {
		if (!strncmp(p, "tag=", 4)) {
			size_t n = strcspn(p + 4, ",");

			if (n == 0 || n >= buflen)
				return NULL;
			memcpy(buf, p + 4, n);
			buf[n] = '\0';
			return buf;
		}
		p = strchr(p, ',');
		if (p)
			p++;
	}
	return NULL;
}

static int p9_xen_create(struct p9_client *client, const char *addr, char *args)
{
	struct xen_9pfs_front_priv *priv;
	char tagbuf[64];
	const char *tag;

	/* Prefer an explicit tag= option so the mount source can differ per
	 * mount (distinct /proc/mounts device names); fall back to the source.
	 */
	tag = p9_xen_opt_tag(args, tagbuf, sizeof(tagbuf));
	if (!tag)
		tag = addr;
	if (tag == NULL)
		return -EINVAL;

	read_lock(&xen_9pfs_lock);
	list_for_each_entry(priv, &xen_9pfs_devs, list) {
		if (!strcmp(priv->tag, tag)) {
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
	while (wait_event_killable(ring->wq,
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

		/* A client's own P9_NOTAG request, Tversion, never maps to
		 * ring 0; see xen_9pfs_front_ctl().
		 */
		if (ring == priv->rings && h.tag == P9_NOTAG &&
		    READ_ONCE(priv->ctl_pending)) {
			if (h.size <= priv->ctl_reply_cap) {
				u32 size = XEN_9PFS_RING_SIZE(ring);

				masked_cons = xen_9pfs_mask(cons, size);
				xen_9pfs_read_packet(priv->ctl_reply,
						     ring->data.in, h.size,
						     masked_prod, &masked_cons,
						     size);
				priv->ctl_reply_len = h.size;
			}
			virt_mb();		/* read ring before freeing it */
			cons += h.size;
			ring->intf->in_cons = cons;
			WRITE_ONCE(priv->ctl_pending, false);
			complete(&priv->ctl_done);
			continue;
		}

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
	.def = 1,
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

		if (!priv->rings[i].intf)
			break;
		if (priv->rings[i].irq > 0)
			unbind_from_irqhandler(priv->rings[i].irq, ring);
		if (priv->rings[i].data.in) {
			for (j = 0;
			     j < (1 << priv->rings[i].intf->ring_order);
			     j++) {
				grant_ref_t ref;

				ref = priv->rings[i].intf->ref[j];
				gnttab_end_foreign_access(ref, NULL);
			}
			free_pages_exact(priv->rings[i].data.in,
			   1UL << (priv->rings[i].intf->ring_order +
				   XEN_PAGE_SHIFT));
		}
		gnttab_end_foreign_access(priv->rings[i].ref, NULL);
		free_page((unsigned long)priv->rings[i].intf);
		priv->rings[i].intf = NULL;
	}
}

static void xen_9pfs_front_session_free(struct xen_9pfs_front_priv *priv)
{
	struct xen_9pfs_session_chunk *chunk, *next;

	list_for_each_entry_safe(chunk, next, &priv->session, list) {
		list_del(&chunk->list);
		kvfree(chunk);
	}
}

static void xen_9pfs_front_free(struct xen_9pfs_front_priv *priv)
{
	if (priv->rings) {
		xen_9pfs_front_free_rings(priv);
		kfree(priv->rings);
	}
	xen_9pfs_front_session_free(priv);
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

/* Send control request @req on ring 0 and wait for the backend's answer in
 * @reply, which must carry @id and at least @body bytes after the header.
 * Returns the reply length or a negative errno. Callers hold rings_sem.
 */
static int xen_9pfs_front_ctl(struct xen_9pfs_front_priv *priv,
			      const u8 *req, u32 len, u8 *reply, u32 cap,
			      u8 id, u32 body)
{
	struct xen_9pfs_dataring *ring = &priv->rings[0];
	RING_IDX cons, prod, masked_cons, masked_prod;
	unsigned long flags;
	u32 reply_len;

	BUILD_BUG_ON(P9_NOTAG % XEN_9PFS_NUM_RINGS == 0);
	if (len > XEN_9PFS_RING_SIZE(ring))
		return -E2BIG;
	if (!wait_event_timeout(ring->wq, p9_xen_write_todo(ring, len),
				XEN_9PFS_CTL_TIMEOUT))
		return -ETIMEDOUT;

	reinit_completion(&priv->ctl_done);
	priv->ctl_reply = reply;
	priv->ctl_reply_cap = cap;
	priv->ctl_reply_len = 0;
	WRITE_ONCE(priv->ctl_pending, true);

	spin_lock_irqsave(&ring->lock, flags);
	cons = ring->intf->out_cons;
	prod = ring->intf->out_prod;
	virt_mb();			/* read indexes before the ring */
	if (XEN_9PFS_RING_SIZE(ring) -
	    xen_9pfs_queued(prod, cons, XEN_9PFS_RING_SIZE(ring)) < len) {
		spin_unlock_irqrestore(&ring->lock, flags);
		WRITE_ONCE(priv->ctl_pending, false);
		return -EBUSY;
	}
	masked_prod = xen_9pfs_mask(prod, XEN_9PFS_RING_SIZE(ring));
	masked_cons = xen_9pfs_mask(cons, XEN_9PFS_RING_SIZE(ring));
	xen_9pfs_write_packet(ring->data.out, req, len, &masked_prod,
			      masked_cons, XEN_9PFS_RING_SIZE(ring));
	virt_wmb();			/* write ring before updating pointer */
	prod += len;
	ring->intf->out_prod = prod;
	spin_unlock_irqrestore(&ring->lock, flags);
	notify_remote_via_irq(ring->irq);

	if (!wait_for_completion_timeout(&priv->ctl_done,
					 XEN_9PFS_CTL_TIMEOUT)) {
		WRITE_ONCE(priv->ctl_pending, false);
		/* A response worker may already be copying into @reply. */
		flush_work(&ring->work);
		return -ETIMEDOUT;
	}

	reply_len = priv->ctl_reply_len;
	if (reply_len < P9_HDRSZ || get_unaligned_le32(reply) != reply_len)
		return -EPROTO;
	if (reply[4] == P9_RLERROR && reply_len >= P9_HDRSZ + 4)
		return -(int)get_unaligned_le32(reply + P9_HDRSZ);
	if (reply[4] != id || reply_len < P9_HDRSZ + body)
		return -EPROTO;
	return reply_len;
}

static int xen_9pfs_front_suspend(struct xenbus_device *dev)
{
	struct xen_9pfs_front_priv *priv = dev_get_drvdata(&dev->dev);
	struct xen_9pfs_session_chunk *chunk;
	u32 offset = 0, total, next, len;
	struct p9_client *client;
	u8 req[P9_HDRSZ + 4];
	unsigned long deadline;
	struct p9_req_t *p9_req;
	unsigned long flags;
	bool idle;
	u8 *reply;
	int ret, id;

	if (!priv)
		return 0;
	xen_9pfs_front_session_free(priv);
	if (!xenbus_read_unsigned(dev->otherend, "feature-session-transfer", 0))
		return 0;

	down_read(&priv->rings_sem);
	client = priv->client;
	if (!READ_ONCE(priv->connected) || !client)
		goto out;

	/* Let what is in flight finish first, so that the saved fids match
	 * what the guest has seen answered.
	 */
	deadline = jiffies + XEN_9PFS_DRAIN_TIMEOUT;
	do {
		idle = true;
		spin_lock_irqsave(&client->lock, flags);
		idr_for_each_entry(&client->reqs, p9_req, id) {
			if (READ_ONCE(p9_req->status) == REQ_STATUS_SENT) {
				idle = false;
				break;
			}
		}
		spin_unlock_irqrestore(&client->lock, flags);
		if (idle)
			break;
		usleep_range(1000, 2000);
	} while (time_before(jiffies, deadline));

	reply = kvmalloc(XEN_9PFS_RING_SIZE(priv->rings), GFP_KERNEL);
	if (!reply) {
		ret = -ENOMEM;
		goto failed;
	}
	do {
		put_unaligned_le32(sizeof(req), req);
		req[4] = P9_XEN_TSESSION_SAVE;
		put_unaligned_le16(P9_NOTAG, req + 5);
		put_unaligned_le32(offset, req + P9_HDRSZ);
		ret = xen_9pfs_front_ctl(priv, req, sizeof(req), reply,
					 XEN_9PFS_RING_SIZE(priv->rings),
					 P9_XEN_RSESSION_SAVE, 12);
		if (ret < 0)
			break;
		total = get_unaligned_le32(reply + P9_HDRSZ);
		next = get_unaligned_le32(reply + P9_HDRSZ + 4);
		len = ret - (P9_HDRSZ + 8);
		ret = 0;
		if (next <= offset && next < total) {
			ret = -EPROTO;
			break;
		}
		chunk = kvmalloc(struct_size(chunk, data, len), GFP_KERNEL);
		if (!chunk) {
			ret = -ENOMEM;
			break;
		}
		chunk->len = len;
		memcpy(chunk->data, reply + P9_HDRSZ + 8, len);
		list_add_tail(&chunk->list, &priv->session);
		offset = next;
	} while (offset < total);
	kvfree(reply);

 failed:
	if (ret) {
		dev_warn(&dev->dev, "saving the 9P session failed: %d\n", ret);
		xen_9pfs_front_session_free(priv);
	}
 out:
	up_read(&priv->rings_sem);
	return 0;
}

static int xen_9pfs_front_alloc_dataring(struct xenbus_device *dev,
					 struct xen_9pfs_dataring *ring,
					 unsigned int order)
{
	int i = 0;
	int ret = -ENOMEM;
	void *bytes = NULL;

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
	}
	gnttab_end_foreign_access(ring->ref, NULL);
	free_page((unsigned long)ring->intf);
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
	/* Lets the toolstack tell, before it exports a domain, whether its
	 * open files can follow it into a new backend.
	 */
	ret = xenbus_printf(xbt, dev->nodename, "feature-session-transfer",
			    "%u", 1);
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
	char *versions, *v;
	unsigned int max_rings, max_ring_order, len = 0;

	versions = xenbus_read(XBT_NIL, dev->otherend, "versions", &len);
	if (IS_ERR(versions))
		return PTR_ERR(versions);
	for (v = versions; *v; v++) {
		if (simple_strtoul(v, &v, 10) == 1) {
			v = NULL;
			break;
		}
	}
	if (v) {
		kfree(versions);
		return -EINVAL;
	}
	kfree(versions);
	max_rings = xenbus_read_unsigned(dev->otherend, "max-rings", 0);
	if (max_rings < XEN_9PFS_NUM_RINGS)
		return -EINVAL;
	max_ring_order = xenbus_read_unsigned(dev->otherend,
					      "max-ring-page-order", 0);
	if (max_ring_order > XEN_9PFS_RING_ORDER)
		max_ring_order = XEN_9PFS_RING_ORDER;
	if (p9_xen_trans.maxsize > XEN_FLEX_RING_SIZE(max_ring_order))
		p9_xen_trans.maxsize = XEN_FLEX_RING_SIZE(max_ring_order) / 2;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;
	priv->ring_order = max_ring_order;
	init_rwsem(&priv->rings_sem);
	INIT_LIST_HEAD(&priv->session);
	init_completion(&priv->ctl_done);
	priv->rings = kcalloc(XEN_9PFS_NUM_RINGS, sizeof(*priv->rings),
			      GFP_KERNEL);
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
	struct xen_9pfs_session_chunk *chunk;
	struct xen_9pfs_front_priv *priv;
	u8 reply[P9_HDRSZ + 4];
	u32 len, flags;
	int ret;
	u8 *req;

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
		if (!priv) {
			xenbus_switch_state(dev, XenbusStateConnected);
			break;
		}

		/* Replay the fids saved at suspend before any request can use
		 * them.
		 */
		down_read(&priv->rings_sem);
		ret = 0;
		flags = P9_XEN_SESSION_RESET;
		if (!list_empty(&priv->session) && priv->client &&
		    xenbus_read_unsigned(dev->otherend,
					 "feature-session-transfer", 0)) {
			list_for_each_entry(chunk, &priv->session, list) {
				len = P9_HDRSZ + 8 + chunk->len;
				req = kvmalloc(len, GFP_KERNEL);
				if (!req) {
					ret = -ENOMEM;
					break;
				}
				put_unaligned_le32(len, req);
				req[4] = P9_XEN_TSESSION_RESTORE;
				put_unaligned_le16(P9_NOTAG, req + 5);
				put_unaligned_le32(priv->client->msize,
						   req + P9_HDRSZ);
				put_unaligned_le32(flags, req + P9_HDRSZ + 4);
				memcpy(req + P9_HDRSZ + 8, chunk->data,
				       chunk->len);
				ret = xen_9pfs_front_ctl(priv, req, len, reply,
							 sizeof(reply),
							 P9_XEN_RSESSION_RESTORE,
							 4);
				kvfree(req);
				if (ret < 0)
					break;
				flags = 0;
			}
		}
		up_read(&priv->rings_sem);
		if (ret < 0)
			dev_warn(&dev->dev, "restoring the 9P session failed: %d\n",
				 ret);
		xen_9pfs_front_session_free(priv);

		WRITE_ONCE(priv->connected, true);
		wake_up_all(&xen_9pfs_connect_wq);
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
	.suspend = xen_9pfs_front_suspend,
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

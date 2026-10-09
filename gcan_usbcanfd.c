// SPDX-License-Identifier: GPL-2.0
/*
 * @Copyright: Copyright (C) 2026 Gahan Ai Pvt Ltd
 * @Author: Pavan Patil
 * @Date: 2026-10-08
 * @Last Modified by:   Pavan Patil
 * @Last Modified time: 2026-10-09 12:36:00
 * @Description: SocketCAN driver for the GCAN USBCANFD adapter (USB 0c66:000e).
 *
 * The protocol was reverse engineered from USB captures of the vendor Windows driver
 * (see docs/PROTOCOL.md). Endpoints (all bulk, 512 byte packets):
 *   0x02 OUT / 0x82 IN : command channel, every command is echoed back as its ack
 *   0x81 IN            : stream of length-prefixed receive records
 *   0x01 OUT / 0x83 IN : transmit records (always a 1024 byte transfer) / 4 byte ack "aa 55 <n> 00"
 *
 * Every channel shows up as canN. Bitrates are chosen with iproute2, e.g.
 *   ip link set can0 up type can bitrate 500000 dbitrate 2000000 fd on
 * Only the bitrates in the tables below are accepted, because the device takes a table index.
 *
 * Target kernel: 6.8 (flat can_priv bittiming layout).
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <asm/unaligned.h>
#include <linux/timekeeping.h>
#include <linux/rtc.h>
#include <linux/netdevice.h>
#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/skb.h>

#define GCAN_VID		0x0c66
#define GCAN_PID		0x000e

#define GCAN_EP_TX_OUT		0x01
#define GCAN_EP_RX_IN		0x81
#define GCAN_EP_CMD_OUT		0x02
#define GCAN_EP_CMD_IN		0x82
#define GCAN_EP_ACK_IN		0x83

#define GCAN_MAX_CHAN		2
#define GCAN_NUM_RX_URBS	4
#define GCAN_RX_BUF_SIZE	1024
#define GCAN_TX_BLOCK_SIZE	1024	/* the device only accepts 1024 byte transmit transfers */
#define GCAN_TX_ECHO_SLOTS	64	/* frames per channel that may be queued or in flight */
#define GCAN_NUM_TX_BLOCKS	4	/* one being filled, up to GCAN_TX_INFLIGHT on the wire, one spare */
#define GCAN_TX_INFLIGHT	2
#define GCAN_ACK_BUF_SIZE	64
#define GCAN_CMD_BUF_SIZE	512
#define GCAN_CMD_TIMEOUT_MS	1000

#define GCAN_REC_HDR_LEN	16	/* len ch ts[8] flags dlen id[4] */
#define GCAN_REC_STATUS_LEN	30	/* periodic channel status record, see gcan_status_record() */

/* Status record (30 bytes): len ch(=type) ts[8] ECR[4] PSR[4] ...; type 0/1 = channel 0/1, type 3 = unknown.
 * ECR/PSR are the Bosch M_CAN error counter and protocol status registers (vendor header ECanFDVci.h). */
#define GCAN_ST_ECR		10
#define GCAN_ST_PSR		14
#define GCAN_PSR_EP		BIT(5)
#define GCAN_PSR_EW		BIT(6)
#define GCAN_PSR_BO		BIT(7)

#define GCAN_FLAG_FD		BIT(0)
#define GCAN_FLAG_EXT		BIT(1)
#define GCAN_FLAG_RTR		BIT(2)
#define GCAN_FLAG_BRS		BIT(3)

/* Table index == value the device expects (order from the vendor header ECanFDVci.h) */
static const u32 gcan_nominal_rates[] = {
	1000000, 800000, 500000, 400000, 250000, 200000, 125000, 100000,
	80000, 62500, 50000, 40000, 25000, 20000, 10000, 5000,
};

static const u32 gcan_data_rates[] = {
	5000000, 4000000, 2000000, 1000000, 800000, 500000, 400000, 250000, 200000,
	125000, 100000, 80000, 62500, 50000, 40000, 25000, 20000, 10000, 5000,
};

struct gcan_dev;
struct gcan_chan;

/* Records of both channels share the 1024 byte transmit blocks (each record carries its channel). */
#define GCAN_TX_MAX_RECS	(GCAN_TX_BLOCK_SIZE / GCAN_REC_HDR_LEN)

struct gcan_tx_ent {
	u8 ch;
	u8 idx;			/* echo skb slot of that channel */
};

struct gcan_tx_blk {
	struct gcan_dev *dev;
	struct urb *urb;
	u8 *buf;
	unsigned int len;	/* bytes used */
	unsigned int n;		/* records */
	bool busy;		/* submitted, completion pending */
	struct gcan_tx_ent ent[GCAN_TX_MAX_RECS];
};

/* can_priv must stay the first member: alloc_candev() puts it at netdev_priv() */
struct gcan_chan {
	struct can_priv can;
	struct net_device *netdev;
	struct gcan_dev *dev;
	u8 ch;
	bool registered;
	u8 tec, rec;		/* last transmit / receive error counters from the status record */

	unsigned int tx_head;	/* next echo slot; tx_head and tx_cnt are protected by dev->tx_lock */
	unsigned int tx_cnt;	/* frames queued in a block or in flight */
};

struct gcan_dev {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct gcan_chan *chan[GCAN_MAX_CHAN];

	struct mutex cmd_lock;
	u8 *cmd_buf;

	struct mutex open_lock;
	int open_count;
	bool disconnected;

	spinlock_t tx_lock;
	struct gcan_tx_blk tx_blk[GCAN_NUM_TX_BLOCKS];
	struct gcan_tx_blk *tx_cur;	/* block being filled */
	int tx_inflight;
	struct usb_anchor tx_anchor;

	struct usb_anchor rx_anchor;
	struct urb *rx_urb[GCAN_NUM_RX_URBS];
	u8 *rx_buf[GCAN_NUM_RX_URBS];
	struct urb *ack_urb;
	u8 *ack_buf;
};

/* ---------------------------------------------------------------- commands (EP 0x02 / 0x82) */

/* Send a command and wait for its echo. The echo must start with the same 4 bytes. */
static int gcan_cmd(struct gcan_dev *d, const void *req, int len)
{
	int actual = 0, err;

	if (d->disconnected)
		return -ENODEV;

	mutex_lock(&d->cmd_lock);
	memcpy(d->cmd_buf, req, len);
	err = usb_bulk_msg(d->udev, usb_sndbulkpipe(d->udev, GCAN_EP_CMD_OUT & 0x0f),
			   d->cmd_buf, len, &actual, GCAN_CMD_TIMEOUT_MS);
	if (err) {
		dev_err(&d->intf->dev, "command write failed: %d\n", err);
		goto out;
	}
	err = usb_bulk_msg(d->udev, usb_rcvbulkpipe(d->udev, GCAN_EP_CMD_IN & 0x0f),
			   d->cmd_buf, GCAN_CMD_BUF_SIZE, &actual, GCAN_CMD_TIMEOUT_MS);
	if (err) {
		dev_err(&d->intf->dev, "command %02x ack failed: %d\n", ((const u8 *)req)[0], err);
		goto out;
	}
	if (actual < min(len, 4) || memcmp(d->cmd_buf, req, min(len, 4))) {
		dev_err(&d->intf->dev, "unexpected ack for command %02x (%d bytes)\n",
			((const u8 *)req)[0], actual);
		err = -EPROTO;
	}
out:
	mutex_unlock(&d->cmd_lock);
	return err;
}

static int gcan_cmd_time_sync(struct gcan_dev *d)
{
	struct tm tm;
	u8 pkt[10] = { 0xaa, 0x55, 0x07 };
	u16 year;

	time64_to_tm(ktime_get_real_seconds(), 0, &tm);
	year = tm.tm_year + 1900;
	pkt[3] = year & 0xff;
	pkt[4] = year >> 8;
	pkt[5] = tm.tm_mon + 1;
	pkt[6] = tm.tm_mday;
	pkt[7] = tm.tm_hour;
	pkt[8] = tm.tm_min;
	pkt[9] = tm.tm_sec;
	return gcan_cmd(d, pkt, sizeof(pkt));
}

static int gcan_rate_index(const u32 *tab, int n, u32 rate)
{
	int i;

	for (i = 0; i < n; i++)
		if (tab[i] == rate)
			return i;
	return -EINVAL;
}

static int gcan_cmd_init(struct gcan_chan *c)
{
	u32 nom = c->can.bittiming.bitrate;
	u32 dat = (c->can.ctrlmode & CAN_CTRLMODE_FD) ? c->can.data_bittiming.bitrate : nom;
	int ni = gcan_rate_index(gcan_nominal_rates, ARRAY_SIZE(gcan_nominal_rates), nom);
	int di = gcan_rate_index(gcan_data_rates, ARRAY_SIZE(gcan_data_rates), dat);
	u8 pkt[80] = { 0 };

	if (ni < 0 || di < 0)
		return -EINVAL;

	pkt[0] = 0x01;
	pkt[1] = c->ch;
	pkt[2] = 0x03;		/* receive all standard + extended ids */
	pkt[3] = 0x00;		/* POSITIVE_SEND: the vendor default when transmitting */
	put_unaligned_le32(nom, &pkt[4]);
	put_unaligned_le32(dat, &pkt[8]);
	/* pkt[12], pkt[13]: filter bits, unused */
	pkt[14] = ni;
	pkt[15] = di;
	return gcan_cmd(c->dev, pkt, sizeof(pkt));
}

static int gcan_cmd_simple(struct gcan_chan *c, u8 op)
{
	u8 pkt[4] = { op, c->ch, 0xaa, 0x55 };

	return gcan_cmd(c->dev, pkt, sizeof(pkt));
}

/* ---------------------------------------------------------------- receive (EP 0x81) */

static void gcan_rx_record(struct gcan_dev *d, const u8 *rec, unsigned int n)
{
	struct gcan_chan *c;
	struct net_device *netdev;
	struct sk_buff *skb;
	u8 ch = rec[1], flags = rec[10], dlen = rec[11];
	u32 id = get_unaligned_le32(&rec[12]);

	if (ch >= GCAN_MAX_CHAN || !d->chan[ch])
		return;
	c = d->chan[ch];
	netdev = c->netdev;
	if (!netif_running(netdev))
		return;
	if (n < GCAN_REC_HDR_LEN + dlen)
		goto drop;

	if (flags & GCAN_FLAG_FD) {
		struct canfd_frame *cf;

		if (dlen > CANFD_MAX_DLEN)
			goto drop;
		skb = alloc_canfd_skb(netdev, &cf);
		if (!skb)
			goto drop;
		cf->can_id = (flags & GCAN_FLAG_EXT) ? (id & CAN_EFF_MASK) | CAN_EFF_FLAG : id & CAN_SFF_MASK;
		cf->len = dlen;
		cf->flags = CANFD_FDF | ((flags & GCAN_FLAG_BRS) ? CANFD_BRS : 0);
		memcpy(cf->data, rec + GCAN_REC_HDR_LEN, dlen);
		netdev->stats.rx_bytes += dlen;
	} else {
		struct can_frame *cf;

		if (dlen > CAN_MAX_DLEN)
			goto drop;
		skb = alloc_can_skb(netdev, &cf);
		if (!skb)
			goto drop;
		cf->can_id = (flags & GCAN_FLAG_EXT) ? (id & CAN_EFF_MASK) | CAN_EFF_FLAG : id & CAN_SFF_MASK;
		cf->len = dlen;
		if (flags & GCAN_FLAG_RTR) {
			cf->can_id |= CAN_RTR_FLAG;
		} else {
			memcpy(cf->data, rec + GCAN_REC_HDR_LEN, dlen);
			netdev->stats.rx_bytes += dlen;
		}
	}
	netdev->stats.rx_packets++;
	netif_rx(skb);
	return;
drop:
	netdev->stats.rx_dropped++;
}

/* CAN error state of one direction from its error counter (ISO 11898: warning >= 96, passive >= 128) */
static enum can_state gcan_counter_state(u8 cnt)
{
	if (cnt >= 128)
		return CAN_STATE_ERROR_PASSIVE;
	if (cnt >= 96)
		return CAN_STATE_ERROR_WARNING;
	return CAN_STATE_ERROR_ACTIVE;
}

static void gcan_status_record(struct gcan_dev *d, const u8 *rec)
{
	struct gcan_chan *c;
	struct net_device *netdev;
	struct can_frame *cf;
	struct sk_buff *skb;
	enum can_state new_state, tx_state, rx_state;
	u32 psr;
	u8 tec = rec[GCAN_ST_ECR], rx = rec[GCAN_ST_ECR + 1] & 0x7f;

	if (rec[1] >= GCAN_MAX_CHAN || !d->chan[rec[1]])
		return;
	c = d->chan[rec[1]];
	netdev = c->netdev;
	if (!netif_running(netdev))
		return;

	c->tec = tec;
	c->rec = rx;
	psr = get_unaligned_le32(&rec[GCAN_ST_PSR]);
	tx_state = gcan_counter_state(tec);
	rx_state = gcan_counter_state(rx);
	if (psr & GCAN_PSR_BO)
		tx_state = rx_state = CAN_STATE_BUS_OFF;
	else if ((psr & GCAN_PSR_EP) && max(tx_state, rx_state) < CAN_STATE_ERROR_PASSIVE)
		tx_state = CAN_STATE_ERROR_PASSIVE;	/* flag set but counters disagree: trust the flag */
	else if ((psr & GCAN_PSR_EW) && max(tx_state, rx_state) < CAN_STATE_ERROR_WARNING)
		tx_state = CAN_STATE_ERROR_WARNING;
	new_state = max(tx_state, rx_state);

	if (new_state == c->can.state || c->can.state == CAN_STATE_BUS_OFF)
		return;		/* once bus-off, only a restart brings the channel back */

	skb = alloc_can_err_skb(netdev, &cf);
	can_change_state(netdev, skb ? cf : NULL, tx_state, rx_state);
	if (skb) {
		cf->data[6] = tec;
		cf->data[7] = rx;
		netdev->stats.rx_packets++;
		netdev->stats.rx_bytes += cf->len;
		netif_rx(skb);
	}
	if (new_state == CAN_STATE_BUS_OFF)
		can_bus_off(netdev);
}

static void gcan_parse_block(struct gcan_dev *d, const u8 *buf, unsigned int len)
{
	unsigned int i = 0;

	while (i + 2 <= len) {
		unsigned int n = buf[i];

		if (n == 0 || i + n > len)
			break;
		if (n == GCAN_REC_STATUS_LEN)
			gcan_status_record(d, buf + i);
		else if (n >= GCAN_REC_HDR_LEN)
			gcan_rx_record(d, buf + i, n);
		i += n;
	}
}

static void gcan_rx_complete(struct urb *urb)
{
	struct gcan_dev *d = urb->context;
	int err;

	switch (urb->status) {
	case 0:
		gcan_parse_block(d, urb->transfer_buffer, urb->actual_length);
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
	case -EPIPE:
		return;
	default:
		dev_dbg_ratelimited(&d->intf->dev, "rx urb status %d\n", urb->status);
		break;
	}

	usb_anchor_urb(urb, &d->rx_anchor);
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err) {
		usb_unanchor_urb(urb);
		if (err != -ENODEV)
			dev_err(&d->intf->dev, "rx resubmit failed: %d\n", err);
	}
}

/* Acks for transmit writes (aa 55 <n> 00). They must be consumed or the device may stall. */
static void gcan_ack_complete(struct urb *urb)
{
	struct gcan_dev *d = urb->context;
	int err;

	switch (urb->status) {
	case 0:
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
	case -EPIPE:
		return;
	default:
		dev_dbg_ratelimited(&d->intf->dev, "ack urb status %d\n", urb->status);
		break;
	}

	usb_anchor_urb(urb, &d->rx_anchor);
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err)
		usb_unanchor_urb(urb);
}

static int gcan_start_rx(struct gcan_dev *d)
{
	int i, err;

	for (i = 0; i < GCAN_NUM_RX_URBS; i++) {
		usb_fill_bulk_urb(d->rx_urb[i], d->udev, usb_rcvbulkpipe(d->udev, GCAN_EP_RX_IN & 0x0f),
				  d->rx_buf[i], GCAN_RX_BUF_SIZE, gcan_rx_complete, d);
		usb_anchor_urb(d->rx_urb[i], &d->rx_anchor);
		err = usb_submit_urb(d->rx_urb[i], GFP_KERNEL);
		if (err) {
			usb_unanchor_urb(d->rx_urb[i]);
			goto fail;
		}
	}
	usb_fill_bulk_urb(d->ack_urb, d->udev, usb_rcvbulkpipe(d->udev, GCAN_EP_ACK_IN & 0x0f),
			  d->ack_buf, GCAN_ACK_BUF_SIZE, gcan_ack_complete, d);
	usb_anchor_urb(d->ack_urb, &d->rx_anchor);
	err = usb_submit_urb(d->ack_urb, GFP_KERNEL);
	if (err) {
		usb_unanchor_urb(d->ack_urb);
		goto fail;
	}
	return 0;
fail:
	usb_kill_anchored_urbs(&d->rx_anchor);
	return err;
}

/* ---------------------------------------------------------------- transmit (EP 0x01) */

static void gcan_tx_complete(struct urb *urb);

/* Give back the echo skbs of records that never reached the wire. Caller holds tx_lock. */
static void gcan_tx_drop_ents(struct gcan_dev *d, const struct gcan_tx_ent *ent, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		struct gcan_chan *c = d->chan[ent[i].ch];

		can_free_echo_skb(c->netdev, ent[i].idx, NULL);
		c->netdev->stats.tx_dropped++;
		c->tx_cnt--;
	}
}

/* Send the block being filled if it holds frames and a USB slot is free. Caller holds tx_lock. */
static void gcan_tx_kick(struct gcan_dev *d)
{
	struct gcan_tx_blk *blk = d->tx_cur, *next = NULL;
	int i, err;

	if (!blk->n || d->tx_inflight >= GCAN_TX_INFLIGHT)
		return;
	for (i = 0; i < GCAN_NUM_TX_BLOCKS; i++) {
		if (&d->tx_blk[i] != blk && !d->tx_blk[i].busy) {
			next = &d->tx_blk[i];
			break;
		}
	}
	if (!next)
		return;

	memset(blk->buf + blk->len, 0, GCAN_TX_BLOCK_SIZE - blk->len);	/* the device expects zero padding */
	blk->busy = true;
	usb_fill_bulk_urb(blk->urb, d->udev, usb_sndbulkpipe(d->udev, GCAN_EP_TX_OUT & 0x0f),
			  blk->buf, GCAN_TX_BLOCK_SIZE, gcan_tx_complete, blk);
	usb_anchor_urb(blk->urb, &d->tx_anchor);
	err = usb_submit_urb(blk->urb, GFP_ATOMIC);
	if (err) {
		usb_unanchor_urb(blk->urb);
		blk->busy = false;
		gcan_tx_drop_ents(d, blk->ent, blk->n);
		blk->n = blk->len = 0;
		if (err != -ENODEV)
			dev_warn_ratelimited(&d->intf->dev, "tx submit failed: %d\n", err);
		return;
	}
	d->tx_inflight++;
	d->tx_cur = next;
}

static void gcan_tx_complete(struct urb *urb)
{
	struct gcan_tx_blk *blk = urb->context;
	struct gcan_dev *d = blk->dev;
	struct gcan_tx_ent ent[GCAN_TX_MAX_RECS];
	unsigned int done[GCAN_MAX_CHAN] = { 0 };
	unsigned int n, i;
	unsigned long flags;
	int status = urb->status;

	spin_lock_irqsave(&d->tx_lock, flags);
	n = blk->n;
	memcpy(ent, blk->ent, n * sizeof(ent[0]));
	blk->n = blk->len = 0;
	blk->busy = false;
	d->tx_inflight--;
	gcan_tx_kick(d);	/* frames that piled up meanwhile go out together */
	spin_unlock_irqrestore(&d->tx_lock, flags);

	for (i = 0; i < n; i++) {
		struct net_device *nd = d->chan[ent[i].ch]->netdev;

		if (!status) {
			nd->stats.tx_packets++;
			nd->stats.tx_bytes += can_get_echo_skb(nd, ent[i].idx, NULL);
		} else {
			can_free_echo_skb(nd, ent[i].idx, NULL);
			if (status != -ENOENT && status != -ECONNRESET && status != -ESHUTDOWN)
				nd->stats.tx_errors++;
		}
		done[ent[i].ch]++;
	}

	for (i = 0; i < GCAN_MAX_CHAN; i++) {
		struct gcan_chan *c = d->chan[i];
		bool full;

		if (!c)
			continue;
		spin_lock_irqsave(&d->tx_lock, flags);
		c->tx_cnt -= done[i];	/* only after the echo slots were released */
		full = c->tx_cnt >= GCAN_TX_ECHO_SLOTS;
		spin_unlock_irqrestore(&d->tx_lock, flags);
		/* a channel may have stopped its queue because no USB slot was free, whoever's block just finished */
		if (!full && netif_running(c->netdev) && netif_queue_stopped(c->netdev) &&
		    c->can.state != CAN_STATE_BUS_OFF)
			netif_wake_queue(c->netdev);
	}
}

static netdev_tx_t gcan_start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct gcan_chan *c = netdev_priv(netdev);
	struct gcan_dev *d = c->dev;
	struct canfd_frame *cf = (struct canfd_frame *)skb->data;
	struct gcan_tx_blk *blk;
	u8 flags = 0, dlen, *rec;
	unsigned long irqflags;
	unsigned int idx;
	int err;

	if (can_dropped_invalid_skb(netdev, skb))
		return NETDEV_TX_OK;

	if (can_is_canfd_skb(skb)) {
		flags |= GCAN_FLAG_FD;
		if (cf->flags & CANFD_BRS)
			flags |= GCAN_FLAG_BRS;
		dlen = cf->len;
	} else {
		struct can_frame *ccf = (struct can_frame *)skb->data;

		if (ccf->can_id & CAN_RTR_FLAG) {
			flags |= GCAN_FLAG_RTR;
			dlen = 0;	/* RTR layout was not captured: send no data bytes */
		} else {
			dlen = ccf->len;
		}
	}
	if (cf->can_id & CAN_EFF_FLAG)
		flags |= GCAN_FLAG_EXT;

	spin_lock_irqsave(&d->tx_lock, irqflags);
	blk = d->tx_cur;
	if (blk->len + GCAN_REC_HDR_LEN + dlen > GCAN_TX_BLOCK_SIZE || blk->n >= GCAN_TX_MAX_RECS) {
		gcan_tx_kick(d);	/* block is full: send it and start the next one */
		blk = d->tx_cur;
		if (blk->n) {		/* no USB slot free yet; a completion wakes the queue */
			netif_stop_queue(netdev);
			spin_unlock_irqrestore(&d->tx_lock, irqflags);
			return NETDEV_TX_BUSY;
		}
	}

	idx = c->tx_head;
	err = can_put_echo_skb(skb, netdev, idx, 0);	/* consumes the skb, also on error */
	if (err) {
		spin_unlock_irqrestore(&d->tx_lock, irqflags);
		netdev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	c->tx_head = (idx + 1) % GCAN_TX_ECHO_SLOTS;

	rec = blk->buf + blk->len;
	memset(rec, 0, GCAN_REC_HDR_LEN + dlen);
	rec[0] = GCAN_REC_HDR_LEN + dlen;
	rec[1] = c->ch;
	/* bytes 2..9: timestamp, left zero */
	rec[10] = flags;
	rec[11] = dlen;
	put_unaligned_le32(cf->can_id & ((cf->can_id & CAN_EFF_FLAG) ? CAN_EFF_MASK : CAN_SFF_MASK), &rec[12]);
	if (dlen)
		memcpy(&rec[GCAN_REC_HDR_LEN], cf->data, dlen);	/* data starts at the same offset in both frame types */
	blk->ent[blk->n].ch = c->ch;
	blk->ent[blk->n].idx = idx;
	blk->n++;
	blk->len += GCAN_REC_HDR_LEN + dlen;

	if (++c->tx_cnt >= GCAN_TX_ECHO_SLOTS)
		netif_stop_queue(netdev);
	gcan_tx_kick(d);
	spin_unlock_irqrestore(&d->tx_lock, irqflags);
	return NETDEV_TX_OK;
}

/* Drop this channel's frames that still wait in the open block, then let the blocks on the wire finish
 * (or kill them if the device stalls, e.g. unacknowledged frames). */
static void gcan_tx_flush_chan(struct gcan_chan *c)
{
	struct gcan_dev *d = c->dev;
	struct gcan_tx_blk *blk;
	unsigned int i, pos = 0, off = 0, keep = 0;
	unsigned long flags;

	spin_lock_irqsave(&d->tx_lock, flags);
	blk = d->tx_cur;
	for (i = 0; i < blk->n; i++) {
		unsigned int rl = blk->buf[pos];

		if (blk->ent[i].ch == c->ch) {
			gcan_tx_drop_ents(d, &blk->ent[i], 1);
		} else {
			memmove(blk->buf + off, blk->buf + pos, rl);
			blk->ent[keep++] = blk->ent[i];
			off += rl;
		}
		pos += rl;
	}
	blk->n = keep;
	blk->len = off;
	spin_unlock_irqrestore(&d->tx_lock, flags);

	/* tx_head / tx_cnt are deliberately not reset: a completion that is still running subtracts from tx_cnt,
	 * and close_candev() frees the echo slots. Every queued frame is released by a completion or dropped above. */
	if (!usb_wait_anchor_empty_timeout(&d->tx_anchor, 200))
		usb_kill_anchored_urbs(&d->tx_anchor);
}

/* ---------------------------------------------------------------- netdev open / stop */

static int gcan_open(struct net_device *netdev)
{
	struct gcan_chan *c = netdev_priv(netdev);
	struct gcan_dev *d = c->dev;
	int err;

	err = open_candev(netdev);
	if (err)
		return err;

	mutex_lock(&d->open_lock);

	err = gcan_cmd_init(c);
	if (err)
		goto fail;
	err = gcan_cmd_simple(c, 0x02);		/* start */
	if (err)
		goto fail;

	if (d->open_count == 0) {
		err = gcan_start_rx(d);
		if (err) {
			gcan_cmd_simple(c, 0x03);
			gcan_cmd_simple(c, 0x01);
			goto fail;
		}
	}
	d->open_count++;
	c->can.state = CAN_STATE_ERROR_ACTIVE;
	netif_start_queue(netdev);
	mutex_unlock(&d->open_lock);
	return 0;

fail:
	mutex_unlock(&d->open_lock);
	close_candev(netdev);
	return err;
}

static int gcan_stop(struct net_device *netdev)
{
	struct gcan_chan *c = netdev_priv(netdev);
	struct gcan_dev *d = c->dev;

	netif_stop_queue(netdev);
	mutex_lock(&d->open_lock);

	gcan_cmd_simple(c, 0x03);		/* stop */
	gcan_cmd_simple(c, 0x01);		/* reset */

	gcan_tx_flush_chan(c);
	if (d->open_count > 0 && --d->open_count == 0)
		usb_kill_anchored_urbs(&d->rx_anchor);

	c->can.state = CAN_STATE_STOPPED;
	mutex_unlock(&d->open_lock);
	close_candev(netdev);
	return 0;
}

static int gcan_get_berr_counter(const struct net_device *netdev, struct can_berr_counter *bec)
{
	const struct gcan_chan *c = netdev_priv(netdev);

	bec->txerr = c->tec;
	bec->rxerr = c->rec;
	return 0;
}

/* "ip link set canX type can restart" (or restart-ms) after bus-off: run the stop/reset/init/start sequence again */
static int gcan_set_mode(struct net_device *netdev, enum can_mode mode)
{
	struct gcan_chan *c = netdev_priv(netdev);
	struct gcan_dev *d = c->dev;
	int err;

	if (mode != CAN_MODE_START)
		return -EOPNOTSUPP;

	mutex_lock(&d->open_lock);
	gcan_tx_flush_chan(c);
	gcan_cmd_simple(c, 0x03);
	gcan_cmd_simple(c, 0x01);
	err = gcan_cmd_init(c);
	if (!err)
		err = gcan_cmd_simple(c, 0x02);
	mutex_unlock(&d->open_lock);
	if (err)
		return err;

	c->tec = c->rec = 0;
	c->can.state = CAN_STATE_ERROR_ACTIVE;
	netif_wake_queue(netdev);
	return 0;
}

static const struct net_device_ops gcan_netdev_ops = {
	.ndo_open	= gcan_open,
	.ndo_stop	= gcan_stop,
	.ndo_start_xmit	= gcan_start_xmit,
	.ndo_change_mtu	= can_change_mtu,
};

/* ---------------------------------------------------------------- probe / disconnect */

static void gcan_free_chan(struct gcan_chan *c)
{
	free_candev(c->netdev);
}

static struct gcan_chan *gcan_create_chan(struct gcan_dev *d, u8 ch)
{
	struct net_device *netdev;
	struct gcan_chan *c;

	netdev = alloc_candev(sizeof(*c), GCAN_TX_ECHO_SLOTS);
	if (!netdev)
		return NULL;
	c = netdev_priv(netdev);
	c->netdev = netdev;
	c->dev = d;
	c->ch = ch;

	c->can.bitrate_const = gcan_nominal_rates;
	c->can.bitrate_const_cnt = ARRAY_SIZE(gcan_nominal_rates);
	c->can.data_bitrate_const = gcan_data_rates;
	c->can.data_bitrate_const_cnt = ARRAY_SIZE(gcan_data_rates);
	c->can.ctrlmode_supported = CAN_CTRLMODE_FD;
	c->can.do_get_berr_counter = gcan_get_berr_counter;
	c->can.do_set_mode = gcan_set_mode;

	netdev->netdev_ops = &gcan_netdev_ops;
	netdev->flags |= IFF_ECHO;
	netdev->dev_port = ch;
	SET_NETDEV_DEV(netdev, &d->intf->dev);
	return c;
}

static int gcan_check_endpoints(struct usb_interface *intf)
{
	static const u8 need[] = { GCAN_EP_TX_OUT, GCAN_EP_RX_IN, GCAN_EP_CMD_OUT, GCAN_EP_CMD_IN, GCAN_EP_ACK_IN };
	struct usb_host_interface *alt = intf->cur_altsetting;
	int i, j;

	for (i = 0; i < ARRAY_SIZE(need); i++) {
		bool found = false;

		for (j = 0; j < alt->desc.bNumEndpoints; j++) {
			struct usb_endpoint_descriptor *ep = &alt->endpoint[j].desc;

			if (ep->bEndpointAddress == need[i] && usb_endpoint_xfer_bulk(ep))
				found = true;
		}
		if (!found)
			return -ENODEV;
	}
	return 0;
}

static void gcan_free_dev(struct gcan_dev *d)
{
	int i;

	for (i = 0; i < GCAN_NUM_RX_URBS; i++) {
		usb_free_urb(d->rx_urb[i]);
		kfree(d->rx_buf[i]);
	}
	for (i = 0; i < GCAN_NUM_TX_BLOCKS; i++) {
		usb_free_urb(d->tx_blk[i].urb);
		kfree(d->tx_blk[i].buf);
	}
	usb_free_urb(d->ack_urb);
	kfree(d->ack_buf);
	kfree(d->cmd_buf);
	usb_put_dev(d->udev);
	kfree(d);
}

static int gcan_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct gcan_dev *d;
	int i, err;

	err = gcan_check_endpoints(intf);
	if (err) {
		dev_err(&intf->dev, "unexpected endpoint layout\n");
		return err;
	}

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->udev = usb_get_dev(interface_to_usbdev(intf));
	d->intf = intf;
	mutex_init(&d->cmd_lock);
	mutex_init(&d->open_lock);
	init_usb_anchor(&d->rx_anchor);
	init_usb_anchor(&d->tx_anchor);
	spin_lock_init(&d->tx_lock);

	d->cmd_buf = kmalloc(GCAN_CMD_BUF_SIZE, GFP_KERNEL);
	d->ack_buf = kmalloc(GCAN_ACK_BUF_SIZE, GFP_KERNEL);
	d->ack_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!d->cmd_buf || !d->ack_buf || !d->ack_urb) {
		err = -ENOMEM;
		goto fail_dev;
	}
	for (i = 0; i < GCAN_NUM_RX_URBS; i++) {
		d->rx_buf[i] = kmalloc(GCAN_RX_BUF_SIZE, GFP_KERNEL);
		d->rx_urb[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!d->rx_buf[i] || !d->rx_urb[i]) {
			err = -ENOMEM;
			goto fail_dev;
		}
	}
	for (i = 0; i < GCAN_NUM_TX_BLOCKS; i++) {
		d->tx_blk[i].dev = d;
		d->tx_blk[i].urb = usb_alloc_urb(0, GFP_KERNEL);
		d->tx_blk[i].buf = kzalloc(GCAN_TX_BLOCK_SIZE, GFP_KERNEL);
		if (!d->tx_blk[i].urb || !d->tx_blk[i].buf) {
			err = -ENOMEM;
			goto fail_dev;
		}
	}
	d->tx_cur = &d->tx_blk[0];
	usb_set_intfdata(intf, d);

	err = gcan_cmd_time_sync(d);
	if (err) {
		dev_err(&intf->dev, "time sync failed, device not responding: %d\n", err);
		goto fail_dev;
	}

	for (i = 0; i < GCAN_MAX_CHAN; i++) {
		d->chan[i] = gcan_create_chan(d, i);
		if (!d->chan[i]) {
			err = -ENOMEM;
			goto fail_chan;
		}
		err = register_candev(d->chan[i]->netdev);
		if (err) {
			dev_err(&intf->dev, "register_candev ch%d failed: %d\n", i, err);
			gcan_free_chan(d->chan[i]);
			d->chan[i] = NULL;
			goto fail_chan;
		}
		d->chan[i]->registered = true;
	}
	dev_info(&intf->dev, "GCAN USBCANFD ready: %s, %s\n",
		 d->chan[0]->netdev->name, d->chan[1]->netdev->name);
	return 0;

fail_chan:
	for (i = 0; i < GCAN_MAX_CHAN; i++) {
		if (d->chan[i]) {
			unregister_candev(d->chan[i]->netdev);
			gcan_free_chan(d->chan[i]);
		}
	}
fail_dev:
	usb_set_intfdata(intf, NULL);
	gcan_free_dev(d);
	return err;
}

static void gcan_disconnect(struct usb_interface *intf)
{
	struct gcan_dev *d = usb_get_intfdata(intf);
	int i;

	if (!d)
		return;
	usb_set_intfdata(intf, NULL);
	d->disconnected = true;
	usb_kill_anchored_urbs(&d->rx_anchor);
	for (i = 0; i < GCAN_MAX_CHAN; i++) {
		if (!d->chan[i])
			continue;
		unregister_candev(d->chan[i]->netdev);	/* closes the interface if it is up */
	}
	usb_kill_anchored_urbs(&d->tx_anchor);
	for (i = 0; i < GCAN_MAX_CHAN; i++) {
		if (d->chan[i])
			gcan_free_chan(d->chan[i]);
	}
	gcan_free_dev(d);
}

static const struct usb_device_id gcan_ids[] = {
	{ USB_DEVICE(GCAN_VID, GCAN_PID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, gcan_ids);

static struct usb_driver gcan_driver = {
	.name		= "gcan_usbcanfd",
	.probe		= gcan_probe,
	.disconnect	= gcan_disconnect,
	.id_table	= gcan_ids,
};
module_usb_driver(gcan_driver);

MODULE_DESCRIPTION("SocketCAN driver for the GCAN USBCANFD adapter (0c66:000e)");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

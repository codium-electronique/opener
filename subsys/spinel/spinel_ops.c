/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Driver operation broker: schedules the operations the host queues with
 * DRV_OP_QUEUE and forwards the driver events to the host as DRV_EVENT.
 *
 * Each operation owns a slot from its insertion until its OP_FINISHED event
 * has been sent to the host. OP_FINISHED events use their own pool, with one
 * buffer per slot, so they are never dropped and slots cannot leak. The other
 * events share the event pool: when it is exhausted they are dropped, counted,
 * and reported in the "dropped" field of the next event sent.
 */

#include <errno.h>
#include <stdarg.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dectnrp.h>
#include <zephyr/net/dectnrp_driver.h>
#include <zephyr/net/dectnrp_spinel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net_buf.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include "spinel_internal.h"

LOG_MODULE_DECLARE(dectnrp_spinel, CONFIG_DECTNRP_SPINEL_LOG_LEVEL);

/* An OP_FINISHED frame: header, command, DRV_EVENT and the event prefix. */
#define FINISHED_FRAME_SIZE 16

/* Op flags allowed for each op type. */
#define TX_FLAGS                                                                                   \
	(DECTNRP_SPINEL_OP_FLAG_SCHEDULED | DECTNRP_SPINEL_OP_FLAG_LBT |                           \
	 DECTNRP_SPINEL_OP_FLAG_HARQ | DECTNRP_SPINEL_OP_FLAG_BEACON)
#define RX_FLAGS                                                                                   \
	(DECTNRP_SPINEL_OP_FLAG_SCHEDULED | DECTNRP_SPINEL_OP_FLAG_BEACON |                        \
	 DECTNRP_SPINEL_OP_FLAG_RX_RECEPTION_MASK)
#define RSSI1_FLAGS DECTNRP_SPINEL_OP_FLAG_SCHEDULED

/* One driver operation queued by the host. */
struct spinel_op_slot {
	/* From a successful insert until the release after OP_FINISHED. */
	bool used;
	/* OP_FINISHED received: later events of the op are ignored. */
	bool finished;
	/* Token chosen by the host. */
	uint16_t token;
	/* Incremented at each release, so a stale OP_FINISHED buffer cannot
	 * release the next operation of the slot.
	 */
	uint32_t generation;
	struct dectnrp_driver_op op;
	/* RSSI1 result storage, for RSSI1 operations. */
	struct dectnrp_rssi1_result rssi1;
};

/* User data of an OP_FINISHED buffer: the slot to release once it is sent. */
struct finished_ref {
	uint32_t generation;
	uint8_t index;
};

/* A DRV_OP_QUEUE value, parsed. */
struct op_request {
	uint16_t token;
	struct dectnrp_driver_op op;
	/* TX data (PCC then PDC), pointing into the request frame. */
	const uint8_t *tx_data;
	unsigned int tx_data_len;
};

static void finished_destroy(struct net_buf *buf);

NET_BUF_POOL_DEFINE(spinel_evt_pool, CONFIG_DECTNRP_SPINEL_EVENT_BUFS,
		    CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE, 0, NULL);
NET_BUF_POOL_DEFINE(spinel_finished_pool, CONFIG_DECTNRP_SPINEL_OP_SLOTS, FINISHED_FRAME_SIZE,
		    sizeof(struct finished_ref), finished_destroy);

static struct spinel_op_slot slots[CONFIG_DECTNRP_SPINEL_OP_SLOTS];
static struct k_spinlock slots_lock;
static K_SEM_DEFINE(slot_released, 0, K_SEM_MAX_LIMIT);

/* Events dropped since the last event sent to the host. */
static atomic_t pending_drops;

/*
 * Slots
 */

size_t spinel_ops_in_flight(void)
{
	k_spinlock_key_t key;
	size_t count = 0;

	key = k_spin_lock(&slots_lock);
	for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
		if (slots[i].used) {
			count++;
		}
	}
	k_spin_unlock(&slots_lock, key);

	return count;
}

/* Reserve a slot for @p token; return a Spinel status. */
static uint32_t slot_alloc(uint16_t token, struct spinel_op_slot **slot_out)
{
	struct spinel_op_slot *free_slot = NULL;
	bool duplicate = false;
	k_spinlock_key_t key;
	uint32_t status;

	key = k_spin_lock(&slots_lock);

	for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
		struct spinel_op_slot *slot = &slots[i];

		if (slot->used && slot->token == token) {
			duplicate = true;
		}
		if (!slot->used && free_slot == NULL) {
			free_slot = slot;
		}
	}

	if (duplicate) {
		status = SPINEL_STATUS_ALREADY;
	} else if (free_slot == NULL) {
		status = SPINEL_STATUS_NOMEM;
	} else {
		free_slot->used = true;
		free_slot->finished = false;
		free_slot->token = token;
		*slot_out = free_slot;
		status = SPINEL_STATUS_OK;
	}

	k_spin_unlock(&slots_lock, key);

	return status;
}

/* Release @p slot, unless it was already released since @p generation. */
static void slot_release(struct spinel_op_slot *slot, uint32_t generation)
{
	struct net_pkt *pkt = NULL;
	bool released = false;
	k_spinlock_key_t key;

	key = k_spin_lock(&slots_lock);
	if (slot->used && slot->generation == generation) {
		if (slot->op.type == DECTNRP_DRIVER_OP_TX) {
			pkt = slot->op.tx.pkt;
			slot->op.tx.pkt = NULL;
		}
		slot->used = false;
		slot->generation++;
		released = true;
	}
	k_spin_unlock(&slots_lock, key);

	if (pkt != NULL) {
		net_pkt_unref(pkt);
	}
	if (released) {
		k_sem_give(&slot_released);
	}
}

/* Find the slot of @p op; NULL if @p op does not belong to a slot. */
static struct spinel_op_slot *slot_of_op(const struct dectnrp_driver_op *op)
{
	for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
		if (&slots[i].op == op) {
			return &slots[i];
		}
	}

	return NULL;
}

/*
 * Get the token of the op in flight in @p slot. With @p finish, also mark the
 * op finished. Return false if no op is in flight in the slot.
 */
static bool slot_get_token(struct spinel_op_slot *slot, bool finish, uint16_t *token,
			   uint32_t *generation)
{
	k_spinlock_key_t key;
	bool in_flight;

	key = k_spin_lock(&slots_lock);
	in_flight = slot->used && !slot->finished;
	if (in_flight) {
		*token = slot->token;
		*generation = slot->generation;
		if (finish) {
			slot->finished = true;
		}
	}
	k_spin_unlock(&slots_lock, key);

	return in_flight;
}

static void finished_destroy(struct net_buf *buf)
{
	struct finished_ref *ref = net_buf_user_data(buf);
	struct spinel_op_slot *slot = &slots[ref->index];
	uint32_t generation = ref->generation;

	net_buf_destroy(buf);

	/* OP_FINISHED was sent to the host (or dropped): the slot is free. */
	slot_release(slot, generation);
}

/*
 * DRV_OP_QUEUE
 */

static uint32_t parse_tx(const uint8_t *value, size_t len, uint8_t flags, struct op_request *req)
{
	struct dectnrp_transport_parameters *params = &req->op.tx.transport_parameters;
	uint8_t phy_header_type;
	size_t pcc_size;
	spinel_ssize_t n;

	if ((flags & ~TX_FLAGS) != 0) {
		return SPINEL_STATUS_INVALID_ARGUMENT;
	}

	n = spinel_datatype_unpack(
		value, len, DECTNRP_SPINEL_FMT_OP_TX, &phy_header_type, &params->tbs, &params->mu,
		&params->beta, &params->mcs, &params->slots, &req->op.tx.lbt.period,
		&req->op.tx.lbt.rssi_threshold, &req->tx_data, &req->tx_data_len);
	if (n < 0) {
		return SPINEL_STATUS_PARSE_ERROR;
	}

	/* Physical Header Field: type 1 is 40 bits, type 2 is 80 bits
	 * (ETSI TS 103 636-4 V2.2.1 clause 6.2).
	 */
	if (phy_header_type == 0) {
		pcc_size = DECTNRP_PHY_HEADER_TYPE1_SIZE;
	} else if (phy_header_type == 1) {
		pcc_size = DECTNRP_PHY_HEADER_TYPE2_SIZE;
	} else {
		return SPINEL_STATUS_INVALID_ARGUMENT;
	}

	/* The data is the physical header, then at most an MTU of payload. */
	if (req->tx_data_len < pcc_size) {
		return SPINEL_STATUS_INVALID_ARGUMENT;
	}
	if (req->tx_data_len > pcc_size + DECTNRP_MTU) {
		return SPINEL_STATUS_INVALID_ARGUMENT;
	}

	req->op.type = DECTNRP_DRIVER_OP_TX;
	req->op.tx.phy_header_type = phy_header_type;
	req->op.tx.mode.is_scheduled = (flags & DECTNRP_SPINEL_OP_FLAG_SCHEDULED) != 0;
	req->op.tx.mode.is_lbt = (flags & DECTNRP_SPINEL_OP_FLAG_LBT) != 0;
	req->op.tx.mode.is_harq = (flags & DECTNRP_SPINEL_OP_FLAG_HARQ) != 0;
	req->op.tx.mode.is_beacon = (flags & DECTNRP_SPINEL_OP_FLAG_BEACON) != 0;

	return SPINEL_STATUS_OK;
}

static uint32_t parse_rx(const uint8_t *value, size_t len, uint8_t flags, struct op_request *req)
{
	uint8_t reception;
	uint32_t duration;
	spinel_ssize_t n;

	if ((flags & ~RX_FLAGS) != 0) {
		return SPINEL_STATUS_INVALID_ARGUMENT;
	}

	reception = (flags & DECTNRP_SPINEL_OP_FLAG_RX_RECEPTION_MASK) >>
		    DECTNRP_SPINEL_OP_FLAG_RX_RECEPTION_SHIFT;
	if (reception > DECTNRP_RX_RECEPTION_CONTINUOUS) {
		return SPINEL_STATUS_INVALID_ARGUMENT;
	}

	n = spinel_datatype_unpack(value, len, DECTNRP_SPINEL_FMT_OP_RX, &duration,
				   &req->op.rx.expected_rssi);
	if (n < 0) {
		return SPINEL_STATUS_PARSE_ERROR;
	}

	req->op.type = DECTNRP_DRIVER_OP_RX;
	req->op.rx.duration = duration;
	req->op.rx.mode.is_scheduled = (flags & DECTNRP_SPINEL_OP_FLAG_SCHEDULED) != 0;
	req->op.rx.mode.is_beacon = (flags & DECTNRP_SPINEL_OP_FLAG_BEACON) != 0;
	req->op.rx.mode.reception = reception;

	return SPINEL_STATUS_OK;
}

static uint32_t parse_rssi1(const uint8_t *value, size_t len, uint8_t flags, struct op_request *req)
{
	spinel_ssize_t n;

	if ((flags & ~RSSI1_FLAGS) != 0) {
		return SPINEL_STATUS_INVALID_ARGUMENT;
	}

	n = spinel_datatype_unpack(value, len, DECTNRP_SPINEL_FMT_OP_RSSI1,
				   &req->op.rssi1.subslots);
	if (n < 0) {
		return SPINEL_STATUS_PARSE_ERROR;
	}

	req->op.type = DECTNRP_DRIVER_OP_RSSI1;
	req->op.rssi1.mode.is_scheduled = (flags & DECTNRP_SPINEL_OP_FLAG_SCHEDULED) != 0;

	return SPINEL_STATUS_OK;
}

/* Parse a DRV_OP_QUEUE value into @p req; return a Spinel status. */
static uint32_t parse_op_request(const uint8_t *value, size_t len, struct op_request *req)
{
	uint8_t type;
	uint16_t channel;
	uint8_t flags;
	uint64_t start_time;
	uint32_t status;
	spinel_ssize_t n;

	memset(req, 0, sizeof(*req));

	n = spinel_datatype_unpack(value, len, DECTNRP_SPINEL_FMT_OP_PREFIX, &req->token, &type,
				   &channel, &flags, &start_time);
	if (n < 0) {
		return SPINEL_STATUS_PARSE_ERROR;
	}
	value += n;
	len -= n;

	switch (type) {
	case DECTNRP_SPINEL_OP_TX:
		status = parse_tx(value, len, flags, req);
		break;
	case DECTNRP_SPINEL_OP_RX:
		status = parse_rx(value, len, flags, req);
		break;
	case DECTNRP_SPINEL_OP_RSSI1:
		status = parse_rssi1(value, len, flags, req);
		break;
	default:
		status = SPINEL_STATUS_INVALID_ARGUMENT;
		break;
	}
	if (status != SPINEL_STATUS_OK) {
		return status;
	}

	req->op.channel = channel;

	/* The driver starts an op with start time 0 immediately. */
	if ((flags & DECTNRP_SPINEL_OP_FLAG_SCHEDULED) != 0) {
		req->op.start_time = start_time;
	} else {
		req->op.start_time = 0;
	}

	return SPINEL_STATUS_OK;
}

/* Copy the TX data into a packet for the driver; NULL if out of packets. */
/* Copy @p data into a new packet stored in @p out; return a Spinel status. */
static uint32_t alloc_tx_pkt(const uint8_t *data, size_t len, struct net_pkt **out)
{
	struct net_if *iface = spinel_iface();
	struct net_pkt *pkt;
	int err;

	pkt = net_pkt_alloc_with_buffer(iface, len, NET_AF_UNSPEC, 0, K_NO_WAIT);
	if (pkt == NULL) {
		return SPINEL_STATUS_NOMEM;
	}

	err = net_pkt_write(pkt, data, len);
	if (err != 0) {
		net_pkt_unref(pkt);
		return SPINEL_STATUS_NOMEM;
	}

	/* Drivers transmit the first fragment of the packet only. */
	if (pkt->buffer->frags != NULL) {
		LOG_WRN("TX data of %zu bytes does not fit in one net_buf "
			"(CONFIG_NET_BUF_DATA_SIZE)",
			len);
		net_pkt_unref(pkt);
		return SPINEL_STATUS_INVALID_ARGUMENT;
	}

	*out = pkt;
	return SPINEL_STATUS_OK;
}

/* Fill @p slot from @p req and schedule its op; return a Spinel status. */
static uint32_t slot_schedule(struct spinel_op_slot *slot, const struct op_request *req)
{
	struct net_if *iface = spinel_iface();
	const struct device *dev = net_if_get_device(iface);
	const struct dectnrp_driver_api *api = dev->api;
	/* Only the RX thread changes the generation of a slot in use. */
	uint32_t generation = slot->generation;
	struct net_pkt *pkt;
	uint32_t status;
	int err;

	slot->op = req->op;

	if (slot->op.type == DECTNRP_DRIVER_OP_TX) {
		status = alloc_tx_pkt(req->tx_data, req->tx_data_len, &pkt);
		if (status != SPINEL_STATUS_OK) {
			slot_release(slot, generation);
			return status;
		}
		slot->op.tx.pkt = pkt;
	}

	if (slot->op.type == DECTNRP_DRIVER_OP_RSSI1) {
		memset(&slot->rssi1, 0, sizeof(slot->rssi1));
		slot->rssi1.channel = slot->op.channel;
		slot->op.rssi1.result = &slot->rssi1;
	}

	err = api->schedule(dev, &slot->op);
	if (err != 0) {
		slot_release(slot, generation);
		return spinel_status_from_errno(err);
	}

	return SPINEL_STATUS_OK;
}

static void reject_op(uint8_t tid, uint32_t status)
{
	spinel_stat_inc(SPINEL_STAT_OPS_REJECTED);
	spinel_reply_status(tid, status);
}

void spinel_ops_insert(uint8_t tid, const uint8_t *value, size_t len)
{
	struct op_request req;
	struct spinel_op_slot *slot;
	uint32_t status;

	status = parse_op_request(value, len, &req);
	if (status != SPINEL_STATUS_OK) {
		reject_op(tid, status);
		return;
	}

	status = slot_alloc(req.token, &slot);
	if (status != SPINEL_STATUS_OK) {
		reject_op(tid, status);
		return;
	}

	status = slot_schedule(slot, &req);
	if (status != SPINEL_STATUS_OK) {
		reject_op(tid, status);
		return;
	}

	spinel_stat_inc(SPINEL_STAT_OPS_ACCEPTED);
	spinel_reply(tid, SPINEL_CMD_PROP_VALUE_INSERTED, DECTNRP_SPINEL_PROP_DRV_OP_QUEUE,
		     SPINEL_DATATYPE_UINT16_S, req.token);
}

int spinel_ops_reset(k_timeout_t timeout)
{
	k_timepoint_t end = sys_timepoint_calc(timeout);
	k_timeout_t remaining;
	size_t in_flight;
	k_spinlock_key_t key;
	bool used;
	uint32_t generation;
	int err;

	/* Each op releases its slot once its OP_FINISHED is sent. */
	in_flight = spinel_ops_in_flight();
	while (in_flight > 0) {
		remaining = sys_timepoint_timeout(end);
		err = k_sem_take(&slot_released, remaining);
		if (err != 0) {
			break;
		}
		in_flight = spinel_ops_in_flight();
	}

	if (in_flight == 0) {
		return 0;
	}

	LOG_WRN("%zu operations did not finish, freeing their slots", in_flight);
	for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
		key = k_spin_lock(&slots_lock);
		used = slots[i].used;
		generation = slots[i].generation;
		k_spin_unlock(&slots_lock, key);

		if (used) {
			slot_release(&slots[i], generation);
		}
	}

	return -ETIMEDOUT;
}

/*
 * DRV_EVENT
 */

/* Take the number of events dropped so far, for the next event sent. */
static uint16_t take_dropped(void)
{
	atomic_val_t dropped;

	dropped = atomic_set(&pending_drops, 0);
	if (dropped > UINT16_MAX) {
		return UINT16_MAX;
	}

	return dropped;
}

static void count_dropped(void)
{
	atomic_inc(&pending_drops);
	spinel_stat_inc(SPINEL_STAT_EVENTS_DROPPED);
}

/*
 * Pack a DRV_EVENT frame into @p buf: header with TID 0 (unsolicited), the
 * common prefix, then the values described by the Spinel format @p fmt.
 */
static int pack_event(struct net_buf *buf, uint16_t token, uint8_t code, int32_t status,
		      uint16_t dropped, const char *fmt, ...)
{
	uint8_t header = SPINEL_HEADER_FLAG;
	size_t room;
	spinel_ssize_t n;
	va_list ap;

	/* Spinel header, command, property and event prefix. */
	room = net_buf_tailroom(buf);
	n = spinel_datatype_pack(net_buf_tail(buf), room, "Cii" DECTNRP_SPINEL_FMT_EVT_PREFIX,
				 header, SPINEL_CMD_PROP_VALUE_IS, DECTNRP_SPINEL_PROP_DRV_EVENT,
				 token, code, status, dropped);
	if (n < 0 || n > room) {
		return -ENOMEM;
	}
	net_buf_add(buf, n);

	/* Event suffix. */
	room = net_buf_tailroom(buf);
	va_start(ap, fmt);
	n = spinel_datatype_vpack(net_buf_tail(buf), room, fmt, ap);
	va_end(ap);
	if (n < 0 || n > room) {
		return -ENOMEM;
	}
	net_buf_add(buf, n);

	return 0;
}

/* Queue a packed event for the host, or drop it if packing failed. */
static void queue_event(struct net_buf *buf, int pack_err, uint16_t dropped)
{
	if (pack_err != 0) {
		LOG_WRN("event does not fit in a frame, dropped");
		net_buf_unref(buf);
		/* Report the earlier drops with the next event instead. */
		atomic_add(&pending_drops, dropped);
		count_dropped();
		return;
	}

	spinel_send_frame(buf);
	spinel_stat_inc(SPINEL_STAT_EVENTS_SENT);
}

static void forward_msg_received(const struct dectnrp_driver_event *evt)
{
	struct spinel_op_slot *slot;
	uint16_t token;
	uint32_t generation;
	bool in_flight;
	uint8_t valid = 0;
	struct net_buf *buf;
	uint16_t dropped;
	int err;

	slot = slot_of_op(evt->msg_received.op);
	if (slot == NULL) {
		LOG_WRN("MSG_RECEIVED for an unknown op");
		return;
	}
	in_flight = slot_get_token(slot, false, &token, &generation);
	if (!in_flight) {
		LOG_WRN("MSG_RECEIVED for a finished op");
		return;
	}

	buf = net_buf_alloc(&spinel_evt_pool, K_NO_WAIT);
	if (buf == NULL) {
		count_dropped();
		return;
	}

	if (evt->msg_received.rssi2_valid) {
		valid |= DECTNRP_SPINEL_RX_VALID_RSSI2;
	}
	if (evt->msg_received.snr_valid) {
		valid |= DECTNRP_SPINEL_RX_VALID_SNR;
	}

	dropped = take_dropped();
	err = pack_event(buf, token, DECTNRP_SPINEL_EVENT_MSG_RECEIVED, evt->msg_received.status,
			 dropped, DECTNRP_SPINEL_FMT_EVT_MSG_RECEIVED,
			 /* phy_type, valid, start_time, rssi2, snr, channel */
			 evt->msg_received.phy_type, valid, (uint64_t)evt->msg_received.start_time,
			 evt->msg_received.rssi2, evt->msg_received.snr, slot->op.channel,
			 /* pcc, pdc */
			 evt->msg_received.pcc, (uint32_t)evt->msg_received.pcc_len,
			 evt->msg_received.pdc, evt->msg_received.pdc_len);
	queue_event(buf, err, dropped);
}

static void forward_msg_error(const struct dectnrp_driver_event *evt)
{
	struct spinel_op_slot *slot;
	uint16_t token;
	uint32_t generation;
	bool in_flight;
	uint8_t valid = 0;
	const uint8_t *pcc = NULL;
	uint32_t pcc_len = 0;
	struct net_buf *buf;
	uint16_t dropped;
	int err;

	slot = slot_of_op(evt->message_error.op);
	if (slot == NULL) {
		LOG_WRN("MSG_ERROR for an unknown op");
		return;
	}
	in_flight = slot_get_token(slot, false, &token, &generation);
	if (!in_flight) {
		LOG_WRN("MSG_ERROR for a finished op");
		return;
	}

	buf = net_buf_alloc(&spinel_evt_pool, K_NO_WAIT);
	if (buf == NULL) {
		count_dropped();
		return;
	}

	if (evt->message_error.rssi2_valid) {
		valid |= DECTNRP_SPINEL_RX_VALID_RSSI2;
	}
	if (evt->message_error.snr_valid) {
		valid |= DECTNRP_SPINEL_RX_VALID_SNR;
	}

	/* The event has no PCC length: it follows from the PHY header type. */
	if (evt->message_error.pcc_valid) {
		valid |= DECTNRP_SPINEL_RX_VALID_PCC;
		pcc = evt->message_error.pcc;
		if (evt->message_error.phy_type == 0) {
			pcc_len = DECTNRP_PHY_HEADER_TYPE1_SIZE;
		} else {
			pcc_len = DECTNRP_PHY_HEADER_TYPE2_SIZE;
		}
	}

	dropped = take_dropped();
	err = pack_event(buf, token, DECTNRP_SPINEL_EVENT_MSG_ERROR, evt->message_error.status,
			 dropped, DECTNRP_SPINEL_FMT_EVT_MSG_ERROR,
			 /* phy_type, valid, start_time, rssi2, snr, channel */
			 evt->message_error.phy_type, valid,
			 (uint64_t)evt->message_error.start_time, evt->message_error.rssi2,
			 evt->message_error.snr, slot->op.channel,
			 /* pcc */
			 pcc, pcc_len);
	queue_event(buf, err, dropped);
}

static void forward_op_results(const struct dectnrp_driver_event *evt)
{
	struct spinel_op_slot *slot;
	uint16_t token;
	uint32_t generation;
	bool in_flight;
	struct net_buf *buf;
	uint16_t dropped;
	int err;

	slot = slot_of_op(evt->rssi1_result.op);
	if (slot == NULL) {
		LOG_WRN("OP_RESULTS for an unknown op");
		return;
	}
	in_flight = slot_get_token(slot, false, &token, &generation);
	if (!in_flight) {
		LOG_WRN("OP_RESULTS for a finished op");
		return;
	}

	buf = net_buf_alloc(&spinel_evt_pool, K_NO_WAIT);
	if (buf == NULL) {
		count_dropped();
		return;
	}

	dropped = take_dropped();
	err = pack_event(buf, token, DECTNRP_SPINEL_EVENT_OP_RESULTS, evt->rssi1_result.status,
			 dropped, DECTNRP_SPINEL_FMT_EVT_OP_RESULTS, slot->op.channel,
			 slot->rssi1.subslot, (uint32_t)sizeof(slot->rssi1.subslot));
	queue_event(buf, err, dropped);
}

static void forward_op_finished(const struct dectnrp_driver_event *evt)
{
	struct spinel_op_slot *slot;
	uint16_t token;
	uint32_t generation;
	bool in_flight;
	struct finished_ref *ref;
	struct net_buf *buf;
	uint16_t dropped;
	int err;

	slot = slot_of_op(evt->op_finished.op);
	if (slot == NULL) {
		LOG_WRN("OP_FINISHED for an unknown op");
		return;
	}
	in_flight = slot_get_token(slot, true, &token, &generation);
	if (!in_flight) {
		LOG_WRN("OP_FINISHED for a finished op");
		return;
	}

	buf = net_buf_alloc(&spinel_finished_pool, K_NO_WAIT);
	if (buf == NULL) {
		/* Only after a reset freed slots whose OP_FINISHED is still queued. */
		LOG_ERR("no OP_FINISHED buffer for token %u", token);
		slot_release(slot, generation);
		return;
	}

	/* Freeing the buffer, once it is sent, releases the slot. */
	ref = net_buf_user_data(buf);
	ref->index = slot - slots;
	ref->generation = generation;

	dropped = take_dropped();
	err = pack_event(buf, token, DECTNRP_SPINEL_EVENT_OP_FINISHED, slot->op.status, dropped,
			 "");
	queue_event(buf, err, dropped);
}

void spinel_ops_event_cb(struct net_if *iface, const struct dectnrp_driver_event *evt)
{
	ARG_UNUSED(iface);

	/* Driver and Spinel event codes differ: map each one explicitly. */
	switch (evt->code) {
	case DECTNRP_EVENT_MSG_RECEIVED:
		forward_msg_received(evt);
		break;
	case DECTNRP_EVENT_MSG_ERROR:
		forward_msg_error(evt);
		break;
	case DECTNRP_EVENT_OP_RESULTS:
		forward_op_results(evt);
		break;
	case DECTNRP_EVENT_OP_FINISHED:
		forward_op_finished(evt);
		break;
	default:
		LOG_WRN("unknown driver event %d", evt->code);
		break;
	}
}

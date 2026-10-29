/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/dectnrp.h>
#include <zephyr/net/dectnrp_driver.h>
#include <zephyr/net/dectnrp_net_l2.h>
#include <zephyr/net/net_if.h>

#include "mock_driver.h"

/* Modem ticks per millisecond (69.12 MHz modem clock). */
#define MOCK_TICKS_PER_MS 69120

static struct mock_driver mock;
static struct net_if *mock_iface;

/* Operations already reported as finished, indexed like mock.ops. */
static bool op_finished[ARRAY_SIZE(mock.ops)];

struct mock_driver *mock_driver_get(void)
{
	return &mock;
}

void mock_driver_reset(void)
{
	dectnrp_driver_event_cb_t cb = mock.cb;

	memset(&mock, 0, sizeof(mock));
	memset(op_finished, 0, sizeof(op_finished));
	mock.cb = cb;
}

struct net_if *mock_driver_iface(void)
{
	return mock_iface;
}

void mock_driver_emit(const struct dectnrp_driver_event *evt)
{
	if (mock.cb != NULL) {
		mock.cb(mock_iface, evt);
	}
}

#ifdef CONFIG_DECTNRP_DRIVER_MOCK_AUTO

/*
 * Auto mode: scheduled operations run against a simulated radio. A canned
 * frame is on air every AUTO_EMIT_PERIOD_MS of uptime, and an RX window open
 * at that instant receives it.
 */

#define AUTO_EMIT_PERIOD_MS CONFIG_DECTNRP_DRIVER_MOCK_AUTO_EMIT_PERIOD_MS
/* Every AUTO_ERROR_PERIOD-th frame on air fails its PDC CRC. */
#define AUTO_ERROR_PERIOD   10
/* Duration of a TX operation. */
#define AUTO_TX_DURATION_MS 1
/* Measured RSSI1, RSSI2 and SNR. */
#define AUTO_RSSI1          0x60
#define AUTO_RSSI2          0x50
#define AUTO_SNR            20
/* phy_type of a type 2 physical header. */
#define AUTO_PHY_TYPE_2     1

/* The type 2 format 0 physical header the dectnrp-driver sample sends. */
static const uint8_t auto_pcc[] = {
	0x10,             /* Header format 0, length type slots, length 1 slot */
	0x78,             /* Short network ID */
	0xee, 0xff,       /* Transmitter short ID */
	0x71,             /* Transmit power 0 dBm, DF MCS 1 */
	0xff, 0xff,       /* Receiver short ID: broadcast */
	0x00, 0x00, 0x00, /* Feedback format 0: no feedback */
};

static const char auto_pdc[] = "Hello World!";
#define AUTO_PDC_LEN (sizeof(auto_pdc) - 1)

struct auto_op {
	struct k_work_delayable work;
	/* Operation being simulated, NULL when the entry is free. */
	struct dectnrp_driver_op *op;
	/* Uptime in ms when the operation starts and ends. */
	int64_t start_ms;
	int64_t end_ms;
	/* Uptime in ms of the next frame on air an RX window can receive. */
	int64_t next_frame_ms;
};

static struct auto_op auto_ops[ARRAY_SIZE(mock.ops)];

/*
 * Held while an entry is used. The work handler only acts on what the time
 * says is due, so a handler that runs late or early is harmless.
 */
static K_MUTEX_DEFINE(auto_lock);

static bool auto_is_scheduled(const struct dectnrp_driver_op *op)
{
	switch (op->type) {
	case DECTNRP_DRIVER_OP_TX:
		return op->tx.mode.is_scheduled;
	case DECTNRP_DRIVER_OP_RX:
		return op->rx.mode.is_scheduled;
	case DECTNRP_DRIVER_OP_RSSI1:
		return op->rssi1.mode.is_scheduled;
	default:
		return false;
	}
}

static int64_t auto_duration_ms(const struct dectnrp_driver_op *op)
{
	switch (op->type) {
	case DECTNRP_DRIVER_OP_TX:
		return AUTO_TX_DURATION_MS;
	case DECTNRP_DRIVER_OP_RX:
		return op->rx.duration / MOCK_TICKS_PER_MS;
	default:
		return 0;
	}
}

/* @return the uptime in ms of the first frame on air at or after @p ms. */
static int64_t auto_next_frame(int64_t ms)
{
	int64_t periods;

	periods = (ms + AUTO_EMIT_PERIOD_MS - 1) / AUTO_EMIT_PERIOD_MS;

	return periods * AUTO_EMIT_PERIOD_MS;
}

/* Free @p a and report its operation finished. */
static void auto_finish(struct auto_op *a)
{
	struct dectnrp_driver_event evt = {
		.code = DECTNRP_EVENT_OP_FINISHED,
		.op_finished.op = a->op,
	};

	a->op = NULL;
	mock_driver_emit(&evt);
}

static void auto_emit_received(struct auto_op *a, int64_t frame_ms)
{
	struct dectnrp_driver_event evt = {
		.code = DECTNRP_EVENT_MSG_RECEIVED,
		.msg_received =
			{
				.op = a->op,
				.status = 0,
				.phy_type = AUTO_PHY_TYPE_2,
				.rssi2_valid = 1,
				.snr_valid = 1,
				.start_time = frame_ms * MOCK_TICKS_PER_MS,
				.pcc = auto_pcc,
				.pcc_len = sizeof(auto_pcc),
				.pdc = (const uint8_t *)auto_pdc,
				.pdc_len = AUTO_PDC_LEN,
				.rssi2 = AUTO_RSSI2,
				.snr = AUTO_SNR,
			},
	};

	mock_driver_emit(&evt);
}

static void auto_emit_error(struct auto_op *a, int64_t frame_ms)
{
	struct dectnrp_driver_event evt = {
		.code = DECTNRP_EVENT_MSG_ERROR,
		.message_error =
			{
				.op = a->op,
				.status = DECTNRP_ERROR_PDC_CRC,
				.rssi2_valid = 1,
				.snr_valid = 1,
				.pcc_valid = 1,
				.phy_type = AUTO_PHY_TYPE_2,
				.start_time = frame_ms * MOCK_TICKS_PER_MS,
				.pcc = auto_pcc,
				.rssi2 = AUTO_RSSI2,
				.snr = AUTO_SNR,
			},
	};

	mock_driver_emit(&evt);
}

static void auto_emit_frame(struct auto_op *a, int64_t frame_ms)
{
	int64_t index;

	index = frame_ms / AUTO_EMIT_PERIOD_MS;
	if (index % AUTO_ERROR_PERIOD == 0) {
		auto_emit_error(a, frame_ms);
	} else {
		auto_emit_received(a, frame_ms);
	}
}

static void auto_emit_rssi1(struct auto_op *a)
{
	struct dectnrp_driver_op *op = a->op;
	struct dectnrp_rssi1_result *result = op->rssi1.result;
	struct dectnrp_driver_event evt = {
		.code = DECTNRP_EVENT_OP_RESULTS,
		.rssi1_result.op = op,
	};

	memset(result->subslot, AUTO_RSSI1, sizeof(result->subslot));
	result->channel = op->channel;
	mock_driver_emit(&evt);
}

/* Run what is due at @p now, then wait for the next thing to do. */
static void auto_step(struct auto_op *a, int64_t now)
{
	struct dectnrp_driver_op *op = a->op;
	bool is_rx;
	bool frame_due;
	bool single_shot;
	int64_t wake_ms;

	is_rx = op->type == DECTNRP_DRIVER_OP_RX;
	if (is_rx) {
		frame_due = now >= a->next_frame_ms && a->next_frame_ms < a->end_ms;
		if (frame_due) {
			auto_emit_frame(a, a->next_frame_ms);
			a->next_frame_ms += AUTO_EMIT_PERIOD_MS;

			/* Like the nRF91 modem, a single shot window closes on reception. */
			single_shot = op->rx.mode.reception == DECTNRP_RX_RECEPTION_SINGLE_SHOT;
			if (single_shot) {
				auto_finish(a);
				return;
			}
		}
	}

	if (now >= a->end_ms) {
		if (op->type == DECTNRP_DRIVER_OP_RSSI1) {
			auto_emit_rssi1(a);
		}
		auto_finish(a);
		return;
	}

	wake_ms = a->end_ms;
	if (is_rx && a->next_frame_ms < wake_ms) {
		wake_ms = a->next_frame_ms;
	}
	(void)k_work_reschedule(&a->work, K_TIMEOUT_ABS_MS(wake_ms));
}

static void auto_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct auto_op *a = CONTAINER_OF(dwork, struct auto_op, work);
	int64_t now;

	k_mutex_lock(&auto_lock, K_FOREVER);
	if (a->op != NULL) {
		now = k_uptime_get();
		auto_step(a, now);
	}
	k_mutex_unlock(&auto_lock);
}

static struct auto_op *auto_alloc(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(auto_ops); i++) {
		if (auto_ops[i].op == NULL) {
			return &auto_ops[i];
		}
	}

	return NULL;
}

static int auto_schedule(struct dectnrp_driver_op *op)
{
	struct auto_op *a;
	int64_t now;
	bool scheduled;
	int64_t duration_ms;

	now = k_uptime_get();
	scheduled = auto_is_scheduled(op);
	duration_ms = auto_duration_ms(op);

	k_mutex_lock(&auto_lock, K_FOREVER);
	a = auto_alloc();
	if (a == NULL) {
		k_mutex_unlock(&auto_lock);
		return -ENOMEM;
	}

	a->op = op;
	if (scheduled) {
		a->start_ms = op->start_time / MOCK_TICKS_PER_MS;
	} else {
		a->start_ms = now;
	}
	a->end_ms = a->start_ms + duration_ms;
	a->next_frame_ms = auto_next_frame(a->start_ms);
	(void)k_work_reschedule(&a->work, K_TIMEOUT_ABS_MS(a->start_ms));
	k_mutex_unlock(&auto_lock);

	return 0;
}

/* Cancel every operation, reporting each one finished. */
static void auto_stop(void)
{
	k_mutex_lock(&auto_lock, K_FOREVER);
	for (size_t i = 0; i < ARRAY_SIZE(auto_ops); i++) {
		struct auto_op *a = &auto_ops[i];

		if (a->op == NULL) {
			continue;
		}
		(void)k_work_cancel_delayable(&a->work);
		auto_finish(a);
	}
	k_mutex_unlock(&auto_lock);
}

static void auto_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(auto_ops); i++) {
		k_work_init_delayable(&auto_ops[i].work, auto_work_handler);
	}
}

#endif /* CONFIG_DECTNRP_DRIVER_MOCK_AUTO */

static int mock_start(const struct device *dev)
{
	ARG_UNUSED(dev);

	mock.start_calls++;
	if (mock.start_ret == 0) {
		mock.started = true;
	}

	return mock.start_ret;
}

static int mock_stop(const struct device *dev)
{
	ARG_UNUSED(dev);

	mock.stop_calls++;
	mock.started = false;

#ifdef CONFIG_DECTNRP_DRIVER_MOCK_AUTO
	auto_stop();
#endif

	/* Like the nRF91 driver, stopping cancels every pending operation. */
	for (size_t i = 0; i < mock.n_ops; i++) {
		struct dectnrp_driver_event evt = {
			.code = DECTNRP_EVENT_OP_FINISHED,
			.op_finished.op = mock.ops[i],
		};

		if (op_finished[i]) {
			continue;
		}
		op_finished[i] = true;
		mock_driver_emit(&evt);
	}

	return 0;
}

static enum dectnrp_hw_caps mock_get_hw_capabilities(const struct device *dev)
{
	ARG_UNUSED(dev);

	return DECTNRP_HW_FILTER | DECTNRP_HW_TXTIME | DECTNRP_HW_RXTIME;
}

static int mock_get_capabilities(const struct device *dev, struct dectnrp_device_capabilities *caps)
{
	/* nRF9131 latencies, in modem ticks. */
	*caps = (struct dectnrp_device_capabilities){
		.hw_caps = mock_get_hw_capabilities(dev),
		.sched_op_transition = 25920,
		.sched_op_startup = 0,
		.rx_startup = 24192,
		.rx_shutdown = 12441,
		.rssi_shutdown = 13824,
		.tx_startup = 29030,
		.tx_shutdown = 7603,
		.stop_to_rf_off = 14169,
	};

	return 0;
}

static int mock_configure(const struct device *dev, enum dectnrp_config_type type,
			  const struct dectnrp_config *config)
{
	ARG_UNUSED(dev);

	switch (type) {
	case DECTNRP_CONFIG_TYPE_EVENT_HANDLER:
		mock.cb = config->event_handler;
		break;
	case DECTNRP_CONFIG_TYPE_NETWORK_ID:
		mock.network_id = config->network_id;
		break;
	case DECTNRP_CONFIG_TYPE_FILTER_SHORT_NETWORK_ID:
		mock.nwid_filter = config->filter_short_network_id.activate;
		mock.nwid = config->filter_short_network_id.value;
		break;
	case DECTNRP_CONFIG_TYPE_FILTER_SHORT_RECEIVER_ADDR:
		mock.rxaddr_filter = config->filter_short_receiver_addr.activate;
		mock.rxaddr = config->filter_short_receiver_addr.value;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static int mock_get_time(const struct device *dev, net_time_t *time)
{
	ARG_UNUSED(dev);

	if (mock.time != 0) {
		*time = mock.time;
	} else {
		*time = k_uptime_get() * MOCK_TICKS_PER_MS;
	}

	return 0;
}

static int mock_schedule(const struct device *dev, struct dectnrp_driver_op *op)
{
	ARG_UNUSED(dev);

	if (mock.schedule_ret != 0) {
		return mock.schedule_ret;
	}

#ifdef CONFIG_DECTNRP_DRIVER_MOCK_AUTO
	return auto_schedule(op);
#else
	if (mock.n_ops == ARRAY_SIZE(mock.ops)) {
		return -ENOMEM;
	}

	op_finished[mock.n_ops] = false;
	mock.ops[mock.n_ops] = op;
	mock.n_ops++;

	return 0;
#endif
}

static void mock_iface_init(struct net_if *iface)
{
	mock_iface = iface;
	dectnrp_l2_init(iface);
}

static const struct dectnrp_driver_api mock_api = {
	.iface_api = {.init = mock_iface_init},
	.start = mock_start,
	.stop = mock_stop,
	.get_hw_capabilities = mock_get_hw_capabilities,
	.get_capabilities = mock_get_capabilities,
	.configure = mock_configure,
	.get_time = mock_get_time,
	.schedule = mock_schedule,
};

static int mock_init(const struct device *dev)
{
	ARG_UNUSED(dev);

#ifdef CONFIG_DECTNRP_DRIVER_MOCK_AUTO
	auto_init();
#endif

	return 0;
}

NET_DEVICE_INIT(dectnrp_mock, "dectnrp_mock", mock_init, NULL, NULL, NULL,
		CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &mock_api, DECTNRP_L2,
		NET_L2_GET_CTX_TYPE(DECTNRP_L2), DECTNRP_MTU);

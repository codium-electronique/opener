/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/net/dectnrp.h>
#include <zephyr/net/dectnrp_spinel.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/ztest.h>

#include "mock_driver.h"
#include "spinel_client.h"

#define CHANNEL     1677
#define RX_DURATION 691200

static const uint8_t pcc[DECTNRP_PHY_HEADER_TYPE1_SIZE] = {0x10, 0x20, 0x30, 0x40, 0x50};
static const char pdc[] = "Hello World!";

/* DRV_EVENT as decoded by the host: common prefix, then the code's suffix. */
struct test_event {
	uint16_t token;
	uint8_t code;
	int32_t status;
	uint16_t dropped;
	const uint8_t *suffix;
	int suffix_len;
};

static void insert_rx(uint8_t tid, uint16_t token, uint8_t flags, uint64_t start_time)
{
	send_req(tid, SPINEL_CMD_PROP_VALUE_INSERT, DECTNRP_SPINEL_PROP_DRV_OP_QUEUE,
		 DECTNRP_SPINEL_FMT_OP_PREFIX DECTNRP_SPINEL_FMT_OP_RX, token, DECTNRP_SPINEL_OP_RX,
		 CHANNEL, flags, start_time, (uint32_t)RX_DURATION, 0);
}

static void insert_rssi1(uint8_t tid, uint16_t token, uint32_t subslots)
{
	send_req(tid, SPINEL_CMD_PROP_VALUE_INSERT, DECTNRP_SPINEL_PROP_DRV_OP_QUEUE,
		 DECTNRP_SPINEL_FMT_OP_PREFIX DECTNRP_SPINEL_FMT_OP_RSSI1, token,
		 DECTNRP_SPINEL_OP_RSSI1, CHANNEL, 0, (uint64_t)0, subslots);
}

/* TX op on CHANNEL, PHY header type 1: tbs 17, mu 1, beta 1, mcs 1, 2 slots, no LBT. */
static void insert_tx(uint8_t tid, uint16_t token, uint8_t flags, const uint8_t *data, uint32_t len)
{
	send_req(tid, SPINEL_CMD_PROP_VALUE_INSERT, DECTNRP_SPINEL_PROP_DRV_OP_QUEUE,
		 DECTNRP_SPINEL_FMT_OP_PREFIX DECTNRP_SPINEL_FMT_OP_TX, token, DECTNRP_SPINEL_OP_TX,
		 CHANNEL, flags, (uint64_t)0, 0, 17, 1, 1, 1, 2, (uint32_t)0, 0, data, len);
}

static void expect_inserted(uint8_t tid, uint16_t token)
{
	uint8_t reply_tid;
	unsigned int cmd;
	unsigned int prop;
	const uint8_t *payload;
	uint16_t reply_token;
	spinel_ssize_t n;
	int len;

	len = recv_reply(&reply_tid, &cmd, &prop, &payload);
	zassert_equal(reply_tid, tid);
	zassert_equal(cmd, SPINEL_CMD_PROP_VALUE_INSERTED, "cmd %u", cmd);
	zassert_equal(prop, DECTNRP_SPINEL_PROP_DRV_OP_QUEUE);

	n = spinel_datatype_unpack(payload, len, "S", &reply_token);
	zassert_true(n > 0);
	zassert_equal(reply_token, token);
}

static void recv_event(struct test_event *evt)
{
	const uint8_t *payload;
	spinel_ssize_t n;
	int len;

	len = recv_is(0, DECTNRP_SPINEL_PROP_DRV_EVENT, &payload);
	n = spinel_datatype_unpack(payload, len, DECTNRP_SPINEL_FMT_EVT_PREFIX, &evt->token,
				   &evt->code, &evt->status, &evt->dropped);
	zassert_true(n > 0);

	evt->suffix = &payload[n];
	evt->suffix_len = len - n;
}

static void expect_finished(uint16_t token)
{
	struct test_event evt;

	recv_event(&evt);
	zassert_equal(evt.token, token);
	zassert_equal(evt.code, DECTNRP_SPINEL_EVENT_OP_FINISHED, "code %u", evt.code);
	zassert_equal(evt.suffix_len, 0);
}

/* The slot is released after the TX thread sent OP_FINISHED: wait for it. */
static void wait_in_flight(size_t expected)
{
	size_t in_flight;

	for (int i = 0; i < 100; i++) {
		in_flight = spinel_ops_in_flight();
		if (in_flight == expected) {
			return;
		}
		k_msleep(1);
	}
	zassert_equal(in_flight, expected);
}

static void emit_msg_received(struct dectnrp_driver_op *op)
{
	struct dectnrp_driver_event evt = {
		.code = DECTNRP_EVENT_MSG_RECEIVED,
		.msg_received =
			{
				.op = op,
				.phy_type = 0,
				.rssi2_valid = 1,
				.snr_valid = 0,
				.start_time = 42,
				.pcc = pcc,
				.pcc_len = sizeof(pcc),
				.pdc = (const uint8_t *)pdc,
				.pdc_len = strlen(pdc),
				.rssi2 = 0x50,
				.snr = 12,
			},
	};

	mock_driver_emit(&evt);
}

static void emit_op_finished(struct dectnrp_driver_op *op)
{
	struct dectnrp_driver_event evt = {
		.code = DECTNRP_EVENT_OP_FINISHED,
		.op_finished.op = op,
	};

	mock_driver_emit(&evt);
}

static void *setup(void)
{
	uint8_t tid;
	unsigned int cmd;
	unsigned int prop;
	const uint8_t *payload;

	/* Consume the boot notification, which the before hook may not see yet. */
	recv_reply(&tid, &cmd, &prop, &payload);
	zassert_equal(prop, SPINEL_PROP_LAST_STATUS);

	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	mock_driver_reset();
	drain_frames();
}

/* Reset the interface so the next test starts with every slot free. */
static void after(void *fixture)
{
	static const uint8_t reset[] = {SPINEL_HEADER_FLAG | 1, SPINEL_CMD_RESET};
	uint8_t tid;
	unsigned int cmd;
	unsigned int prop;
	unsigned int status;
	const uint8_t *payload;
	size_t in_flight;
	spinel_ssize_t n;
	int len;

	ARG_UNUSED(fixture);

	spinel_test_block_tx(false);
	send_raw(reset, sizeof(reset));

	/* Skip the OP_FINISHED events up to the reset notification. */
	for (int i = 0; i < 32; i++) {
		len = recv_reply(&tid, &cmd, &prop, &payload);
		if (prop != SPINEL_PROP_LAST_STATUS) {
			continue;
		}
		n = spinel_datatype_unpack(payload, len, "i", &status);
		zassert_true(n > 0);
		zassert_equal(status, SPINEL_STATUS_RESET_SOFTWARE);
		break;
	}
	in_flight = spinel_ops_in_flight();
	zassert_equal(in_flight, 0);
}

ZTEST(spinel_ops, test_insert_rx_scheduled)
{
	struct mock_driver *mock = mock_driver_get();
	struct dectnrp_driver_op *op;

	insert_rx(1, 7, DECTNRP_SPINEL_OP_FLAG_SCHEDULED, 1000);
	expect_inserted(1, 7);

	zassert_equal(mock->n_ops, 1);
	op = mock->ops[0];
	zassert_equal(op->type, DECTNRP_DRIVER_OP_RX);
	zassert_equal(op->rx.mode.is_scheduled, 1);
	zassert_equal(op->rx.mode.reception, DECTNRP_RX_RECEPTION_SINGLE_SHOT);
	zassert_equal(op->start_time, 1000);
	zassert_equal(op->rx.duration, RX_DURATION);
	zassert_equal(op->rx.expected_rssi, 0);
	zassert_equal(op->channel, CHANNEL);
}

ZTEST(spinel_ops, test_insert_rx_reception)
{
	struct mock_driver *mock = mock_driver_get();
	struct dectnrp_driver_op *op;
	uint8_t flags;

	flags = DECTNRP_SPINEL_OP_FLAG_SCHEDULED |
		(DECTNRP_RX_RECEPTION_SEMICONTINUOUS << DECTNRP_SPINEL_OP_FLAG_RX_RECEPTION_SHIFT);
	insert_rx(1, 7, flags, 1000);
	expect_inserted(1, 7);
	op = mock->ops[0];
	zassert_equal(op->rx.mode.reception, DECTNRP_RX_RECEPTION_SEMICONTINUOUS);

	/* Reception mode 3 does not exist. */
	flags = 3 << DECTNRP_SPINEL_OP_FLAG_RX_RECEPTION_SHIFT;
	insert_rx(2, 8, flags, 0);
	expect_status(2, SPINEL_STATUS_INVALID_ARGUMENT);

	/* The reception mode is for RX ops only. */
	flags = DECTNRP_RX_RECEPTION_CONTINUOUS << DECTNRP_SPINEL_OP_FLAG_RX_RECEPTION_SHIFT;
	insert_tx(3, 9, flags, pcc, sizeof(pcc));
	expect_status(3, SPINEL_STATUS_INVALID_ARGUMENT);

	zassert_equal(mock->n_ops, 1);
}

ZTEST(spinel_ops, test_insert_tx)
{
	struct mock_driver *mock = mock_driver_get();
	struct dectnrp_driver_op *op;
	uint8_t data[sizeof(pcc) + sizeof(pdc) - 1];
	uint8_t sent[sizeof(data)];
	struct net_pkt *pkt;
	size_t pkt_len;
	int err;

	memcpy(data, pcc, sizeof(pcc));
	memcpy(&data[sizeof(pcc)], pdc, strlen(pdc));

	insert_tx(1, 7, 0, data, sizeof(data));
	expect_inserted(1, 7);

	zassert_equal(mock->n_ops, 1);
	op = mock->ops[0];
	zassert_equal(op->type, DECTNRP_DRIVER_OP_TX);
	zassert_equal(op->channel, CHANNEL);
	zassert_equal(op->tx.mode.is_scheduled, 0);
	zassert_equal(op->tx.phy_header_type, 0);
	zassert_equal(op->tx.transport_parameters.tbs, 17);
	zassert_equal(op->tx.transport_parameters.mu, 1);
	zassert_equal(op->tx.transport_parameters.beta, 1);
	zassert_equal(op->tx.transport_parameters.mcs, 1);
	zassert_equal(op->tx.transport_parameters.slots, 2);

	pkt = op->tx.pkt;
	zassert_not_null(pkt);
	pkt_len = net_pkt_get_len(pkt);
	zassert_equal(pkt_len, 17);
	net_pkt_cursor_init(pkt);
	err = net_pkt_read(pkt, sent, sizeof(sent));
	zassert_equal(err, 0);
	zassert_mem_equal(sent, data, sizeof(data));
}

ZTEST(spinel_ops, test_insert_tx_too_short)
{
	struct mock_driver *mock = mock_driver_get();

	/* Shorter than the 5-byte PHY header. */
	insert_tx(1, 7, 0, pcc, 3);
	expect_status(1, SPINEL_STATUS_INVALID_ARGUMENT);
	zassert_equal(mock->n_ops, 0);
}

ZTEST(spinel_ops, test_insert_tx_larger_than_net_buf)
{
	struct mock_driver *mock = mock_driver_get();
	static uint8_t data[CONFIG_NET_BUF_DATA_SIZE + 1];

	/* Drivers transmit the first fragment of the packet only. */
	memset(data, 0x5A, sizeof(data));
	memcpy(data, pcc, sizeof(pcc));

	insert_tx(1, 7, 0, data, sizeof(data));
	expect_status(1, SPINEL_STATUS_INVALID_ARGUMENT);
	zassert_equal(mock->n_ops, 0);
}

ZTEST(spinel_ops, test_duplicate_token)
{
	insert_rx(1, 7, 0, 0);
	expect_inserted(1, 7);

	insert_rx(2, 7, 0, 0);
	expect_status(2, SPINEL_STATUS_ALREADY);
}

ZTEST(spinel_ops, test_slots_exhausted)
{
	uint8_t tid;

	for (uint16_t token = 1; token <= CONFIG_DECTNRP_SPINEL_OP_SLOTS; token++) {
		tid = 1 + (token % 14);
		insert_rx(tid, token, 0, 0);
		expect_inserted(tid, token);
	}

	insert_rx(15, 100, 0, 0);
	expect_status(15, SPINEL_STATUS_NOMEM);
}

ZTEST(spinel_ops, test_schedule_error)
{
	struct mock_driver *mock = mock_driver_get();
	size_t in_flight;

	mock->schedule_ret = -EBUSY;
	insert_rx(1, 7, 0, 0);
	expect_status(1, SPINEL_STATUS_BUSY);
	in_flight = spinel_ops_in_flight();
	zassert_equal(in_flight, 0);

	/* The slot was not leaked: the same token is accepted now. */
	mock->schedule_ret = 0;
	insert_rx(2, 7, 0, 0);
	expect_inserted(2, 7);
}

ZTEST(spinel_ops, test_msg_received_event)
{
	struct mock_driver *mock = mock_driver_get();
	struct test_event evt;
	uint8_t phy_type;
	uint8_t valid;
	uint64_t start_time;
	uint8_t rssi2;
	int8_t snr;
	uint16_t channel;
	const uint8_t *rx_pcc;
	unsigned int rx_pcc_len;
	const uint8_t *rx_pdc;
	unsigned int rx_pdc_len;
	spinel_ssize_t n;

	insert_rx(1, 7, 0, 0);
	expect_inserted(1, 7);
	emit_msg_received(mock->ops[0]);

	recv_event(&evt);
	zassert_equal(evt.token, 7);
	zassert_equal(evt.code, DECTNRP_SPINEL_EVENT_MSG_RECEIVED);
	zassert_equal(evt.status, 0);
	zassert_equal(evt.dropped, 0);

	n = spinel_datatype_unpack(evt.suffix, evt.suffix_len, DECTNRP_SPINEL_FMT_EVT_MSG_RECEIVED,
				   &phy_type, &valid, &start_time, &rssi2, &snr, &channel, &rx_pcc,
				   &rx_pcc_len, &rx_pdc, &rx_pdc_len);
	zassert_equal(n, evt.suffix_len);
	zassert_equal(phy_type, 0);
	zassert_equal(valid, DECTNRP_SPINEL_RX_VALID_RSSI2);
	zassert_equal(start_time, 42);
	zassert_equal(rssi2, 0x50);
	zassert_equal(snr, 12);
	zassert_equal(channel, CHANNEL);
	zassert_equal(rx_pcc_len, sizeof(pcc));
	zassert_mem_equal(rx_pcc, pcc, sizeof(pcc));
	zassert_equal(rx_pdc_len, strlen(pdc));
	zassert_mem_equal(rx_pdc, pdc, strlen(pdc));
}

ZTEST(spinel_ops, test_msg_error_event_no_pcc)
{
	struct mock_driver *mock = mock_driver_get();
	struct dectnrp_driver_event driver_evt = {
		.code = DECTNRP_EVENT_MSG_ERROR,
		.message_error =
			{
				.status = DECTNRP_ERROR_PCC_CRC,
				.rssi2_valid = 1,
				.snr_valid = 1,
				.pcc_valid = 0,
				.phy_type = 1,
				.start_time = 4242,
				.rssi2 = 0x60,
				.snr = -3,
			},
	};
	struct test_event evt;
	uint8_t phy_type;
	uint8_t valid;
	uint64_t start_time;
	uint8_t rssi2;
	int8_t snr;
	uint16_t channel;
	const uint8_t *rx_pcc;
	unsigned int rx_pcc_len;
	spinel_ssize_t n;

	insert_rx(1, 7, 0, 0);
	expect_inserted(1, 7);
	driver_evt.message_error.op = mock->ops[0];
	mock_driver_emit(&driver_evt);

	recv_event(&evt);
	zassert_equal(evt.token, 7);
	zassert_equal(evt.code, DECTNRP_SPINEL_EVENT_MSG_ERROR);
	zassert_equal(evt.status, DECTNRP_ERROR_PCC_CRC);

	n = spinel_datatype_unpack(evt.suffix, evt.suffix_len, DECTNRP_SPINEL_FMT_EVT_MSG_ERROR,
				   &phy_type, &valid, &start_time, &rssi2, &snr, &channel, &rx_pcc,
				   &rx_pcc_len);
	zassert_equal(n, evt.suffix_len);
	zassert_equal(phy_type, 1);
	zassert_equal(valid, DECTNRP_SPINEL_RX_VALID_RSSI2 | DECTNRP_SPINEL_RX_VALID_SNR);
	zassert_equal(start_time, 4242);
	zassert_equal(rssi2, 0x60);
	zassert_equal(snr, -3);
	zassert_equal(channel, CHANNEL);
	zassert_equal(rx_pcc_len, 0);
}

ZTEST(spinel_ops, test_rssi1_results)
{
	struct mock_driver *mock = mock_driver_get();
	struct dectnrp_driver_op *op;
	struct dectnrp_driver_event driver_evt = {.code = DECTNRP_EVENT_OP_RESULTS};
	struct test_event evt;
	uint16_t channel;
	const uint8_t *rssi1;
	unsigned int rssi1_len;
	spinel_ssize_t n;

	insert_rssi1(1, 7, DECTNRP_SUBSLOTS_PER_FRAME);
	expect_inserted(1, 7);

	op = mock->ops[0];
	zassert_equal(op->type, DECTNRP_DRIVER_OP_RSSI1);
	zassert_equal(op->rssi1.subslots, DECTNRP_SUBSLOTS_PER_FRAME);
	zassert_not_null(op->rssi1.result);

	/* The driver writes the measurements into the result storage. */
	for (size_t i = 0; i < DECTNRP_SUBSLOTS_PER_FRAME; i++) {
		op->rssi1.result->subslot[i] = 100 + i;
	}
	driver_evt.rssi1_result.op = op;
	mock_driver_emit(&driver_evt);

	recv_event(&evt);
	zassert_equal(evt.token, 7);
	zassert_equal(evt.code, DECTNRP_SPINEL_EVENT_OP_RESULTS);

	n = spinel_datatype_unpack(evt.suffix, evt.suffix_len, DECTNRP_SPINEL_FMT_EVT_OP_RESULTS,
				   &channel, &rssi1, &rssi1_len);
	zassert_equal(n, evt.suffix_len);
	zassert_equal(channel, CHANNEL);
	zassert_equal(rssi1_len, DECTNRP_SUBSLOTS_PER_FRAME);
	for (size_t i = 0; i < DECTNRP_SUBSLOTS_PER_FRAME; i++) {
		zassert_equal(rssi1[i], 100 + i);
	}
}

ZTEST(spinel_ops, test_finished_frees_slot)
{
	struct mock_driver *mock = mock_driver_get();
	size_t in_flight;

	insert_rx(1, 7, 0, 0);
	expect_inserted(1, 7);
	in_flight = spinel_ops_in_flight();
	zassert_equal(in_flight, 1);

	emit_op_finished(mock->ops[0]);
	expect_finished(7);
	wait_in_flight(0);

	insert_rx(2, 7, 0, 0);
	expect_inserted(2, 7);
}

ZTEST(spinel_ops, test_event_pool_exhaustion)
{
	struct mock_driver *mock = mock_driver_get();
	struct dectnrp_driver_op *op;
	struct test_event evt;
	uint32_t dropped_before;
	uint32_t dropped_after;

	insert_rx(1, 7, 0, 0);
	expect_inserted(1, 7);
	op = mock->ops[0];
	dropped_before = read_stat(SPINEL_STAT_EVENTS_DROPPED);

	/* With the TX thread stuck, events pile up in the event pool. */
	spinel_test_block_tx(true);
	for (int i = 0; i < CONFIG_DECTNRP_SPINEL_EVENT_BUFS + 5; i++) {
		emit_msg_received(op);
	}
	emit_op_finished(op);
	spinel_test_block_tx(false);

	for (int i = 0; i < CONFIG_DECTNRP_SPINEL_EVENT_BUFS; i++) {
		recv_event(&evt);
		zassert_equal(evt.code, DECTNRP_SPINEL_EVENT_MSG_RECEIVED);
		zassert_equal(evt.dropped, 0);
	}

	/* OP_FINISHED has its own buffer and reports the dropped events. */
	recv_event(&evt);
	zassert_equal(evt.code, DECTNRP_SPINEL_EVENT_OP_FINISHED);
	zassert_equal(evt.dropped, 5);

	dropped_after = read_stat(SPINEL_STAT_EVENTS_DROPPED);
	zassert_equal(dropped_after, dropped_before + 5);
	wait_in_flight(0);
}

ZTEST(spinel_ops, test_reset_with_ops_in_flight)
{
	static const uint8_t reset[] = {SPINEL_HEADER_FLAG | 5, SPINEL_CMD_RESET};
	struct mock_driver *mock = mock_driver_get();
	size_t in_flight;

	for (uint16_t token = 1; token <= 3; token++) {
		insert_rx(token, token, 0, 0);
		expect_inserted(token, token);
	}

	send_raw(reset, sizeof(reset));
	expect_finished(1);
	expect_finished(2);
	expect_finished(3);
	expect_status(0, SPINEL_STATUS_RESET_SOFTWARE);

	zassert_equal(mock->stop_calls, 1);
	in_flight = spinel_ops_in_flight();
	zassert_equal(in_flight, 0);
}

ZTEST_SUITE(spinel_ops, NULL, setup, before, after, NULL);

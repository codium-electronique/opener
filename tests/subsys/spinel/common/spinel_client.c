/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdarg.h>

#include <zephyr/net/dectnrp_spinel.h>
#include <zephyr/ztest.h>

#include "spinel_client.h"

static uint8_t rx_frame[CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE];

void send_raw(const uint8_t *frame, size_t len)
{
	spinel_rx_submit(frame, len);
}

void send_req(uint8_t tid, uint32_t cmd, spinel_prop_key_t prop, const char *fmt, ...)
{
	uint8_t frame[128];
	uint8_t header;
	spinel_ssize_t header_len;
	spinel_ssize_t value_len;
	va_list ap;

	header = SPINEL_HEADER_FLAG | tid;
	header_len = spinel_datatype_pack(frame, sizeof(frame), "Cii", header, cmd, prop);
	zassert_true(header_len > 0);

	va_start(ap, fmt);
	value_len = spinel_datatype_vpack(&frame[header_len], sizeof(frame) - header_len, fmt, ap);
	va_end(ap);
	zassert_true(value_len >= 0);
	zassert_true(header_len + value_len <= sizeof(frame));

	send_raw(frame, header_len + value_len);
}

int parse_reply(const uint8_t *frame, int len, uint8_t *tid, unsigned int *cmd, unsigned int *prop,
		const uint8_t **payload)
{
	uint8_t header;
	uint8_t flags_and_iid;
	spinel_ssize_t n;

	n = spinel_datatype_unpack(frame, len, "Cii", &header, cmd, prop);
	zassert_true(n > 0, "unparsable reply");

	/* Device frames: FLG = 0b10, IID = 0. */
	flags_and_iid = header & ~SPINEL_HEADER_TID_MASK;
	zassert_equal(flags_and_iid, SPINEL_HEADER_FLAG);

	*tid = SPINEL_HEADER_GET_TID(header);
	*payload = &frame[n];

	return len - n;
}

int recv_reply(uint8_t *tid, unsigned int *cmd, unsigned int *prop, const uint8_t **payload)
{
	int len;

	len = spinel_test_recv(rx_frame, sizeof(rx_frame), CLIENT_TIMEOUT);
	zassert_true(len > 0, "no reply (%d)", len);

	return parse_reply(rx_frame, len, tid, cmd, prop, payload);
}

int recv_is(uint8_t tid, spinel_prop_key_t prop, const uint8_t **payload)
{
	uint8_t reply_tid;
	unsigned int reply_cmd;
	unsigned int reply_prop;
	int len;

	len = recv_reply(&reply_tid, &reply_cmd, &reply_prop, payload);
	zassert_equal(reply_tid, tid);
	zassert_equal(reply_cmd, SPINEL_CMD_PROP_VALUE_IS);
	zassert_equal(reply_prop, prop, "prop 0x%x", reply_prop);

	return len;
}

void expect_status(uint8_t tid, unsigned int status)
{
	const uint8_t *payload;
	unsigned int value;
	spinel_ssize_t n;
	int len;

	len = recv_is(tid, SPINEL_PROP_LAST_STATUS, &payload);
	n = spinel_datatype_unpack(payload, len, "i", &value);
	zassert_true(n > 0);
	zassert_equal(value, status, "status 0x%x", value);
}

void expect_no_frame(k_timeout_t timeout)
{
	int len;

	len = spinel_test_recv(rx_frame, sizeof(rx_frame), timeout);
	zassert_equal(len, -EAGAIN, "unexpected frame (%d)", len);
}

uint32_t read_stat(enum spinel_stat stat)
{
	const uint8_t *payload;
	uint32_t values[SPINEL_STAT_COUNT];
	spinel_ssize_t n;
	int len;

	send_req(9, SPINEL_CMD_PROP_VALUE_GET, DECTNRP_SPINEL_PROP_DRV_STATS, "");
	len = recv_is(9, DECTNRP_SPINEL_PROP_DRV_STATS, &payload);
	n = spinel_datatype_unpack(
		payload, len, DECTNRP_SPINEL_FMT_DRV_STATS, &values[SPINEL_STAT_OPS_ACCEPTED],
		&values[SPINEL_STAT_OPS_REJECTED], &values[SPINEL_STAT_EVENTS_SENT],
		&values[SPINEL_STAT_EVENTS_DROPPED], &values[SPINEL_STAT_FRAMES_RX],
		&values[SPINEL_STAT_FRAMES_REJECTED], &values[SPINEL_STAT_TRANSPORT_ERRORS]);
	zassert_true(n > 0);

	return values[stat];
}

void drain_frames(void)
{
	int len;

	do {
		len = spinel_test_recv(rx_frame, sizeof(rx_frame), K_NO_WAIT);
	} while (len >= 0);
}

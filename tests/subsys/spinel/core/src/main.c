/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/net/dectnrp_spinel.h>
#include <zephyr/ztest.h>

#include "mock_driver.h"
#include "spinel_client.h"

static uint8_t boot_frame[CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE];
static int boot_len;

static void *setup(void)
{
	/* Keep the boot notification: the before hook drains the queue. */
	boot_len = spinel_test_recv(boot_frame, sizeof(boot_frame), K_SECONDS(1));

	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	mock_driver_reset();
	/* Drop frames left over by the previous test. */
	drain_frames();
}

ZTEST(spinel_core, test_00_boot_reset_notification)
{
	uint8_t tid;
	unsigned int cmd;
	unsigned int prop;
	unsigned int status;
	const uint8_t *payload;
	spinel_ssize_t n;
	int len;

	zassert_true(boot_len > 0, "no boot notification (%d)", boot_len);
	len = parse_reply(boot_frame, boot_len, &tid, &cmd, &prop, &payload);
	zassert_equal(tid, 0);
	zassert_equal(cmd, SPINEL_CMD_PROP_VALUE_IS);
	zassert_equal(prop, SPINEL_PROP_LAST_STATUS);

	n = spinel_datatype_unpack(payload, len, "i", &status);
	zassert_true(n > 0);
	zassert_equal(status, SPINEL_STATUS_RESET_POWER_ON);
}

ZTEST(spinel_core, test_noop)
{
	static const uint8_t noop[] = {SPINEL_HEADER_FLAG | 3, SPINEL_CMD_NOOP};

	send_raw(noop, sizeof(noop));
	expect_status(3, SPINEL_STATUS_OK);
}

ZTEST(spinel_core, test_protocol_version)
{
	const uint8_t *payload;
	unsigned int major;
	unsigned int minor;
	spinel_ssize_t n;
	int len;

	send_req(1, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_PROTOCOL_VERSION, "");
	len = recv_is(1, SPINEL_PROP_PROTOCOL_VERSION, &payload);
	n = spinel_datatype_unpack(payload, len, "ii", &major, &minor);
	zassert_true(n > 0);
	zassert_equal(major, 4);
	zassert_equal(minor, 3);
}

ZTEST(spinel_core, test_ncp_version)
{
	const uint8_t *payload;
	const char *version;
	const char *driver_name;
	spinel_ssize_t n;
	int len;

	send_req(1, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_NCP_VERSION, "");
	len = recv_is(1, SPINEL_PROP_NCP_VERSION, &payload);
	n = spinel_datatype_unpack(payload, len, "U", &version);
	zassert_true(n > 0);

	zassert_mem_equal(version, "OPENER/", 7, "%s", version);
	driver_name = strstr(version, "; dectnrp_mock; ");
	zassert_not_null(driver_name, "%s", version);
}

ZTEST(spinel_core, test_interface_type)
{
	const uint8_t *payload;
	unsigned int type;
	spinel_ssize_t n;
	int len;

	send_req(2, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_INTERFACE_TYPE, "");
	len = recv_is(2, SPINEL_PROP_INTERFACE_TYPE, &payload);
	n = spinel_datatype_unpack(payload, len, "i", &type);
	zassert_true(n > 0);
	zassert_equal(type, 0x4E52);
}

ZTEST(spinel_core, test_caps_contains_drv)
{
	const uint8_t *payload;
	unsigned int cap;
	bool found = false;
	spinel_ssize_t n;
	int len;

	send_req(4, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_CAPS, "");
	len = recv_is(4, SPINEL_PROP_CAPS, &payload);

	/* CAPS is a sequence of packed integers. */
	while (len > 0) {
		n = spinel_packed_uint_decode(payload, len, &cap);
		zassert_true(n > 0);
		if (cap == 0x3D00) {
			found = true;
		}
		payload += n;
		len -= n;
	}
	zassert_true(found);
}

/* Set DRV_ENABLED; return the value in the reply. */
static bool set_enabled(uint8_t tid, bool enable)
{
	const uint8_t *payload;
	bool value;
	spinel_ssize_t n;
	int len;

	send_req(tid, SPINEL_CMD_PROP_VALUE_SET, DECTNRP_SPINEL_PROP_DRV_ENABLED, "b", enable);
	len = recv_is(tid, DECTNRP_SPINEL_PROP_DRV_ENABLED, &payload);
	n = spinel_datatype_unpack(payload, len, "b", &value);
	zassert_true(n > 0);

	return value;
}

ZTEST(spinel_core, test_enable_disable)
{
	struct mock_driver *mock = mock_driver_get();
	bool enabled;

	enabled = set_enabled(5, true);
	zassert_true(enabled);
	zassert_true(mock->started);

	/* Starting again: the driver returns -EALREADY, which is not an error. */
	mock->start_ret = -EALREADY;
	enabled = set_enabled(6, true);
	zassert_true(enabled);
	zassert_true(mock->started);

	enabled = set_enabled(7, false);
	zassert_false(enabled);
	zassert_false(mock->started);
	zassert_equal(mock->stop_calls, 1);
}

ZTEST(spinel_core, test_network_id_roundtrip)
{
	struct mock_driver *mock = mock_driver_get();
	const uint8_t *payload;
	uint32_t value;
	spinel_ssize_t n;
	int len;

	send_req(8, SPINEL_CMD_PROP_VALUE_SET, DECTNRP_SPINEL_PROP_DRV_NETWORK_ID, "L", 0x32545279);
	recv_is(8, DECTNRP_SPINEL_PROP_DRV_NETWORK_ID, &payload);
	zassert_equal(mock->network_id, 0x32545279);

	send_req(9, SPINEL_CMD_PROP_VALUE_GET, DECTNRP_SPINEL_PROP_DRV_NETWORK_ID, "");
	len = recv_is(9, DECTNRP_SPINEL_PROP_DRV_NETWORK_ID, &payload);
	n = spinel_datatype_unpack(payload, len, "L", &value);
	zassert_true(n > 0);
	zassert_equal(value, 0x32545279);
}

ZTEST(spinel_core, test_filters)
{
	struct mock_driver *mock = mock_driver_get();
	const uint8_t *payload;
	bool active;
	uint8_t nwid;
	uint16_t rxaddr;
	spinel_ssize_t n;
	int len;

	send_req(10, SPINEL_CMD_PROP_VALUE_SET, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_NWID, "bC",
		 true, 0x79);
	recv_is(10, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_NWID, &payload);
	send_req(11, SPINEL_CMD_PROP_VALUE_SET, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_RXADDR, "bS",
		 true, 0xBEEF);
	recv_is(11, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_RXADDR, &payload);
	zassert_true(mock->nwid_filter);
	zassert_equal(mock->nwid, 0x79);
	zassert_true(mock->rxaddr_filter);
	zassert_equal(mock->rxaddr, 0xBEEF);

	send_req(12, SPINEL_CMD_PROP_VALUE_GET, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_NWID, "");
	len = recv_is(12, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_NWID, &payload);
	n = spinel_datatype_unpack(payload, len, "bC", &active, &nwid);
	zassert_true(n > 0);
	zassert_true(active);
	zassert_equal(nwid, 0x79);

	send_req(13, SPINEL_CMD_PROP_VALUE_GET, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_RXADDR, "");
	len = recv_is(13, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_RXADDR, &payload);
	n = spinel_datatype_unpack(payload, len, "bS", &active, &rxaddr);
	zassert_true(n > 0);
	zassert_true(active);
	zassert_equal(rxaddr, 0xBEEF);
}

ZTEST(spinel_core, test_reset_clears_filters)
{
	static const uint8_t reset[] = {SPINEL_HEADER_FLAG | 14, SPINEL_CMD_RESET};
	struct mock_driver *mock = mock_driver_get();
	const uint8_t *payload;
	bool active;
	uint8_t nwid;
	uint16_t rxaddr;
	spinel_ssize_t n;
	int len;

	send_req(10, SPINEL_CMD_PROP_VALUE_SET, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_NWID, "bC",
		 true, 0x79);
	recv_is(10, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_NWID, &payload);
	send_req(11, SPINEL_CMD_PROP_VALUE_SET, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_RXADDR, "bS",
		 true, 0xBEEF);
	recv_is(11, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_RXADDR, &payload);

	send_raw(reset, sizeof(reset));
	expect_status(0, SPINEL_STATUS_RESET_SOFTWARE);

	zassert_false(mock->nwid_filter);
	zassert_false(mock->rxaddr_filter);

	send_req(12, SPINEL_CMD_PROP_VALUE_GET, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_NWID, "");
	len = recv_is(12, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_NWID, &payload);
	n = spinel_datatype_unpack(payload, len, "bC", &active, &nwid);
	zassert_true(n > 0);
	zassert_false(active);

	send_req(13, SPINEL_CMD_PROP_VALUE_GET, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_RXADDR, "");
	len = recv_is(13, DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_RXADDR, &payload);
	n = spinel_datatype_unpack(payload, len, "bS", &active, &rxaddr);
	zassert_true(n > 0);
	zassert_false(active);
}

ZTEST(spinel_core, test_reset_clears_network_id)
{
	static const uint8_t reset[] = {SPINEL_HEADER_FLAG | 14, SPINEL_CMD_RESET};
	struct mock_driver *mock = mock_driver_get();
	const uint8_t *payload;
	uint32_t value;
	spinel_ssize_t n;
	int len;

	send_req(10, SPINEL_CMD_PROP_VALUE_SET, DECTNRP_SPINEL_PROP_DRV_NETWORK_ID, "L",
		 0x12345678);
	recv_is(10, DECTNRP_SPINEL_PROP_DRV_NETWORK_ID, &payload);
	zassert_equal(mock->network_id, 0x12345678);

	send_raw(reset, sizeof(reset));
	expect_status(0, SPINEL_STATUS_RESET_SOFTWARE);

	/* The driver's boot value. */
	zassert_equal(mock->network_id, 0);

	send_req(11, SPINEL_CMD_PROP_VALUE_GET, DECTNRP_SPINEL_PROP_DRV_NETWORK_ID, "");
	len = recv_is(11, DECTNRP_SPINEL_PROP_DRV_NETWORK_ID, &payload);
	n = spinel_datatype_unpack(payload, len, "L", &value);
	zassert_true(n > 0);
	zassert_equal(value, 0);
}

ZTEST(spinel_core, test_time)
{
	struct mock_driver *mock = mock_driver_get();
	const uint8_t *payload;
	uint64_t time;
	spinel_ssize_t n;
	int len;

	mock->time = 123456789;
	send_req(14, SPINEL_CMD_PROP_VALUE_GET, DECTNRP_SPINEL_PROP_DRV_TIME, "");
	len = recv_is(14, DECTNRP_SPINEL_PROP_DRV_TIME, &payload);
	n = spinel_datatype_unpack(payload, len, "X", &time);
	zassert_true(n > 0);
	zassert_equal(time, 123456789);
}

ZTEST(spinel_core, test_caps_layout)
{
	const uint8_t *payload;
	uint32_t hw_caps;
	uint8_t release;
	uint8_t power_class;
	uint8_t nss_rx;
	uint8_t diversity;
	int8_t rxgain;
	uint8_t mcs;
	uint32_t softbuffer;
	uint8_t harq_processes;
	uint8_t harq_feedback_delay;
	uint8_t mu;
	uint8_t beta;
	uint32_t ticks_per_symbol;
	uint16_t max_frame_size;
	uint8_t op_slots;
	uint32_t latency[8];
	spinel_ssize_t n;
	int len;

	send_req(15, SPINEL_CMD_PROP_VALUE_GET, DECTNRP_SPINEL_PROP_DRV_CAPS, "");
	len = recv_is(15, DECTNRP_SPINEL_PROP_DRV_CAPS, &payload);
	n = spinel_datatype_unpack(payload, len, DECTNRP_SPINEL_FMT_DRV_CAPS, &hw_caps, &release,
				   &power_class, &nss_rx, &diversity, &rxgain, &mcs, &softbuffer,
				   &harq_processes, &harq_feedback_delay, &mu, &beta,
				   &ticks_per_symbol, &max_frame_size, &op_slots, &latency[0],
				   &latency[1], &latency[2], &latency[3], &latency[4], &latency[5],
				   &latency[6], &latency[7]);
	zassert_equal(n, len);

	zassert_equal(hw_caps, 0x904);
	zassert_equal(release, 1);
	zassert_equal(power_class, 2);
	zassert_equal(rxgain, -4);
	zassert_equal(mcs, 4);
	zassert_equal(softbuffer, 25344);
	zassert_equal(harq_processes, 8);
	zassert_equal(ticks_per_symbol, 2880);
	zassert_equal(max_frame_size, 1300);
	zassert_equal(op_slots, 8);

	/* Latencies in struct dectnrp_device_capabilities order. */
	zassert_equal(latency[0], 25920);
	zassert_equal(latency[1], 0);
	zassert_equal(latency[2], 24192);
	zassert_equal(latency[3], 12441);
	zassert_equal(latency[4], 13824);
	zassert_equal(latency[5], 29030);
	zassert_equal(latency[6], 7603);
	zassert_equal(latency[7], 14169);
}

ZTEST(spinel_core, test_unknown_prop)
{
	send_req(1, SPINEL_CMD_PROP_VALUE_GET, 0x3DFF, "");
	expect_status(1, SPINEL_STATUS_PROP_NOT_FOUND);
}

ZTEST(spinel_core, test_set_read_only_prop)
{
	send_req(5, SPINEL_CMD_PROP_VALUE_SET, DECTNRP_SPINEL_PROP_DRV_CAPS, "");
	expect_status(5, SPINEL_STATUS_INVALID_COMMAND_FOR_PROP);

	/* Device to host only. */
	send_req(6, SPINEL_CMD_PROP_VALUE_SET, DECTNRP_SPINEL_PROP_DRV_EVENT, "");
	expect_status(6, SPINEL_STATUS_INVALID_COMMAND_FOR_PROP);

	/* Unknown stays unknown. */
	send_req(7, SPINEL_CMD_PROP_VALUE_SET, 0x3DFF, "");
	expect_status(7, SPINEL_STATUS_PROP_NOT_FOUND);
}

ZTEST(spinel_core, test_unknown_cmd)
{
	static const uint8_t frame[] = {SPINEL_HEADER_FLAG | 2, 0x7F};

	send_raw(frame, sizeof(frame));
	expect_status(2, SPINEL_STATUS_INVALID_COMMAND);
}

ZTEST(spinel_core, test_truncated_set)
{
	/* DRV_NETWORK_ID is 4 bytes: send only 2. */
	send_req(3, SPINEL_CMD_PROP_VALUE_SET, DECTNRP_SPINEL_PROP_DRV_NETWORK_ID, "S", 0x5279);
	expect_status(3, SPINEL_STATUS_PARSE_ERROR);
}

ZTEST(spinel_core, test_bad_header_ignored)
{
	/* FLG = 0b01: not a host request. */
	static const uint8_t frame[] = {0x45, SPINEL_CMD_NOOP};
	uint32_t rejected_before;
	uint32_t rejected_after;

	rejected_before = read_stat(SPINEL_STAT_FRAMES_REJECTED);
	send_raw(frame, sizeof(frame));
	expect_no_frame(K_MSEC(50));

	rejected_after = read_stat(SPINEL_STAT_FRAMES_REJECTED);
	zassert_equal(rejected_after, rejected_before + 1);
}

ZTEST(spinel_core, test_driver_errno_status)
{
	struct mock_driver *mock = mock_driver_get();

	mock->start_ret = -EIO;
	send_req(4, SPINEL_CMD_PROP_VALUE_SET, DECTNRP_SPINEL_PROP_DRV_ENABLED, "b", true);
	expect_status(4, 0x3C80 + EIO);
}

ZTEST_SUITE(spinel_core, NULL, setup, before, NULL, NULL);

/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/net/dectnrp_spinel.h>

#include <spinel.h>

ZTEST(spinel_codec, test_pack_unpack_op_prefix)
{
	static const uint8_t expected[] = {0x34, 0x12, 0x01, 0x8d, 0x06, 0x01, 0x08,
					   0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
	uint8_t buf[32];
	uint16_t token, channel;
	uint8_t type, flags;
	uint64_t start;
	spinel_ssize_t len;

	len = spinel_datatype_pack(buf, sizeof(buf), DECTNRP_SPINEL_FMT_OP_PREFIX, 0x1234, 1, 1677,
				   0x01, 0x0102030405060708ULL);
	zassert_equal(len, sizeof(expected));
	zassert_mem_equal(buf, expected, sizeof(expected));

	len = spinel_datatype_unpack(buf, len, DECTNRP_SPINEL_FMT_OP_PREFIX, &token, &type,
				     &channel, &flags, &start);
	zassert_equal(len, sizeof(expected));
	zassert_equal(token, 0x1234);
	zassert_equal(type, DECTNRP_SPINEL_OP_RX);
	zassert_equal(channel, 1677);
	zassert_equal(flags, DECTNRP_SPINEL_OP_FLAG_SCHEDULED);
	zassert_equal(start, 0x0102030405060708ULL);
}

ZTEST(spinel_codec, test_header_byte)
{
	zassert_equal(SPINEL_HEADER_FLAG | 5, 0x85);
}

ZTEST(spinel_codec, test_prop_range)
{
	static const unsigned int props[] = {
		DECTNRP_SPINEL_PROP_DRV_ENABLED,
		DECTNRP_SPINEL_PROP_DRV_CAPS,
		DECTNRP_SPINEL_PROP_DRV_TIME,
		DECTNRP_SPINEL_PROP_DRV_NETWORK_ID,
		DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_NWID,
		DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_RXADDR,
		DECTNRP_SPINEL_PROP_DRV_OP_QUEUE,
		DECTNRP_SPINEL_PROP_DRV_EVENT,
		DECTNRP_SPINEL_PROP_DRV_STATS,
	};

	zassert_true((unsigned int)DECTNRP_SPINEL_PROP_DRV_ENABLED >= SPINEL_PROP_VENDOR__BEGIN);
	for (size_t i = 0; i < ARRAY_SIZE(props); i++) {
		zassert_true(props[i] < SPINEL_PROP_VENDOR__END, "prop 0x%x", props[i]);
	}
}

ZTEST_SUITE(spinel_codec, NULL, NULL, NULL, NULL, NULL);

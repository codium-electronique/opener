/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "hdlc.h"

#define DEC_SIZE 300

static uint8_t dec_buf[DEC_SIZE];
static struct spinel_hdlc_decoder dec;
static uint8_t enc[SPINEL_HDLC_MAX_ENCODED(DEC_SIZE + 10)];

/* Feed bytes to the decoder; return the first non-zero result, or 0. */
static int feed(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		int ret = spinel_hdlc_decode_byte(&dec, data[i]);

		if (ret != 0) {
			zassert_equal(i, len - 1, "result before the end of the input");
			return ret;
		}
	}

	return 0;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	spinel_hdlc_decoder_init(&dec, dec_buf, sizeof(dec_buf));
}

ZTEST(spinel_hdlc, test_fcs_check_value)
{
	/* CRC-16/X-25 check value of "123456789" is 0x906E, sent LSB first. */
	static const uint8_t check[] = "123456789";
	size_t len = spinel_hdlc_encode(check, 9, enc, sizeof(enc));

	zassert_equal(len, 1 + 9 + 2 + 1);
	zassert_equal(enc[10], 0x6E);
	zassert_equal(enc[11], 0x90);
	zassert_equal(enc[12], 0x7E);
}

ZTEST(spinel_hdlc, test_encode_escapes)
{
	static const uint8_t frame[] = {0x7E, 0x11, 0x42};
	/* FCS = 0xBB50: neither byte needs escaping. */
	static const uint8_t expected[] = {0x7E, 0x7D, 0x5E, 0x7D, 0x31, 0x42, 0x50, 0xBB, 0x7E};
	size_t len = spinel_hdlc_encode(frame, sizeof(frame), enc, sizeof(enc));

	zassert_equal(len, sizeof(expected));
	zassert_mem_equal(enc, expected, sizeof(expected));

	/* One byte short of the encoded length. */
	zassert_equal(spinel_hdlc_encode(frame, sizeof(frame), enc, sizeof(expected) - 1), 0);
}

ZTEST(spinel_hdlc, test_roundtrip_all_bytes)
{
	uint8_t frame[256];
	size_t len;

	for (size_t i = 0; i < sizeof(frame); i++) {
		frame[i] = i;
	}

	len = spinel_hdlc_encode(frame, sizeof(frame), enc, sizeof(enc));
	zassert_true(len > sizeof(frame) + 4);
	zassert_true(len <= SPINEL_HDLC_MAX_ENCODED(sizeof(frame)));

	zassert_equal(feed(enc, len), sizeof(frame));
	zassert_mem_equal(dec_buf, frame, sizeof(frame));
}

ZTEST(spinel_hdlc, test_bad_fcs)
{
	static const uint8_t frame[] = {0x80, 0x02, 0x01, 0x02, 0x03};
	size_t len = spinel_hdlc_encode(frame, sizeof(frame), enc, sizeof(enc));

	enc[3] ^= 0x01;
	zassert_equal(feed(enc, len), -EBADMSG);

	enc[3] ^= 0x01;
	zassert_equal(feed(enc, len), sizeof(frame));
	zassert_mem_equal(dec_buf, frame, sizeof(frame));
}

ZTEST(spinel_hdlc, test_garbage_then_frame)
{
	static const uint8_t garbage[] = {0x00, 0x13, 0xA5, 0x7D, 0xFF, 0x42, 0x11, 0x5E, 0xF8};
	static const uint8_t frame[] = {0x81, 0x03, 0x00};
	size_t len = spinel_hdlc_encode(frame, sizeof(frame), enc, sizeof(enc));

	zassert_equal(feed(garbage, sizeof(garbage)), 0);
	/* The frame's leading flag closes the garbage, which is dropped as a bad frame. */
	zassert_equal(spinel_hdlc_decode_byte(&dec, enc[0]), -EBADMSG);
	zassert_equal(feed(&enc[1], len - 1), sizeof(frame));
	zassert_mem_equal(dec_buf, frame, sizeof(frame));
}

ZTEST(spinel_hdlc, test_overflow_resync)
{
	static uint8_t big[DEC_SIZE + 10];
	static const uint8_t frame[] = {0x80, 0x01, 0x00};
	size_t len;
	int errors = 0;

	memset(big, 0x55, sizeof(big));
	len = spinel_hdlc_encode(big, sizeof(big), enc, sizeof(enc));
	zassert_true(len > 0);

	for (size_t i = 0; i < len; i++) {
		int ret = spinel_hdlc_decode_byte(&dec, enc[i]);

		zassert_true(ret == 0 || ret == -EMSGSIZE, "unexpected %d", ret);
		errors += (ret == -EMSGSIZE);
	}
	zassert_equal(errors, 1);

	len = spinel_hdlc_encode(frame, sizeof(frame), enc, sizeof(enc));
	zassert_equal(feed(enc, len), sizeof(frame));
	zassert_mem_equal(dec_buf, frame, sizeof(frame));
}

ZTEST(spinel_hdlc, test_empty_frames_ignored)
{
	static const uint8_t flags[] = {0x7E, 0x7E, 0x7E};

	zassert_equal(feed(flags, sizeof(flags)), 0);
}

ZTEST_SUITE(spinel_hdlc, NULL, NULL, before, NULL, NULL);

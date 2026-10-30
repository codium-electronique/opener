/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "spi_link.h"

/*
 * The byte vectors are shared with the host's tests/spinel/test_spi_frame.py in linux-tools:
 * keep them identical.
 */

#define BUF_SIZE  700
#define RECV_LEN  1300
#define LONG_LEN  600
#define CLOCK_LEN 128

static struct spinel_spi_link link;
static uint8_t tx_buf[BUF_SIZE];
static uint8_t rx_buf[BUF_SIZE];
static uint8_t frame[LONG_LEN];
static struct spinel_spi_result res;

/* The host's header with nothing to send, accepting 256 bytes. */
static const uint8_t host_empty[] = {0x02, 0x00, 0x01, 0x00, 0x00};

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	spinel_spi_link_init(&link, RECV_LEN);
	memset(tx_buf, 0, sizeof(tx_buf));
	memset(rx_buf, 0, sizeof(rx_buf));
	memset(&res, 0, sizeof(res));
	for (size_t i = 0; i < sizeof(frame); i++) {
		frame[i] = (uint8_t)i;
	}
}

static void complete_with(const uint8_t *host, size_t host_len, size_t clocked)
{
	memcpy(rx_buf, host, host_len);
	spinel_spi_link_complete(&link, rx_buf, clocked, &res);
}

ZTEST(spinel_spi_link, test_arm_header)
{
	static const uint8_t header[] = {0x02, 0x14, 0x05, 0x07, 0x00};

	link.reset_pending = false;
	size_t armed = spinel_spi_link_arm(&link, frame, 7, tx_buf, sizeof(tx_buf));

	zassert_equal(armed, 12);
	zassert_mem_equal(tx_buf, header, sizeof(header));
	zassert_mem_equal(&tx_buf[SPINEL_SPI_HEADER_LEN], frame, 7);
	zassert_equal(link.armed_len, 7);
}

ZTEST(spinel_spi_link, test_arm_reset_flag)
{
	size_t armed = spinel_spi_link_arm(&link, NULL, 0, tx_buf, sizeof(tx_buf));

	zassert_equal(armed, SPINEL_SPI_HEADER_LEN);
	zassert_equal(tx_buf[0], 0x82);

	complete_with(host_empty, sizeof(host_empty), SPINEL_SPI_HEADER_LEN);
	zassert_false(link.reset_pending);

	armed = spinel_spi_link_arm(&link, NULL, 0, tx_buf, sizeof(tx_buf));
	zassert_equal(armed, SPINEL_SPI_HEADER_LEN);
	zassert_equal(tx_buf[0], 0x02);
}

ZTEST(spinel_spi_link, test_arm_empty)
{
	static const uint8_t header[] = {0x02, 0x14, 0x05, 0x00, 0x00};

	link.reset_pending = false;
	size_t armed = spinel_spi_link_arm(&link, NULL, 0, tx_buf, sizeof(tx_buf));

	zassert_equal(armed, SPINEL_SPI_HEADER_LEN);
	zassert_mem_equal(tx_buf, header, sizeof(header));
	zassert_equal(link.armed_len, 0);
}

ZTEST(spinel_spi_link, test_arm_too_small)
{
	size_t armed = spinel_spi_link_arm(&link, frame, 7, tx_buf, 8);

	zassert_equal(armed, 0);
}

ZTEST(spinel_spi_link, test_complete_rx_frame)
{
	static const uint8_t host[] = {0x02, 0x00, 0x01, 0x03, 0x00, 0xAA, 0xBB, 0xCC};

	complete_with(host, sizeof(host), sizeof(host));

	zassert_equal(res.err, 0);
	zassert_equal(res.rx_len, 3);
	zassert_equal_ptr(res.rx_frame, &rx_buf[SPINEL_SPI_HEADER_LEN]);
	zassert_false(res.sent);
}

ZTEST(spinel_spi_link, test_complete_sent)
{
	size_t armed = spinel_spi_link_arm(&link, frame, 7, tx_buf, sizeof(tx_buf));

	zassert_equal(armed, 12);
	complete_with(host_empty, sizeof(host_empty), 12);
	zassert_true(res.sent);
	zassert_equal(res.err, 0);

	armed = spinel_spi_link_arm(&link, frame, 7, tx_buf, sizeof(tx_buf));
	zassert_equal(armed, 12);
	complete_with(host_empty, sizeof(host_empty), CLOCK_LEN);
	zassert_true(res.sent);
}

ZTEST(spinel_spi_link, test_complete_short_clock_not_sent)
{
	size_t armed = spinel_spi_link_arm(&link, frame, LONG_LEN, tx_buf, sizeof(tx_buf));

	zassert_equal(armed, SPINEL_SPI_HEADER_LEN + LONG_LEN);
	complete_with(host_empty, sizeof(host_empty), SPINEL_SPI_HEADER_LEN + CLOCK_LEN);

	zassert_false(res.sent);
	zassert_equal(res.err, 0);
	zassert_equal(res.rx_len, 0);
	zassert_equal(link.armed_len, LONG_LEN);
}

ZTEST(spinel_spi_link, test_complete_bad_pattern)
{
	static const uint8_t ones[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	static const uint8_t zeros[] = {0x00, 0x00, 0x00, 0x00, 0x00};

	complete_with(ones, sizeof(ones), sizeof(ones));
	zassert_equal(res.err, -EBADMSG);
	zassert_equal(res.rx_len, 0);

	complete_with(zeros, sizeof(zeros), sizeof(zeros));
	zassert_equal(res.err, -EBADMSG);
	zassert_equal(res.rx_len, 0);
}

ZTEST(spinel_spi_link, test_complete_crc_flag)
{
	static const uint8_t host[] = {0x42, 0x00, 0x01, 0x00, 0x00};

	complete_with(host, sizeof(host), sizeof(host));

	zassert_equal(res.err, -EBADMSG);
	zassert_equal(res.rx_len, 0);
}

ZTEST(spinel_spi_link, test_complete_too_long)
{
	static const uint8_t host[] = {0x02, 0x00, 0x01, 0x11, 0x00};

	spinel_spi_link_init(&link, 16);
	complete_with(host, sizeof(host), SPINEL_SPI_HEADER_LEN + 17);

	zassert_equal(res.err, -EMSGSIZE);
	zassert_equal(res.rx_len, 0);
}

ZTEST(spinel_spi_link, test_complete_truncated)
{
	static const uint8_t host[] = {0x02, 0x00, 0x01, 0x0A, 0x00};

	complete_with(host, sizeof(host), 12);

	zassert_equal(res.err, -EBADMSG);
	zassert_equal(res.rx_len, 0);
}

ZTEST(spinel_spi_link, test_complete_no_header)
{
	size_t armed = spinel_spi_link_arm(&link, frame, 7, tx_buf, sizeof(tx_buf));

	zassert_equal(armed, 12);
	complete_with(host_empty, sizeof(host_empty), 3);

	zassert_equal(res.err, -EBADMSG);
	zassert_false(res.sent);
	zassert_true(link.reset_pending);
}

ZTEST_SUITE(spinel_spi_link, NULL, NULL, before, NULL, NULL);

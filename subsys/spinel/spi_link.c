/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#include "spi_link.h"

#define HDR_OFFSET      0
#define RECV_LEN_OFFSET 1
#define DATA_LEN_OFFSET 3

void spinel_spi_link_init(struct spinel_spi_link *link, uint16_t recv_len)
{
	link->reset_pending = true;
	link->recv_len = recv_len;
	link->armed_len = 0;
}

size_t spinel_spi_link_arm(struct spinel_spi_link *link, const uint8_t *frame, size_t len,
			   uint8_t *tx_buf, size_t tx_size)
{
	size_t total = SPINEL_SPI_HEADER_LEN + len;

	if (total > tx_size || len > UINT16_MAX) {
		return 0;
	}

	uint8_t hdr = SPINEL_SPI_PATTERN;

	if (link->reset_pending) {
		hdr |= SPINEL_SPI_FLAG_RST;
	}

	tx_buf[HDR_OFFSET] = hdr;
	sys_put_le16(link->recv_len, &tx_buf[RECV_LEN_OFFSET]);
	sys_put_le16((uint16_t)len, &tx_buf[DATA_LEN_OFFSET]);
	if (len > 0) {
		memcpy(&tx_buf[SPINEL_SPI_HEADER_LEN], frame, len);
	}
	link->armed_len = len;

	return total;
}

static int parse_host_frame(const struct spinel_spi_link *link, const uint8_t *rx_buf,
			    size_t clocked, size_t *data_len)
{
	uint8_t hdr = rx_buf[HDR_OFFSET];
	uint8_t pattern = hdr & SPINEL_SPI_PATTERN_MASK;

	if (pattern != SPINEL_SPI_PATTERN) {
		return -EBADMSG;
	}
	if ((hdr & SPINEL_SPI_FLAG_CRC) != 0) {
		return -EBADMSG;
	}

	size_t len = sys_get_le16(&rx_buf[DATA_LEN_OFFSET]);

	if (len > link->recv_len) {
		return -EMSGSIZE;
	}
	if (SPINEL_SPI_HEADER_LEN + len > clocked) {
		return -EBADMSG;
	}

	*data_len = len;
	return 0;
}

void spinel_spi_link_complete(struct spinel_spi_link *link, const uint8_t *rx_buf, size_t clocked,
			      struct spinel_spi_result *res)
{
	res->sent = false;
	res->rx_frame = NULL;
	res->rx_len = 0;
	res->err = 0;

	if (clocked < SPINEL_SPI_HEADER_LEN) {
		res->err = -EBADMSG;
		return;
	}

	/* The host clocked our whole header: it has seen RST. */
	link->reset_pending = false;

	if (link->armed_len > 0 && clocked >= SPINEL_SPI_HEADER_LEN + link->armed_len) {
		res->sent = true;
		link->armed_len = 0;
	}

	size_t data_len = 0;
	int err = parse_host_frame(link, rx_buf, clocked, &data_len);

	if (err != 0) {
		res->err = err;
		return;
	}
	if (data_len > 0) {
		res->rx_frame = &rx_buf[SPINEL_SPI_HEADER_LEN];
		res->rx_len = data_len;
	}
}

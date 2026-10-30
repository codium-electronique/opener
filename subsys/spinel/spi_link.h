/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Spinel SPI framing, used by the SPI transport.
 *
 * Each SPI transaction starts with a 5-byte header in both directions, followed by the raw
 * Spinel frame, without HDLC (OpenThread spi_frame.hpp):
 *
 *     HDR (u8) | RECV_LEN (u16 LE) | DATA_LEN (u16 LE) | frame
 *
 * HDR carries the RST, CRC and CCF flags and the pattern 0b10 in bits 1..0. RECV_LEN is the
 * frame length the sender can accept in this transaction, DATA_LEN the length of the frame it
 * sends, 0 for none. CRC is not supported: a header with the CRC flag is invalid.
 *
 * A frame counts as sent only if the transaction clocked its whole length. The device sets RST
 * in its headers until the first transaction of at least a header after boot.
 */

#ifndef DECTNRP_SPINEL_SPI_LINK_H_
#define DECTNRP_SPINEL_SPI_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

/** Length of the SPI header. */
#define SPINEL_SPI_HEADER_LEN 5

/** HDR flag: the sender has reset since its previous transaction. */
#define SPINEL_SPI_FLAG_RST     BIT(7)
/** HDR flag: the frame is followed by a CRC (not supported). */
#define SPINEL_SPI_FLAG_CRC     BIT(6)
/** HDR flag: the sender checks CRCs (not supported). */
#define SPINEL_SPI_FLAG_CCF     BIT(5)
/** HDR pattern, in the bits of @ref SPINEL_SPI_PATTERN_MASK. */
#define SPINEL_SPI_PATTERN      0x02
/** HDR bits holding @ref SPINEL_SPI_PATTERN. */
#define SPINEL_SPI_PATTERN_MASK 0x03

/** Device side state of the SPI link. */
struct spinel_spi_link {
	/** No transaction of at least a header since init: set RST. */
	bool reset_pending;
	/** RECV_LEN advertised in the headers: the largest frame accepted from the host. */
	uint16_t recv_len;
	/** Length of the frame in the armed buffer, 0 for none. */
	size_t armed_len;
};

/** Outcome of one transaction. */
struct spinel_spi_result {
	/** The armed frame was clocked out whole: it is sent. */
	bool sent;
	/** The host's frame, inside the receive buffer. Valid if @ref rx_len is not 0. */
	const uint8_t *rx_frame;
	/** Length of the host's frame, 0 for none. */
	size_t rx_len;
	/** 0, or -EBADMSG for an invalid or truncated header or frame, -EMSGSIZE for a host
	 *  frame longer than the advertised RECV_LEN.
	 */
	int err;
};

/**
 * @brief Initialize the link state, with RST pending.
 *
 * @param link Link state.
 * @param recv_len Largest frame accepted from the host.
 */
void spinel_spi_link_init(struct spinel_spi_link *link, uint16_t recv_len);

/**
 * @brief Write the header and a frame to transmit in the next transaction.
 *
 * @param link Link state.
 * @param frame Frame to send, NULL if @p len is 0.
 * @param len Length of @p frame, 0 for none.
 * @param tx_buf Transmit buffer.
 * @param tx_size Size of @p tx_buf.
 *
 * @return Number of bytes to arm (header and frame), or 0 if @p tx_buf is too small.
 */
size_t spinel_spi_link_arm(struct spinel_spi_link *link, const uint8_t *frame, size_t len,
			   uint8_t *tx_buf, size_t tx_size);

/**
 * @brief Process a finished transaction.
 *
 * @param link Link state.
 * @param rx_buf Receive buffer of the transaction.
 * @param clocked Number of bytes the host clocked (received in @p rx_buf).
 * @param res Outcome of the transaction.
 */
void spinel_spi_link_complete(struct spinel_spi_link *link, const uint8_t *rx_buf, size_t clocked,
			      struct spinel_spi_result *res);

#endif /* DECTNRP_SPINEL_SPI_LINK_H_ */

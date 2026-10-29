/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Spinel HDLC-lite framing, used by the UART transport.
 *
 * Frames are delimited by 0x7E. The bytes 0x7E, 0x7D, 0x11, 0x13 and 0xF8 are sent as 0x7D
 * followed by the byte XOR 0x20. Each frame ends with its CRC-16/X-25 FCS, least significant
 * byte first.
 */

#ifndef DECTNRP_SPINEL_HDLC_H_
#define DECTNRP_SPINEL_HDLC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Worst-case encoded size of a @p len byte frame: everything escaped, plus the two flags. */
#define SPINEL_HDLC_MAX_ENCODED(len) (2 * ((len) + 2) + 2)

/** Byte-wise HDLC-lite decoder state. */
struct spinel_hdlc_decoder {
	/** Frame buffer, holds the frame and its FCS while decoding. */
	uint8_t *buf;
	/** Size of @ref buf. */
	size_t size;
	/** Bytes written to @ref buf for the current frame. */
	size_t len;
	/** The previous byte was the escape byte. */
	bool escaped;
	/** The current frame did not fit: drop bytes until the next flag. */
	bool overflow;
	/** Running FCS of the bytes in @ref buf. */
	uint16_t fcs;
};

/**
 * @brief Initialize a decoder.
 *
 * @param d Decoder.
 * @param buf Frame buffer. It must hold the largest frame plus its 2-byte FCS.
 * @param size Size of @p buf.
 */
void spinel_hdlc_decoder_init(struct spinel_hdlc_decoder *d, uint8_t *buf, size_t size);

/**
 * @brief Feed one received byte to a decoder.
 *
 * @param d Decoder.
 * @param byte Received byte.
 *
 * @retval >0 A frame of this length (FCS stripped) is complete in @c d->buf. It stays valid
 *            until the next call.
 * @retval 0 More bytes are needed.
 * @retval -EBADMSG A frame was dropped because of a bad FCS.
 * @retval -EMSGSIZE A frame was dropped because it did not fit in the buffer. The decoder
 *                   resynchronizes on the next flag.
 */
int spinel_hdlc_decode_byte(struct spinel_hdlc_decoder *d, uint8_t byte);

/**
 * @brief Encode a frame.
 *
 * @param frame Spinel frame.
 * @param len Length of @p frame.
 * @param out Output buffer. @ref SPINEL_HDLC_MAX_ENCODED gives a size that always fits.
 * @param out_size Size of @p out.
 *
 * @return Encoded length, including the leading and trailing flags, or 0 if @p out is too
 *         small.
 */
size_t spinel_hdlc_encode(const uint8_t *frame, size_t len, uint8_t *out, size_t out_size);

#endif /* DECTNRP_SPINEL_HDLC_H_ */

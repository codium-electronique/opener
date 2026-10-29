/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/sys/crc.h>

#include "hdlc.h"

#define HDLC_FLAG       0x7E
#define HDLC_ESCAPE     0x7D
#define HDLC_ESCAPE_XOR 0x20
#define HDLC_FCS_SIZE   2

/* Other bytes Spinel HDLC-lite escapes: software flow control and vendor. */
#define HDLC_XON    0x11
#define HDLC_XOFF   0x13
#define HDLC_VENDOR 0xF8

/* CRC-16/X-25: reflected CCITT polynomial, initial value and final XOR 0xFFFF. */
#define HDLC_FCS_INIT 0xFFFF
/* FCS computed over a frame followed by its own (complemented, LSB first) FCS. */
#define HDLC_FCS_GOOD 0xF0B8

static bool needs_escape(uint8_t byte)
{
	switch (byte) {
	case HDLC_FLAG:
	case HDLC_ESCAPE:
	case HDLC_XON:
	case HDLC_XOFF:
	case HDLC_VENDOR:
		return true;
	default:
		return false;
	}
}

static void decoder_reset(struct spinel_hdlc_decoder *d)
{
	d->len = 0;
	d->escaped = false;
	d->overflow = false;
	d->fcs = HDLC_FCS_INIT;
}

void spinel_hdlc_decoder_init(struct spinel_hdlc_decoder *d, uint8_t *buf, size_t size)
{
	d->buf = buf;
	d->size = size;
	decoder_reset(d);
}

int spinel_hdlc_decode_byte(struct spinel_hdlc_decoder *d, uint8_t byte)
{
	int ret = 0;

	if (byte == HDLC_FLAG) {
		if (d->overflow) {
			ret = -EMSGSIZE;
		} else if (d->len == 0) {
			/* Back-to-back flags: empty frame, ignored. */
		} else if (d->len < HDLC_FCS_SIZE || d->fcs != HDLC_FCS_GOOD) {
			ret = -EBADMSG;
		} else {
			ret = d->len - HDLC_FCS_SIZE;
		}

		decoder_reset(d);
		return ret;
	}

	if (d->overflow) {
		return 0;
	}

	if (byte == HDLC_ESCAPE) {
		d->escaped = true;
		return 0;
	}

	if (d->escaped) {
		byte ^= HDLC_ESCAPE_XOR;
		d->escaped = false;
	}

	if (d->len == d->size) {
		d->overflow = true;
		return 0;
	}

	d->buf[d->len] = byte;
	d->len++;
	d->fcs = crc16_ccitt(d->fcs, &byte, 1);

	return 0;
}

/* Append @p byte to @p out at @p pos, escaped if needed; false if it does not fit. */
static bool put_byte(uint8_t byte, uint8_t *out, size_t out_size, size_t *pos)
{
	size_t needed;

	if (needs_escape(byte)) {
		needed = 2;
	} else {
		needed = 1;
	}
	if (*pos + needed > out_size) {
		return false;
	}

	if (needs_escape(byte)) {
		out[*pos] = HDLC_ESCAPE;
		*pos += 1;
		byte ^= HDLC_ESCAPE_XOR;
	}
	out[*pos] = byte;
	*pos += 1;

	return true;
}

size_t spinel_hdlc_encode(const uint8_t *frame, size_t len, uint8_t *out, size_t out_size)
{
	uint16_t fcs;
	uint8_t fcs_low;
	uint8_t fcs_high;
	size_t pos = 0;

	/* Leading flag. */
	if (out_size < 1) {
		return 0;
	}
	out[pos] = HDLC_FLAG;
	pos++;

	for (size_t i = 0; i < len; i++) {
		if (!put_byte(frame[i], out, out_size, &pos)) {
			return 0;
		}
	}

	/* FCS, least significant byte first. */
	fcs = ~crc16_ccitt(HDLC_FCS_INIT, frame, len);
	fcs_low = fcs & 0xFF;
	fcs_high = fcs >> 8;
	if (!put_byte(fcs_low, out, out_size, &pos)) {
		return 0;
	}
	if (!put_byte(fcs_high, out, out_size, &pos)) {
		return 0;
	}

	/* Trailing flag. */
	if (pos + 1 > out_size) {
		return 0;
	}
	out[pos] = HDLC_FLAG;
	pos++;

	return pos;
}

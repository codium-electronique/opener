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

/* CRC-16/X-25: reflected CCITT polynomial, initial value and final XOR 0xFFFF. */
#define HDLC_FCS_INIT 0xFFFF
/* FCS computed over a frame followed by its own (complemented, LSB first) FCS. */
#define HDLC_FCS_GOOD 0xF0B8

static bool needs_escape(uint8_t byte)
{
	return byte == HDLC_FLAG || byte == HDLC_ESCAPE || byte == 0x11 || byte == 0x13 ||
	       byte == 0xF8;
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

	d->buf[d->len++] = byte;
	d->fcs = crc16_ccitt(d->fcs, &byte, 1);

	return 0;
}

static bool put_byte(uint8_t byte, uint8_t *out, size_t out_size, size_t *pos)
{
	if (needs_escape(byte)) {
		if (*pos + 2 > out_size) {
			return false;
		}
		out[(*pos)++] = HDLC_ESCAPE;
		byte ^= HDLC_ESCAPE_XOR;
	} else if (*pos + 1 > out_size) {
		return false;
	}

	out[(*pos)++] = byte;
	return true;
}

size_t spinel_hdlc_encode(const uint8_t *frame, size_t len, uint8_t *out, size_t out_size)
{
	uint16_t fcs;
	size_t pos = 0;

	if (out_size < 1) {
		return 0;
	}
	out[pos++] = HDLC_FLAG;

	for (size_t i = 0; i < len; i++) {
		if (!put_byte(frame[i], out, out_size, &pos)) {
			return 0;
		}
	}

	fcs = ~crc16_ccitt(HDLC_FCS_INIT, frame, len);
	if (!put_byte(fcs & 0xFF, out, out_size, &pos) ||
	    !put_byte(fcs >> 8, out, out_size, &pos) || pos + 1 > out_size) {
		return 0;
	}
	out[pos++] = HDLC_FLAG;

	return pos;
}

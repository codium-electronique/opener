/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * UART transport: Spinel frames in HDLC-lite framing, on the UART chosen as
 * opener,spinel-uart. Both directions are interrupt driven; received frames
 * are decoded in the ISR and handed to the core.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "hdlc.h"
#include "spinel_internal.h"

LOG_MODULE_DECLARE(dectnrp_spinel, CONFIG_DECTNRP_SPINEL_LOG_LEVEL);

/* Size of the FCS the decoder keeps after each frame. */
#define UART_FCS_SIZE 2

/* Bytes read from the UART FIFO at once. */
#define UART_RX_CHUNK_SIZE 16

/*
 * Time to send one encoded frame. A worst-case frame of the default 1300
 * bytes, fully escaped, takes about 230 ms at 115200 baud.
 */
#define UART_TX_TIMEOUT K_MSEC(1000)

static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(opener_spinel_uart));

static uint8_t rx_frame[CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE + UART_FCS_SIZE];
static struct spinel_hdlc_decoder decoder;

/* Encoded frame being sent; tx_pos is the next byte to hand to the FIFO. */
static uint8_t tx_buf[SPINEL_HDLC_MAX_ENCODED(CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE)];
static size_t tx_len;
static size_t tx_pos;
static K_SEM_DEFINE(tx_done, 0, 1);

static void rx_byte(uint8_t byte)
{
	int ret;

	ret = spinel_hdlc_decode_byte(&decoder, byte);
	if (ret > 0) {
		spinel_rx_submit(decoder.buf, ret);
	} else if (ret == -EBADMSG || ret == -EMSGSIZE) {
		spinel_stat_inc(SPINEL_STAT_FRAMES_REJECTED);
	}
}

static void isr_rx(const struct device *dev)
{
	uint8_t chunk[UART_RX_CHUNK_SIZE];
	int n;

	n = uart_fifo_read(dev, chunk, sizeof(chunk));
	while (n > 0) {
		for (int i = 0; i < n; i++) {
			rx_byte(chunk[i]);
		}
		n = uart_fifo_read(dev, chunk, sizeof(chunk));
	}
}

static void isr_tx(const struct device *dev)
{
	size_t remaining;
	int n;

	remaining = tx_len - tx_pos;
	if (remaining == 0) {
		uart_irq_tx_disable(dev);
		k_sem_give(&tx_done);
		return;
	}

	n = uart_fifo_fill(dev, &tx_buf[tx_pos], remaining);
	if (n > 0) {
		tx_pos += n;
	}
}

static void uart_isr(const struct device *dev, void *user_data)
{
	int rx_ready;
	int tx_ready;

	ARG_UNUSED(user_data);

	uart_irq_update(dev);

	rx_ready = uart_irq_rx_ready(dev);
	if (rx_ready > 0) {
		isr_rx(dev);
	}

	tx_ready = uart_irq_tx_ready(dev);
	if (tx_ready > 0) {
		isr_tx(dev);
	}
}

int spinel_transport_init(void)
{
	bool ready;
	int ret;

	ready = device_is_ready(uart_dev);
	if (!ready) {
		LOG_ERR("UART %s not ready", uart_dev->name);
		return -ENODEV;
	}

	spinel_hdlc_decoder_init(&decoder, rx_frame, sizeof(rx_frame));

	ret = uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
	if (ret != 0) {
		LOG_ERR("UART %s has no interrupt-driven API (%d)", uart_dev->name, ret);
		return ret;
	}
	uart_irq_rx_enable(uart_dev);

	return 0;
}

int spinel_transport_send(const uint8_t *frame, size_t len)
{
	size_t encoded_len;
	int ret;

	encoded_len = spinel_hdlc_encode(frame, len, tx_buf, sizeof(tx_buf));
	if (encoded_len == 0) {
		return -EMSGSIZE;
	}

	tx_len = encoded_len;
	tx_pos = 0;
	k_sem_reset(&tx_done);
	uart_irq_tx_enable(uart_dev);

	ret = k_sem_take(&tx_done, UART_TX_TIMEOUT);
	if (ret != 0) {
		uart_irq_tx_disable(uart_dev);
		LOG_WRN("UART TX timeout, %zu of %zu bytes sent", tx_pos, tx_len);
		return -EIO;
	}

	return 0;
}

/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * SPI transport: Spinel frames in the Spinel SPI framing (spi_link.h), the
 * device being the SPI slave of the first enabled opener,spinel-spi node.
 *
 * The device raises IRQ while it has a frame armed for the host, and holds
 * BUSY while the SPI slave is not armed. A thread arms one transaction at a
 * time, with the frame of spinel_transport_send() or none, and sets IRQ
 * before it releases BUSY: a host that waits for BUSY inactive reads a
 * current IRQ. The transaction callback sets BUSY again.
 *
 * A host frame is received only into a buffer reserved before arming: with
 * none free, the header advertises RECV_LEN 0 and the host sends it again.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>

#include "spi_handoff.h"
#include "spi_link.h"
#include "spinel_internal.h"

LOG_MODULE_DECLARE(dectnrp_spinel, CONFIG_DECTNRP_SPINEL_LOG_LEVEL);

#define SPINEL_SPI_NODE     DT_COMPAT_GET_ANY_STATUS_OKAY(opener_spinel_spi)
#define SPINEL_SPI_OP       (SPI_OP_MODE_SLAVE | SPI_WORD_SET(8) | SPI_TRANSFER_MSB)
#define SPINEL_SPI_BUF_SIZE (SPINEL_SPI_HEADER_LEN + CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE)

/* Bits of thread_events. */
#define EVENT_XFER_DONE 0
#define EVENT_FRAME_NEW 1

static const struct spi_dt_spec spi = SPI_DT_SPEC_GET(SPINEL_SPI_NODE, SPINEL_SPI_OP);
static const struct gpio_dt_spec irq_gpio = GPIO_DT_SPEC_GET(SPINEL_SPI_NODE, irq_gpios);
static const struct gpio_dt_spec busy_gpio = GPIO_DT_SPEC_GET(SPINEL_SPI_NODE, busy_gpios);

static struct spinel_spi_link link;
static uint8_t tx_buf[SPINEL_SPI_BUF_SIZE];
static uint8_t rx_buf[SPINEL_SPI_BUF_SIZE];
/* Core buffer for the next host frame, NULL if none was free. */
static struct net_buf *rx_reserved;

/* Received length of the last transaction, or a negative errno. */
static int xfer_result;
static atomic_t thread_events;
static K_SEM_DEFINE(thread_wake, 0, 1);

/*
 * Hand-off of the frame of spinel_transport_send() to this thread. Arming copies the frame
 * and completing it give the sender its result, both under the hand-off's lock, so that a
 * sender that returned is never read, and a transaction finishing after its sender timed out
 * does not complete the next sender's frame (the frames are numbered).
 */
static struct spinel_spi_handoff handoff;

static K_THREAD_STACK_DEFINE(spi_stack, CONFIG_DECTNRP_SPINEL_SPI_STACK_SIZE);
static struct k_thread spi_thread;

/* Wake the SPI thread for a new frame. */
static void wake_thread(void)
{
	atomic_set_bit(&thread_events, EVENT_FRAME_NEW);
	k_sem_give(&thread_wake);
}

static void xfer_cb(const struct device *dev, int result, void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(data);

	/* The SPI slave is not armed until the thread arms it again. */
	gpio_pin_set_dt(&busy_gpio, 1);
	xfer_result = result;
	atomic_set_bit(&thread_events, EVENT_XFER_DONE);
	k_sem_give(&thread_wake);
}

/* Arm the pending frame, or none. @return sequence number of the armed frame. */
static uint32_t arm(void)
{
	uint32_t seq;
	size_t total;
	size_t frame_len;
	int ret;

	if (rx_reserved == NULL) {
		rx_reserved = spinel_rx_buf_alloc();
	}
	bool rx_room = rx_reserved != NULL;

	/* The frame is copied under the lock: its sender can return from now on. */
	ret = spinel_spi_handoff_arm(&handoff, &link, rx_room, tx_buf, sizeof(tx_buf), &total, &seq,
				     &frame_len);
	if (ret == -EMSGSIZE) {
		size_t max_len = sizeof(tx_buf) - SPINEL_SPI_HEADER_LEN;

		LOG_ERR("frame of %zu bytes too long for SPI (max %zu)", frame_len, max_len);
		spinel_stat_inc(SPINEL_STAT_TRANSPORT_ERRORS);
	}

	const struct spi_buf tx = {.buf = tx_buf, .len = total};
	const struct spi_buf rx = {.buf = rx_buf, .len = sizeof(rx_buf)};
	const struct spi_buf_set tx_set = {.buffers = &tx, .count = 1};
	const struct spi_buf_set rx_set = {.buffers = &rx, .count = 1};
	int err;

	err = spi_transceive_cb(spi.bus, &spi.config, &tx_set, &rx_set, xfer_cb, NULL);
	if (err != 0) {
		LOG_ERR("cannot arm the SPI slave (%d)", err);
		xfer_result = err;
		atomic_set_bit(&thread_events, EVENT_XFER_DONE);
		k_sem_give(&thread_wake);
		return seq;
	}

	bool frame_armed = link.armed_len > 0;

	gpio_pin_set_dt(&irq_gpio, frame_armed);
	gpio_pin_set_dt(&busy_gpio, 0);

	return seq;
}

static void wait_xfer(void)
{
	atomic_val_t events;

	while (true) {
		k_sem_take(&thread_wake, K_FOREVER);
		events = atomic_clear(&thread_events);
		if ((events & BIT(EVENT_FRAME_NEW)) != 0 && link.armed_len == 0) {
			/* Armed with no frame: the host clocks one transaction to
			 * see none, then the frame is armed.
			 */
			gpio_pin_set_dt(&irq_gpio, 1);
		}
		if ((events & BIT(EVENT_XFER_DONE)) != 0) {
			return;
		}
	}
}

static void complete(uint32_t seq)
{
	struct spinel_spi_result res;
	int clocked = xfer_result;

	if (clocked < 0) {
		spinel_stat_inc(SPINEL_STAT_TRANSPORT_ERRORS);
		/* Do not spin on a failing SPI slave. */
		k_sleep(K_MSEC(10));
		return;
	}

	spinel_spi_link_complete(&link, rx_buf, clocked, &res);
	if (res.sent) {
		spinel_spi_handoff_done(&handoff, seq);
	}
	if (res.rx_len > 0) {
		/* The link received a frame only if arm() advertised room. */
		struct net_buf *buf = rx_reserved;

		rx_reserved = NULL;
		net_buf_add_mem(buf, res.rx_frame, res.rx_len);
		spinel_rx_buf_submit(buf);
	}
	if (res.err != 0) {
		spinel_stat_inc(SPINEL_STAT_FRAMES_REJECTED);
	}
}

static void spi_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t seq;

	while (true) {
		atomic_clear_bit(&thread_events, EVENT_FRAME_NEW);
		seq = arm();
		wait_xfer();
		complete(seq);
	}
}

int spinel_transport_init(void)
{
	bool ready;
	int err;

	ready = spi_is_ready_dt(&spi);
	if (!ready) {
		LOG_ERR("SPI %s not ready", spi.bus->name);
		return -ENODEV;
	}
	ready = gpio_is_ready_dt(&irq_gpio);
	if (!ready) {
		return -ENODEV;
	}
	ready = gpio_is_ready_dt(&busy_gpio);
	if (!ready) {
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&busy_gpio, GPIO_OUTPUT_ACTIVE);
	if (err != 0) {
		return err;
	}
	err = gpio_pin_configure_dt(&irq_gpio, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		return err;
	}

	spinel_spi_handoff_init(&handoff, wake_thread);
	spinel_spi_link_init(&link, CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE);

	k_thread_create(&spi_thread, spi_stack, K_THREAD_STACK_SIZEOF(spi_stack), spi_thread_fn,
			NULL, NULL, NULL, CONFIG_DECTNRP_SPINEL_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&spi_thread, "spinel_spi");

	return 0;
}

int spinel_transport_send(const uint8_t *frame, size_t len)
{
	int ret;

	ret = spinel_spi_handoff_send(&handoff, frame, len,
				      K_MSEC(CONFIG_DECTNRP_SPINEL_SPI_TX_TIMEOUT_MS));
	if (ret == -ETIMEDOUT) {
		LOG_WRN("no host clocked a frame of %zu bytes in time, gave up", len);
	}

	return ret;
}

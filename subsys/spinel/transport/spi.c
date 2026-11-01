/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * SPI transport: Spinel frames in the Spinel SPI framing (spi_link.h), the
 * device being the SPI slave of the first enabled opener,spinel-spi node.
 *
 * The transport has no sender: one thread takes frames from the core's TX
 * queue (spinel_tx_get()) and owns each until a transaction clocks it whole
 * (spinel_tx_done()). It arms one transaction at a time, with the frame in
 * armed_buf or none. While armed empty and a frame is queued, it takes that
 * frame as next_buf and raises IRQ: the host's transaction shows no frame,
 * then next_buf is armed. arm() sets IRQ before it releases BUSY, so a host
 * that waits for BUSY inactive reads a current IRQ, and the transaction
 * callback sets BUSY again.
 *
 * After a host frame, BUSY is held until a reply is available, at most
 * CONFIG_DECTNRP_SPINEL_SPI_REPLY_WAIT_US: the reply then comes in the host's
 * next transaction, not after an arming one. The hold is skipped when a frame
 * is already offered or queued. IRQ is inactive during the hold until a frame
 * is taken. A transaction clocked during the hold finds the SPI slave not
 * armed and is ignored.
 *
 * A frame offered (armed, or taken as next_buf) and not sent within
 * CONFIG_DECTNRP_SPINEL_SPI_TX_TIMEOUT_MS of its offer makes the frames queued
 * behind it be dropped, so that the TX buffers do not run out without a
 * host. The offered frame stays. Only sending the frame restarts the count: a
 * transaction too short to carry it does not.
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

#include "spi_link.h"
#include "spinel_internal.h"

LOG_MODULE_DECLARE(dectnrp_spinel, CONFIG_DECTNRP_SPINEL_LOG_LEVEL);

#define SPINEL_SPI_NODE     DT_COMPAT_GET_ANY_STATUS_OKAY(opener_spinel_spi)
#define SPINEL_SPI_OP       (SPI_OP_MODE_SLAVE | SPI_WORD_SET(8) | SPI_TRANSFER_MSB)
#define SPINEL_SPI_BUF_SIZE (SPINEL_SPI_HEADER_LEN + CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE)

/* Events of the wait: the transaction semaphore, and the TX queue while no frame is offered. */
#define POLL_XFER 0
#define POLL_TX   1

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
static K_SEM_DEFINE(xfer_done, 0, 1);

/*
 * Frames owned by the SPI thread. armed_buf is the frame copied to tx_buf, NULL when the SPI
 * slave is armed empty. next_buf is taken from the TX queue while armed empty, and armed by the
 * next arm(); it is NULL whenever armed_buf is not.
 */
static struct net_buf *armed_buf;
static struct net_buf *next_buf;
/* When the TX timeout of the offered frame ends. Set when a frame is first offered. */
static k_timepoint_t offer_deadline;

/* A frame was taken from the TX queue and is offered to the host. */
static void offer_started(void)
{
	offer_deadline = sys_timepoint_calc(K_MSEC(CONFIG_DECTNRP_SPINEL_SPI_TX_TIMEOUT_MS));
}

static K_THREAD_STACK_DEFINE(spi_stack, CONFIG_DECTNRP_SPINEL_SPI_STACK_SIZE);
static struct k_thread spi_thread;

static void xfer_cb(const struct device *dev, int result, void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(data);

	/* The SPI slave is not armed until the thread arms it again. */
	gpio_pin_set_dt(&busy_gpio, 1);
	xfer_result = result;
	k_sem_give(&xfer_done);
}

/* Arm the transaction: the frame not yet sent, else next_buf, else the next queued frame. */
static void arm(void)
{
	size_t total;
	int err;

	if (rx_reserved == NULL) {
		rx_reserved = spinel_rx_buf_alloc();
	}
	bool rx_room = rx_reserved != NULL;

	if (armed_buf == NULL) {
		armed_buf = next_buf;
		next_buf = NULL;
	}
	if (armed_buf == NULL) {
		armed_buf = spinel_tx_get(K_NO_WAIT);
		if (armed_buf != NULL) {
			offer_started();
		}
	}
	if (armed_buf != NULL && armed_buf->len > sizeof(tx_buf) - SPINEL_SPI_HEADER_LEN) {
		size_t max_len = sizeof(tx_buf) - SPINEL_SPI_HEADER_LEN;

		LOG_ERR("frame of %u bytes too long for SPI (max %zu)", armed_buf->len, max_len);
		spinel_tx_done(armed_buf, -EMSGSIZE);
		armed_buf = NULL;
	}

	if (armed_buf != NULL) {
		total = spinel_spi_link_arm(&link, armed_buf->data, armed_buf->len, rx_room, tx_buf,
					    sizeof(tx_buf));
	} else {
		total = spinel_spi_link_arm(&link, NULL, 0, rx_room, tx_buf, sizeof(tx_buf));
	}

	const struct spi_buf tx = {.buf = tx_buf, .len = total};
	const struct spi_buf rx = {.buf = rx_buf, .len = sizeof(rx_buf)};
	const struct spi_buf_set tx_set = {.buffers = &tx, .count = 1};
	const struct spi_buf_set rx_set = {.buffers = &rx, .count = 1};

	err = spi_transceive_cb(spi.bus, &spi.config, &tx_set, &rx_set, xfer_cb, NULL);
	if (err != 0) {
		LOG_ERR("cannot arm the SPI slave (%d)", err);
		xfer_result = err;
		k_sem_give(&xfer_done);
		return;
	}

	bool frame_armed = armed_buf != NULL;

	gpio_pin_set_dt(&irq_gpio, frame_armed);
	gpio_pin_set_dt(&busy_gpio, 0);
}

/* Drop the frames queued behind the offered one, after a timeout without a transaction. */
static void drop_queued(void)
{
	struct net_buf *buf = spinel_tx_get(K_NO_WAIT);
	unsigned int dropped = 0;

	while (buf != NULL) {
		spinel_tx_done(buf, -ETIMEDOUT);
		dropped++;
		buf = spinel_tx_get(K_NO_WAIT);
	}
	if (dropped > 0) {
		LOG_WRN("no host clocked a frame in time, dropped %u queued frames", dropped);
	}
}

/* Wait for the transaction, and take a frame from the TX queue while armed empty. */
static void wait_xfer(void)
{
	struct k_poll_event events[2];
	k_timeout_t timeout = K_FOREVER;

	while (true) {
		bool offered = armed_buf != NULL || next_buf != NULL;

		if (offered) {
			timeout = sys_timepoint_timeout(offer_deadline);
		} else {
			timeout = K_FOREVER;
		}

		k_poll_event_init(&events[POLL_XFER], K_POLL_TYPE_SEM_AVAILABLE,
				  K_POLL_MODE_NOTIFY_ONLY, &xfer_done);
		uint32_t count = 1;

		if (!offered) {
			spinel_tx_poll_init(&events[POLL_TX]);
			count = 2;
		}

		int err = k_poll(events, count, timeout);

		if (err == -EAGAIN) {
			drop_queued();
			/* The offered frame stays: drop again if it is still not sent later. */
			offer_started();
			continue;
		}
		if (count == 2 && events[POLL_TX].state == K_POLL_STATE_FIFO_DATA_AVAILABLE) {
			/* Armed empty: the host clocks one transaction to see none, then the
			 * frame is armed.
			 */
			next_buf = spinel_tx_get(K_NO_WAIT);
			if (next_buf != NULL) {
				offer_started();
				gpio_pin_set_dt(&irq_gpio, 1);
			}
		}
		if (events[POLL_XFER].state == K_POLL_STATE_SEM_AVAILABLE) {
			k_sem_take(&xfer_done, K_NO_WAIT);
			return;
		}
	}
}

/* Returns true if the transaction delivered a host frame. */
static bool complete(void)
{
	struct spinel_spi_result res;
	int clocked = xfer_result;

	if (clocked < 0) {
		spinel_stat_inc(SPINEL_STAT_TRANSPORT_ERRORS);
		/* Do not spin on a failing SPI slave. */
		k_sleep(K_MSEC(10));
		return false;
	}

	spinel_spi_link_complete(&link, rx_buf, clocked, &res);
	if (res.sent) {
		spinel_tx_done(armed_buf, 0);
		armed_buf = NULL;
		/* Nothing is offered now (next_buf is NULL while armed_buf is not): IRQ is set
		 * again if the next arm() or the hold for a reply takes a frame.
		 */
		gpio_pin_set_dt(&irq_gpio, 0);
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

	return res.rx_len > 0;
}

/*
 * After a host frame, wait for a frame (its reply) while BUSY is still active, so that arm()
 * arms it. Nothing to wait for if a frame is already offered: arm() takes the unsent armed_buf
 * or next_buf first. A frame already queued is returned by spinel_tx_get() at once.
 */
static void hold_for_reply(void)
{
	if (CONFIG_DECTNRP_SPINEL_SPI_REPLY_WAIT_US == 0) {
		return;
	}
	if (armed_buf != NULL || next_buf != NULL) {
		return;
	}

	next_buf = spinel_tx_get(K_USEC(CONFIG_DECTNRP_SPINEL_SPI_REPLY_WAIT_US));
	if (next_buf != NULL) {
		offer_started();
		gpio_pin_set_dt(&irq_gpio, 1);
	}
}

static void spi_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		arm();
		wait_xfer();
		bool host_frame = complete();

		if (host_frame) {
			hold_for_reply();
		}
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

	spinel_spi_link_init(&link, CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE);

	k_thread_create(&spi_thread, spi_stack, K_THREAD_STACK_SIZEOF(spi_stack), spi_thread_fn,
			NULL, NULL, NULL, CONFIG_DECTNRP_SPINEL_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&spi_thread, "spinel_spi");

	return 0;
}

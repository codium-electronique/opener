/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The real SPI transport (subsys/spinel/transport/spi.c) on native_sim, against a fake SPI
 * slave controller (fake_spis.c) and emulated GPIOs. The test plays the host.
 *
 * Ordering: native_sim time only advances when every thread is blocked, so k_sleep() in the
 * test thread lets all Spinel threads settle first. The test thread has a higher priority than
 * the Spinel threads (ZTEST_THREAD_PRIORITY is cooperative): after fake_spis_clock() the SPI
 * thread has not run until the test thread sleeps or blocks. The tick is 1 us
 * (CONFIG_SYS_CLOCK_TICKS_PER_SEC in prj.conf), so a 50 us poll step is really 50 us.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "fake_spis.h"
#include "spinel_internal.h"

#define SPI_HEADER_LEN  5
#define HOST_RECV_LEN   64
#define XFER_LEN        (SPI_HEADER_LEN + HOST_RECV_LEN)
#define WAIT_MS         100
#define POLL_US         50
#define IRQ_PIN         0
#define BUSY_PIN        1
#define BOOT_FRAMES_MAX 256

static const struct device *const spis = DEVICE_DT_GET(DT_NODELABEL(fake_spis));
static const struct device *const gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));

/* Header of the first transaction after boot, and the frames the device sent until the suite
 * setup finished.
 */
static uint8_t first_header[SPI_HEADER_LEN];
/* RECV_LEN of the last valid device header: the largest frame the device accepts next. It is
 * zero until the first transaction, so the host sends nothing before it knows.
 */
static size_t device_recv_len;
static bool first_seen;
static bool recording;
/* Frames, each prefixed by its length in one byte. */
static uint8_t boot_frames[BOOT_FRAMES_MAX];
static size_t boot_frames_len;

static const uint8_t reset_power_on[] = {0x80, 0x06, 0x00, 0x70};
/* SPINEL_CMD_NOOP (0), TID 1. */
static const uint8_t noop[] = {0x81, 0x00};
static const uint8_t status_ok_tid1[] = {0x81, 0x06, 0x00, 0x00};
static const uint8_t status_1[] = {0x80, 0x06, 0x00, 0x01};
/* PROTOCOL_VERSION GET, TID 1 and 2, and the replies. */
static const uint8_t get_protocol_version[] = {0x81, 0x02, 0x01};
static const uint8_t get_protocol_version_tid2[] = {0x82, 0x02, 0x01};
static const uint8_t protocol_version_is[] = {0x81, 0x06, 0x01, 0x04, 0x03};
static const uint8_t protocol_version_is_tid2[] = {0x82, 0x06, 0x01, 0x04, 0x03};
/* Rejected: not a request, no reply. */
static const uint8_t not_a_request[] = {0x01, 0x01};

static bool busy(void)
{
	int level = gpio_emul_output_get(gpio, BUSY_PIN);

	return level != 0;
}

static bool irq(void)
{
	int level = gpio_emul_output_get(gpio, IRQ_PIN);

	return level != 0;
}

/* Wait up to WAIT_MS for BUSY to be low. */
static bool wait_ready(void)
{
	for (int i = 0; i < (WAIT_MS * 1000) / POLL_US; i++) {
		if (!busy()) {
			return true;
		}
		k_sleep(K_USEC(POLL_US));
	}

	return false;
}

/* Wait up to WAIT_MS for IRQ to be high. */
static bool wait_irq(void)
{
	for (int i = 0; i < (WAIT_MS * 1000) / POLL_US; i++) {
		if (irq()) {
			return true;
		}
		k_sleep(K_USEC(POLL_US));
	}

	return false;
}

/*
 * One transaction with a frame to the device (none if len is 0), as the host does after BUSY
 * went low. @p miso gets XFER_LEN bytes.
 *
 * @return the DATA_LEN of the device, or -EBADMSG for an invalid header.
 */
static int host_xfer(const uint8_t *frame, size_t len, uint8_t *miso)
{
	uint8_t mosi[XFER_LEN] = {0};
	bool ready = wait_ready();

	zassert_true(ready, "BUSY stays high");
	zassert_true(len <= HOST_RECV_LEN);
	zassert_true(len <= device_recv_len, "frame of %zu bytes, device accepts %zu", len,
		     device_recv_len);

	mosi[0] = 0x02;
	mosi[1] = HOST_RECV_LEN & 0xff;
	mosi[2] = HOST_RECV_LEN >> 8;
	mosi[3] = len & 0xff;
	mosi[4] = len >> 8;
	if (len > 0) {
		memcpy(&mosi[SPI_HEADER_LEN], frame, len);
	}

	int clocked = fake_spis_clock(spis, mosi, miso, XFER_LEN);

	zassert_equal(clocked, XFER_LEN, "transaction not clocked (%d)", clocked);

	bool pattern_ok = (miso[0] & 0x03) == 0x02;
	bool crc = (miso[0] & 0x40) != 0;

	if (!pattern_ok || crc) {
		return -EBADMSG;
	}

	device_recv_len = miso[1] | (miso[2] << 8);

	int data_len = miso[3] | (miso[4] << 8);

	zassert_true(data_len <= HOST_RECV_LEN, "device frame of %d bytes", data_len);

	if (!first_seen) {
		memcpy(first_header, miso, SPI_HEADER_LEN);
		first_seen = true;
	}
	if (recording && data_len > 0) {
		zassert_true(boot_frames_len + 1 + data_len <= sizeof(boot_frames));
		boot_frames[boot_frames_len] = (uint8_t)data_len;
		memcpy(&boot_frames[boot_frames_len + 1], &miso[SPI_HEADER_LEN], data_len);
		boot_frames_len += 1 + data_len;
	}

	return data_len;
}

/* Wait for IRQ, then clock until a transaction carries a frame (at most 3). */
static int recv_frame(uint8_t *frame)
{
	uint8_t miso[XFER_LEN];
	bool pending = wait_irq();

	zassert_true(pending, "no IRQ");

	for (int i = 0; i < 3; i++) {
		int len = host_xfer(NULL, 0, miso);

		zassert_true(len >= 0, "invalid header");
		if (len > 0) {
			memcpy(frame, &miso[SPI_HEADER_LEN], len);
			return len;
		}
	}

	zassert_unreachable("no frame in 3 transactions");
	return -ENODATA;
}

/* DRV_STATS GET, TID 1. */
static const uint8_t drv_stats_get[] = {0x81, 0x02, 0xa0, 0x7a};

/* The transport errors counter of DRV_STATS: the last of 7 u32 LE in the reply. */
static uint32_t transport_errors(void)
{
	uint8_t miso[XFER_LEN];
	uint8_t frame[HOST_RECV_LEN];

	int sent = host_xfer(drv_stats_get, sizeof(drv_stats_get), miso);

	zassert_equal(sent, 0);
	int len = recv_frame(frame);
	size_t head = sizeof(drv_stats_get);

	zassert_equal(len, head + 7 * 4);

	return sys_get_le32(&frame[head + 6 * 4]);
}

/* Clock out every pending frame, until the device has nothing more for the host. */
static void drain(void)
{
	uint8_t miso[XFER_LEN];

	while (true) {
		/* Let the Spinel threads settle: all of them blocked before time advances. */
		k_sleep(K_USEC(POLL_US));
		bool ready = wait_ready();

		zassert_true(ready, "BUSY stays high");
		if (!irq()) {
			return;
		}
		int ret = host_xfer(NULL, 0, miso);

		zassert_true(ret >= 0, "invalid header");
	}
}

static bool boot_frames_contain(const uint8_t *frame, size_t len)
{
	size_t pos = 0;

	while (pos < boot_frames_len) {
		size_t flen = boot_frames[pos];

		if (flen == len && memcmp(&boot_frames[pos + 1], frame, len) == 0) {
			return true;
		}
		pos += 1 + flen;
	}

	return false;
}

static void *suite_setup(void)
{
	bool spis_ready = device_is_ready(spis);
	bool gpio_ready = device_is_ready(gpio);

	zassert_true(spis_ready);
	zassert_true(gpio_ready);

	recording = true;
	drain();
	recording = false;

	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	drain();
}

ZTEST(spi_transport, test_boot_frames)
{
	zassert_true(first_seen);
	zassert_true((first_header[0] & 0x80) != 0, "first header without RST");
	bool has_reset = boot_frames_contain(reset_power_on, sizeof(reset_power_on));

	zassert_true(has_reset);
}

ZTEST(spi_transport, test_noop_reply)
{
	uint8_t miso[XFER_LEN];
	uint8_t frame[HOST_RECV_LEN];

	int sent = host_xfer(noop, sizeof(noop), miso);

	zassert_equal(sent, 0);
	int len = recv_frame(frame);

	zassert_equal(len, sizeof(status_ok_tid1));
	zassert_mem_equal(frame, status_ok_tid1, sizeof(status_ok_tid1));
}

ZTEST(spi_transport, test_unsolicited_needs_arming_transaction)
{
	uint8_t miso[XFER_LEN];

	spinel_reply_status(0, 1);

	bool pending = wait_irq();

	zassert_true(pending, "no IRQ");
	/* The device was armed without a frame: this transaction shows none, then the frame is
	 * armed.
	 */
	int arming = host_xfer(NULL, 0, miso);

	zassert_equal(arming, 0);

	int framed = host_xfer(NULL, 0, miso);

	zassert_equal(framed, sizeof(status_1));
	zassert_mem_equal(&miso[SPI_HEADER_LEN], status_1, sizeof(status_1));
}

ZTEST(spi_transport, test_transaction_while_busy_ignored)
{
	uint8_t miso[XFER_LEN];
	uint8_t mosi[XFER_LEN] = {0x02, HOST_RECV_LEN};

	int first = host_xfer(NULL, 0, miso);

	zassert_equal(first, 0);

	/* No wait: the SPI thread has not armed again, see the ordering note on top. */
	bool busy_high = busy();

	zassert_true(busy_high, "BUSY not active after a transaction");

	int ret = fake_spis_clock(spis, mosi, miso, XFER_LEN);

	zassert_equal(ret, -EAGAIN);
	zassert_equal(miso[0], 0xff);
}

ZTEST(spi_transport, test_frames_sent_in_order)
{
	uint8_t frame[HOST_RECV_LEN];

	spinel_reply_status(0, 1);
	spinel_reply_status(0, 2);
	spinel_reply_status(0, 3);

	for (uint8_t seq = 1; seq <= 3; seq++) {
		int len = recv_frame(frame);

		zassert_equal(len, 4);
		zassert_equal(frame[3], seq);
	}
	/* Let the SPI thread arm again: it clears IRQ with no frame left. */
	k_sleep(K_USEC(POLL_US));

	bool irq_high = irq();

	zassert_false(irq_high);
}

ZTEST(spi_transport, test_tx_buffers_released)
{
	uint8_t frame[HOST_RECV_LEN];

	for (int i = 0; i < 3 * CONFIG_DECTNRP_SPINEL_TX_BUFS; i++) {
		spinel_reply_status(0, 1);

		int len = recv_frame(frame);

		zassert_equal(len, 4);
	}
}

ZTEST(spi_transport, test_frames_behind_unclocked_frame_dropped)
{
	uint8_t frame[HOST_RECV_LEN];
	uint32_t errors = transport_errors();

	spinel_reply_status(0, 1);
	k_sleep(K_MSEC(10));
	spinel_reply_status(0, 2);
	spinel_reply_status(0, 3);
	/* The TX timeout is 100 ms and no host clocks. */
	k_sleep(K_MSEC(250));

	int len = recv_frame(frame);

	zassert_equal(len, 4);
	/* The offered frame stays. */
	zassert_equal(frame[3], 1);
	k_sleep(K_USEC(POLL_US));

	bool irq_high = irq();

	zassert_false(irq_high);

	uint32_t dropped = transport_errors() - errors;

	zassert_equal(dropped, 2);
}

ZTEST(spi_transport, test_reply_in_next_transaction)
{
	uint8_t miso[XFER_LEN];

	int sent = host_xfer(get_protocol_version, sizeof(get_protocol_version), miso);

	zassert_equal(sent, 0);

	/* Until the SPI thread arms the reply, BUSY is high and IRQ is not. No wait here: see the
	 * ordering note on top.
	 */
	bool busy_high = busy();
	bool irq_in_hold = irq();

	zassert_true(busy_high);
	zassert_false(irq_in_hold);

	bool ready = wait_ready();

	zassert_true(ready, "BUSY stays high");

	/* The reply is armed with IRQ high for the very next transaction. */
	bool irq_armed = irq();

	zassert_true(irq_armed);

	int len = host_xfer(NULL, 0, miso);

	zassert_equal(len, sizeof(protocol_version_is));
	zassert_mem_equal(&miso[SPI_HEADER_LEN], protocol_version_is, sizeof(protocol_version_is));
}

ZTEST(spi_transport, test_request_rides_on_reply_transaction)
{
	uint8_t miso[XFER_LEN];

	int first = host_xfer(noop, sizeof(noop), miso);

	zassert_equal(first, 0);

	int second = host_xfer(get_protocol_version_tid2, sizeof(get_protocol_version_tid2), miso);

	zassert_equal(second, sizeof(status_ok_tid1));
	zassert_mem_equal(&miso[SPI_HEADER_LEN], status_ok_tid1, sizeof(status_ok_tid1));

	int third = host_xfer(NULL, 0, miso);

	zassert_equal(third, sizeof(protocol_version_is_tid2));
	zassert_mem_equal(&miso[SPI_HEADER_LEN], protocol_version_is_tid2,
			  sizeof(protocol_version_is_tid2));
}

ZTEST(spi_transport, test_hold_bounded_without_reply)
{
	uint8_t miso[XFER_LEN];
	uint8_t frame[HOST_RECV_LEN];
	int64_t start = k_uptime_ticks();

	int sent = host_xfer(not_a_request, sizeof(not_a_request), miso);

	zassert_equal(sent, 0);

	bool ready = wait_ready();

	zassert_true(ready, "BUSY stays high");

	int64_t held_us = k_ticks_to_us_floor64(k_uptime_ticks() - start);

	zassert_true(held_us >= 2000, "held %lld us", held_us);

	bool irq_high = irq();

	zassert_false(irq_high);

	/* A late frame still arrives. */
	spinel_reply_status(0, 4);

	int len = recv_frame(frame);

	zassert_equal(len, 4);
	zassert_equal(frame[3], 4);
}

ZTEST(spi_transport, test_queued_frame_armed_without_hold)
{
	uint8_t miso[XFER_LEN];
	uint8_t frame[HOST_RECV_LEN];

	spinel_reply_status(0, 5);

	bool pending = wait_irq();

	zassert_true(pending, "no IRQ");

	int64_t start = k_uptime_ticks();
	/* The arming transaction carries the request. */
	int arming = host_xfer(noop, sizeof(noop), miso);

	zassert_equal(arming, 0);

	bool ready = wait_ready();

	zassert_true(ready, "BUSY stays high");

	int64_t held_us = k_ticks_to_us_floor64(k_uptime_ticks() - start);

	zassert_true(held_us < 1000, "held %lld us", held_us);

	int queued = host_xfer(NULL, 0, miso);

	zassert_equal(queued, 4);
	/* The queued frame first. */
	zassert_equal(miso[SPI_HEADER_LEN + 3], 5);

	int len = recv_frame(frame);

	zassert_equal(len, sizeof(status_ok_tid1));
	/* Then the reply. */
	zassert_mem_equal(frame, status_ok_tid1, sizeof(status_ok_tid1));
}

ZTEST(spi_transport, test_transaction_during_hold_ignored)
{
	uint8_t miso[XFER_LEN];
	uint8_t mosi[XFER_LEN] = {0x02, HOST_RECV_LEN};

	int sent = host_xfer(not_a_request, sizeof(not_a_request), miso);

	zassert_equal(sent, 0);
	/* Inside the 2 ms hold. */
	k_sleep(K_USEC(500));

	bool busy_high = busy();

	zassert_true(busy_high);

	int ret = fake_spis_clock(spis, mosi, miso, XFER_LEN);

	zassert_equal(ret, -EAGAIN);
	zassert_equal(miso[0], 0xff);

	int idle = host_xfer(NULL, 0, miso);

	zassert_equal(idle, 0);
}

/* A transaction of the bare header: too short to carry a device frame. */
static void short_xfer(void)
{
	uint8_t mosi[SPI_HEADER_LEN] = {0x02, HOST_RECV_LEN};
	uint8_t miso[SPI_HEADER_LEN];
	bool ready = wait_ready();

	zassert_true(ready, "BUSY stays high");

	int clocked = fake_spis_clock(spis, mosi, miso, sizeof(mosi));

	zassert_equal(clocked, sizeof(mosi));
}

ZTEST(spi_transport, test_short_xfers_do_not_postpone_drop)
{
	uint8_t miso[XFER_LEN];
	uint8_t frame[HOST_RECV_LEN];
	uint32_t errors = transport_errors();

	/* Past the reply hold of the statistics request. */
	drain();
	spinel_reply_status(0, 1);
	spinel_reply_status(0, 2);
	spinel_reply_status(0, 3);

	bool pending = wait_irq();

	zassert_true(pending, "no IRQ");

	/* The arming transaction shows no frame, then frame 1 is armed. */
	int arming = host_xfer(NULL, 0, miso);

	zassert_equal(arming, 0);

	/* The TX timeout is 100 ms; every transaction is shorter than the frame. */
	for (int i = 0; i < 8; i++) {
		k_sleep(K_MSEC(30));
		short_xfer();
	}
	k_sleep(K_USEC(POLL_US));

	int len = recv_frame(frame);

	zassert_equal(len, 4);
	/* The offered frame stays. */
	zassert_equal(frame[3], 1);

	uint32_t dropped = transport_errors() - errors;

	zassert_equal(dropped, 2);
}

ZTEST(spi_transport, test_irq_low_during_reply_hold)
{
	uint8_t miso[XFER_LEN];

	spinel_reply_status(0, 5);

	bool pending = wait_irq();

	zassert_true(pending, "no IRQ");

	/* The arming transaction shows no frame; frame 5 is armed. */
	int arming = host_xfer(not_a_request, sizeof(not_a_request), miso);

	zassert_equal(arming, 0);

	/* This one carries frame 5 away and a host frame that gets no reply. */
	int sent = host_xfer(not_a_request, sizeof(not_a_request), miso);

	zassert_equal(sent, 4);

	/* Inside the 2 ms hold, with nothing armed. */
	k_sleep(K_USEC(500));

	bool busy_high = busy();
	bool irq_high = irq();

	zassert_true(busy_high);
	zassert_false(irq_high);
}

ZTEST_SUITE(spi_transport, NULL, suite_setup, before, NULL, NULL);

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
 * thread has not run until the test thread sleeps or blocks.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>
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

	int data_len = miso[3] | (miso[4] << 8);

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
	zassert_true(device_is_ready(spis));
	zassert_true(device_is_ready(gpio));

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
	zassert_true(boot_frames_contain(reset_power_on, sizeof(reset_power_on)));
}

ZTEST(spi_transport, test_noop_reply)
{
	uint8_t miso[XFER_LEN];
	uint8_t frame[HOST_RECV_LEN];

	host_xfer(noop, sizeof(noop), miso);
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
	zassert_equal(host_xfer(NULL, 0, miso), 0);
	zassert_equal(host_xfer(NULL, 0, miso), sizeof(status_1));
	zassert_mem_equal(&miso[SPI_HEADER_LEN], status_1, sizeof(status_1));
}

ZTEST(spi_transport, test_transaction_while_busy_ignored)
{
	uint8_t miso[XFER_LEN];
	uint8_t mosi[XFER_LEN] = {0x02, HOST_RECV_LEN};

	host_xfer(NULL, 0, miso);

	/* No wait: the SPI thread has not armed again, see the ordering note on top. */
	int ret = fake_spis_clock(spis, mosi, miso, XFER_LEN);

	zassert_equal(ret, -EAGAIN);
	zassert_equal(miso[0], 0xff);
}

ZTEST_SUITE(spi_transport, NULL, suite_setup, before, NULL, NULL);

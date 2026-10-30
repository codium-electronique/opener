/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/net/dectnrp_driver.h>
#include <zephyr/net/dectnrp_net_l2.h>
#include <zephyr/net/net_if.h>
#include <zephyr/ztest.h>

#include "mock_driver.h"

/* Modem ticks per millisecond and the simulated frame period in ms. */
#define TICKS_PER_MS 69120
#define PERIOD_MS    CONFIG_DECTNRP_DRIVER_MOCK_AUTO_EMIT_PERIOD_MS

static struct dectnrp_driver_op op;
static K_SEM_DEFINE(finished, 0, 1);
static int frames;
static int64_t last_frame_time;

static void on_event(struct net_if *iface, const struct dectnrp_driver_event *evt)
{
	ARG_UNUSED(iface);

	if (evt->code == DECTNRP_EVENT_MSG_RECEIVED) {
		frames++;
		last_frame_time = evt->msg_received.start_time;
	}
	if (evt->code == DECTNRP_EVENT_MSG_ERROR) {
		frames++;
		last_frame_time = evt->message_error.start_time;
	}
	if (evt->code == DECTNRP_EVENT_OP_FINISHED) {
		k_sem_give(&finished);
	}
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	mock_driver_reset();
	k_sem_reset(&finished);
	frames = 0;
	last_frame_time = 0;
}

/* Run a continuous RX window from @p start to @p end, in modem ticks. */
static void run_window(int64_t start, int64_t end)
{
	const struct device *dev = net_if_get_device(mock_driver_iface());
	const struct dectnrp_driver_api *api = dev->api;
	struct dectnrp_config cfg = {.event_handler = on_event};
	int ret;

	ret = api->configure(dev, DECTNRP_CONFIG_TYPE_EVENT_HANDLER, &cfg);
	zassert_ok(ret);

	memset(&op, 0, sizeof(op));
	op.type = DECTNRP_DRIVER_OP_RX;
	op.start_time = start;
	op.rx.mode.is_scheduled = 1;
	op.rx.mode.reception = DECTNRP_RX_RECEPTION_CONTINUOUS;
	op.rx.duration = end - start;
	ret = api->schedule(dev, &op);
	zassert_ok(ret);

	ret = k_sem_take(&finished, K_SECONDS(5));
	zassert_ok(ret);
}

/* The first frame on air, in ms of uptime, a few periods ahead. */
static int64_t first_frame_ms(void)
{
	int64_t now;

	now = k_uptime_get();

	return ((now / PERIOD_MS) + 3) * PERIOD_MS;
}

ZTEST(spinel_mock_auto, test_window_starting_after_a_frame)
{
	int64_t frame_ms = first_frame_ms();
	int64_t start = frame_ms * TICKS_PER_MS + TICKS_PER_MS / 4;

	/* The frame starts 0.25 ms before the window: it is not received. */
	run_window(start, start + PERIOD_MS * TICKS_PER_MS / 2);

	zassert_equal(frames, 0);
}

ZTEST(spinel_mock_auto, test_window_ending_after_a_frame)
{
	int64_t frame_ms = first_frame_ms();
	int64_t frame_ticks = frame_ms * TICKS_PER_MS;
	int64_t start = frame_ticks - PERIOD_MS * TICKS_PER_MS / 2;

	/* The window ends 0.75 ms after the frame: it is received. */
	run_window(start, frame_ticks + TICKS_PER_MS * 3 / 4);

	zassert_equal(frames, 1);
	zassert_equal(last_frame_time, frame_ticks);
}

ZTEST(spinel_mock_auto, test_window_ending_at_a_frame)
{
	int64_t frame_ms = first_frame_ms();
	int64_t frame_ticks = frame_ms * TICKS_PER_MS;
	int64_t start = frame_ticks - PERIOD_MS * TICKS_PER_MS / 2;

	/* A window ends before a frame starting at its end. */
	run_window(start, frame_ticks);

	zassert_equal(frames, 0);
}

ZTEST_SUITE(spinel_mock_auto, NULL, NULL, before, NULL, NULL);

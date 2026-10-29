/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/dectnrp.h>
#include <zephyr/net/dectnrp_driver.h>
#include <zephyr/net/dectnrp_net_l2.h>
#include <zephyr/net/net_if.h>

#include "mock_driver.h"

/* Modem ticks per millisecond (69.12 MHz modem clock). */
#define MOCK_TICKS_PER_MS 69120

static struct mock_driver mock;
static struct net_if *mock_iface;

/* Operations already reported as finished, indexed like mock.ops. */
static bool op_finished[ARRAY_SIZE(mock.ops)];

struct mock_driver *mock_driver_get(void)
{
	return &mock;
}

void mock_driver_reset(void)
{
	dectnrp_driver_event_cb_t cb = mock.cb;

	memset(&mock, 0, sizeof(mock));
	memset(op_finished, 0, sizeof(op_finished));
	mock.cb = cb;
}

struct net_if *mock_driver_iface(void)
{
	return mock_iface;
}

void mock_driver_emit(const struct dectnrp_driver_event *evt)
{
	if (mock.cb != NULL) {
		mock.cb(mock_iface, evt);
	}
}

static int mock_start(const struct device *dev)
{
	ARG_UNUSED(dev);

	mock.start_calls++;
	if (mock.start_ret == 0) {
		mock.started = true;
	}

	return mock.start_ret;
}

static int mock_stop(const struct device *dev)
{
	ARG_UNUSED(dev);

	mock.stop_calls++;
	mock.started = false;

	/* Like the nRF91 driver, stopping cancels every pending operation. */
	for (size_t i = 0; i < mock.n_ops; i++) {
		struct dectnrp_driver_event evt = {
			.code = DECTNRP_EVENT_OP_FINISHED,
			.op_finished.op = mock.ops[i],
		};

		if (op_finished[i]) {
			continue;
		}
		op_finished[i] = true;
		mock_driver_emit(&evt);
	}

	return 0;
}

static enum dectnrp_hw_caps mock_get_hw_capabilities(const struct device *dev)
{
	ARG_UNUSED(dev);

	return DECTNRP_HW_FILTER | DECTNRP_HW_TXTIME | DECTNRP_HW_RXTIME;
}

static int mock_get_capabilities(const struct device *dev, struct dectnrp_device_capabilities *caps)
{
	/* nRF9131 latencies, in modem ticks. */
	*caps = (struct dectnrp_device_capabilities){
		.hw_caps = mock_get_hw_capabilities(dev),
		.sched_op_transition = 25920,
		.sched_op_startup = 0,
		.rx_startup = 24192,
		.rx_shutdown = 12441,
		.rssi_shutdown = 13824,
		.tx_startup = 29030,
		.tx_shutdown = 7603,
		.stop_to_rf_off = 14169,
	};

	return 0;
}

static int mock_configure(const struct device *dev, enum dectnrp_config_type type,
			  const struct dectnrp_config *config)
{
	ARG_UNUSED(dev);

	switch (type) {
	case DECTNRP_CONFIG_TYPE_EVENT_HANDLER:
		mock.cb = config->event_handler;
		break;
	case DECTNRP_CONFIG_TYPE_NETWORK_ID:
		mock.network_id = config->network_id;
		break;
	case DECTNRP_CONFIG_TYPE_FILTER_SHORT_NETWORK_ID:
		mock.nwid_filter = config->filter_short_network_id.activate;
		mock.nwid = config->filter_short_network_id.value;
		break;
	case DECTNRP_CONFIG_TYPE_FILTER_SHORT_RECEIVER_ADDR:
		mock.rxaddr_filter = config->filter_short_receiver_addr.activate;
		mock.rxaddr = config->filter_short_receiver_addr.value;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static int mock_get_time(const struct device *dev, net_time_t *time)
{
	ARG_UNUSED(dev);

	if (mock.time != 0) {
		*time = mock.time;
	} else {
		*time = k_uptime_get() * MOCK_TICKS_PER_MS;
	}

	return 0;
}

static int mock_schedule(const struct device *dev, struct dectnrp_driver_op *op)
{
	ARG_UNUSED(dev);

	if (mock.schedule_ret != 0) {
		return mock.schedule_ret;
	}
	if (mock.n_ops == ARRAY_SIZE(mock.ops)) {
		return -ENOMEM;
	}

	op_finished[mock.n_ops] = false;
	mock.ops[mock.n_ops++] = op;

	return 0;
}

static void mock_iface_init(struct net_if *iface)
{
	mock_iface = iface;
	dectnrp_l2_init(iface);
}

static const struct dectnrp_driver_api mock_api = {
	.iface_api = {.init = mock_iface_init},
	.start = mock_start,
	.stop = mock_stop,
	.get_hw_capabilities = mock_get_hw_capabilities,
	.get_capabilities = mock_get_capabilities,
	.configure = mock_configure,
	.get_time = mock_get_time,
	.schedule = mock_schedule,
};

static int mock_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

NET_DEVICE_INIT(dectnrp_mock, "dectnrp_mock", mock_init, NULL, NULL, NULL,
		CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &mock_api, DECTNRP_L2,
		NET_L2_GET_CTX_TYPE(DECTNRP_L2), DECTNRP_MTU);

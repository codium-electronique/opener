/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Mock DECT NR+ driver for the Spinel tests.
 *
 * The mock records what the driver API is asked to do; tests inspect and
 * steer it through struct mock_driver.
 */

#ifndef DECTNRP_TESTS_SPINEL_MOCK_DRIVER_H_
#define DECTNRP_TESTS_SPINEL_MOCK_DRIVER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/net/dectnrp_driver.h>
#include <zephyr/net/net_if.h>

/** State of the mock driver. */
struct mock_driver {
	/** Between a successful start() and stop(). */
	bool started;
	/** Number of start() and stop() calls. */
	int start_calls, stop_calls;
	/** Last configured network ID. */
	uint32_t network_id;
	/** Short network ID filter. */
	bool nwid_filter;
	uint8_t nwid;
	/** Short receiver address filter. */
	bool rxaddr_filter;
	uint16_t rxaddr;
	/** Event handler registered with configure(). */
	dectnrp_driver_event_cb_t cb;
	/** Operations accepted by schedule(), in order. */
	struct dectnrp_driver_op *ops[16];
	size_t n_ops;
	/** Return value of schedule(); the op is recorded only when it is 0. */
	int schedule_ret;
	/** Return value of start(). */
	int start_ret;
	/** Time returned by get_time() when non-zero. */
	net_time_t time;
};

/** @return the mock state. */
struct mock_driver *mock_driver_get(void);

/** Reset the mock state, except the registered event handler. */
void mock_driver_reset(void);

/** @return the network interface of the mock driver. */
struct net_if *mock_driver_iface(void);

/** Deliver @p evt to the registered event handler, if any. */
void mock_driver_emit(const struct dectnrp_driver_event *evt);

#endif /* DECTNRP_TESTS_SPINEL_MOCK_DRIVER_H_ */

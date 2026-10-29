/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/net/dectnrp_driver.h>
#include <zephyr/net/dectnrp_net_l2.h>
#include <zephyr/net/net_if.h>
#include <zephyr/ztest.h>

#include "mock_driver.h"

static struct dectnrp_driver_op ops[3];
static int finished;

static void count_finished(struct net_if *iface, const struct dectnrp_driver_event *evt)
{
	struct net_if *mock_iface = mock_driver_iface();

	zassert_equal(iface, mock_iface);
	if (evt->code == DECTNRP_EVENT_OP_FINISHED) {
		finished++;
	}
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	mock_driver_reset();
	finished = 0;
}

ZTEST(spinel_mock, test_iface_found)
{
	struct net_if *mock_iface = mock_driver_iface();
	struct net_if *dectnrp_iface;

	zassert_not_null(mock_iface);
	dectnrp_iface = net_if_get_first_by_type(&NET_L2_GET_NAME(DECTNRP));
	zassert_equal(dectnrp_iface, mock_iface);
}

ZTEST(spinel_mock, test_schedule_records)
{
	struct mock_driver *mock = mock_driver_get();
	const struct device *dev = net_if_get_device(mock_driver_iface());
	const struct dectnrp_driver_api *api = dev->api;
	int ret;

	ops[0].type = DECTNRP_DRIVER_OP_RX;
	ret = api->schedule(dev, &ops[0]);
	zassert_ok(ret);
	zassert_equal(mock->n_ops, 1);
	zassert_equal_ptr(mock->ops[0], &ops[0]);

	/* A rejected op is not recorded. */
	mock->schedule_ret = -EBUSY;
	ret = api->schedule(dev, &ops[1]);
	zassert_equal(ret, -EBUSY);
	zassert_equal(mock->n_ops, 1);
}

ZTEST(spinel_mock, test_stop_finishes_ops)
{
	struct mock_driver *mock = mock_driver_get();
	const struct device *dev = net_if_get_device(mock_driver_iface());
	const struct dectnrp_driver_api *api = dev->api;
	struct dectnrp_config cfg = {.event_handler = count_finished};
	int ret;

	ret = api->configure(dev, DECTNRP_CONFIG_TYPE_EVENT_HANDLER, &cfg);
	zassert_ok(ret);
	ret = api->start(dev);
	zassert_ok(ret);
	for (size_t i = 0; i < ARRAY_SIZE(ops); i++) {
		ret = api->schedule(dev, &ops[i]);
		zassert_ok(ret);
	}

	ret = api->stop(dev);
	zassert_ok(ret);
	zassert_equal(finished, ARRAY_SIZE(ops));
	zassert_equal(mock->stop_calls, 1);

	/* Finished ops are not reported twice. */
	ret = api->stop(dev);
	zassert_ok(ret);
	zassert_equal(finished, ARRAY_SIZE(ops));
}

ZTEST_SUITE(spinel_mock, NULL, NULL, before, NULL, NULL);

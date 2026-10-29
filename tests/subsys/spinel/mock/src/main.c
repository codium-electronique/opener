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

static const struct dectnrp_driver_api *api(void)
{
	return net_if_get_device(mock_driver_iface())->api;
}

static void count_finished(struct net_if *iface, const struct dectnrp_driver_event *evt)
{
	zassert_equal(iface, mock_driver_iface());
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
	zassert_not_null(mock_driver_iface());
	zassert_equal(net_if_get_first_by_type(&NET_L2_GET_NAME(DECTNRP)), mock_driver_iface());
}

ZTEST(spinel_mock, test_schedule_records)
{
	const struct device *dev = net_if_get_device(mock_driver_iface());

	ops[0].type = DECTNRP_DRIVER_OP_RX;
	zassert_ok(api()->schedule(dev, &ops[0]));
	zassert_equal(mock_driver_get()->n_ops, 1);
	zassert_equal_ptr(mock_driver_get()->ops[0], &ops[0]);

	mock_driver_get()->schedule_ret = -EBUSY;
	zassert_equal(api()->schedule(dev, &ops[1]), -EBUSY);
	zassert_equal(mock_driver_get()->n_ops, 1);
}

ZTEST(spinel_mock, test_stop_finishes_ops)
{
	const struct device *dev = net_if_get_device(mock_driver_iface());
	struct dectnrp_config cfg = {.event_handler = count_finished};

	zassert_ok(api()->configure(dev, DECTNRP_CONFIG_TYPE_EVENT_HANDLER, &cfg));
	zassert_ok(api()->start(dev));
	for (size_t i = 0; i < ARRAY_SIZE(ops); i++) {
		zassert_ok(api()->schedule(dev, &ops[i]));
	}

	zassert_ok(api()->stop(dev));
	zassert_equal(finished, ARRAY_SIZE(ops));
	zassert_equal(mock_driver_get()->stop_calls, 1);

	/* Finished ops are not reported twice. */
	zassert_ok(api()->stop(dev));
	zassert_equal(finished, ARRAY_SIZE(ops));
}

ZTEST_SUITE(spinel_mock, NULL, NULL, before, NULL, NULL);

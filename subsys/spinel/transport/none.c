/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Test transport: frames sent by the core are queued for spinel_test_recv(),
 * and tests inject frames with spinel_rx_submit().
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "spinel_internal.h"

struct test_frame {
	size_t len;
	uint8_t data[CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE];
};

K_MSGQ_DEFINE(test_frames, sizeof(struct test_frame), 8, 4);

int spinel_transport_init(void)
{
	return 0;
}

int spinel_transport_send(const uint8_t *frame, size_t len)
{
	static struct test_frame item;

	if (len > sizeof(item.data)) {
		return -EMSGSIZE;
	}
	item.len = len;
	memcpy(item.data, frame, len);

	return k_msgq_put(&test_frames, &item, K_FOREVER);
}

int spinel_test_recv(uint8_t *buf, size_t size, k_timeout_t timeout)
{
	static struct test_frame item;
	int ret = k_msgq_get(&test_frames, &item, timeout);

	if (ret != 0) {
		return ret;
	}
	if (item.len > size) {
		return -EMSGSIZE;
	}
	memcpy(buf, item.data, item.len);

	return item.len;
}

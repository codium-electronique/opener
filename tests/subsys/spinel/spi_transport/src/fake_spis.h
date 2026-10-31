/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Fake SPI slave controller, driven by the test as the SPI master.
 *
 * The driver holds one armed transfer (transceive_async()). The test plays the host by
 * clocking bytes with fake_spis_clock().
 */

#ifndef FAKE_SPIS_H_
#define FAKE_SPIS_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

/**
 * @brief Clock one transaction of @p len bytes.
 *
 * If a transfer is armed, @p miso gets the armed TX bytes, then 0xfe past them, the armed RX
 * buffer gets @p mosi up to its length, the transfer is disarmed and its callback is called,
 * in the caller's thread, with the clocked count.
 *
 * @return the clocked count, or -EAGAIN if nothing is armed: @p miso is then all 0xff and no
 *         callback is called.
 */
int fake_spis_clock(const struct device *dev, const uint8_t *mosi, uint8_t *miso, size_t len);

/** @return whether a transfer is armed. */
bool fake_spis_armed(const struct device *dev);

#endif /* FAKE_SPIS_H_ */

/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT opener_test_fake_spis

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/ztest_assert.h>

#include "fake_spis.h"

#define SPI_HEADER_LEN 5

struct fake_spis_data {
	struct k_spinlock lock;
	bool armed;
	const uint8_t *tx;
	size_t tx_len;
	uint8_t *rx;
	size_t rx_len;
	spi_callback_t cb;
	void *cb_data;
};

static int fake_spis_transceive(const struct device *dev, const struct spi_config *config,
				const struct spi_buf_set *tx_bufs,
				const struct spi_buf_set *rx_bufs)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(config);
	ARG_UNUSED(tx_bufs);
	ARG_UNUSED(rx_bufs);

	return -ENOTSUP;
}

static int fake_spis_transceive_async(const struct device *dev, const struct spi_config *config,
				      const struct spi_buf_set *tx_bufs,
				      const struct spi_buf_set *rx_bufs, spi_callback_t cb,
				      void *userdata)
{
	struct fake_spis_data *data = dev->data;
	k_spinlock_key_t key;
	int ret = 0;

	ARG_UNUSED(config);

	/* The transport arms one TX and one RX buffer, each at least the 5-byte SPI header. */
	zassert_equal(tx_bufs->count, 1, "%zu TX buffers", tx_bufs->count);
	zassert_equal(rx_bufs->count, 1, "%zu RX buffers", rx_bufs->count);

	size_t tx_len_armed = tx_bufs->buffers[0].len;
	size_t rx_len_armed = rx_bufs->buffers[0].len;
	size_t rx_len_expected = SPI_HEADER_LEN + CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE;

	zassert_true(tx_len_armed >= SPI_HEADER_LEN && tx_len_armed <= rx_len_expected,
		     "TX of %zu bytes", tx_len_armed);
	zassert_equal(rx_len_armed, rx_len_expected, "RX of %zu bytes", rx_len_armed);

	key = k_spin_lock(&data->lock);
	if (data->armed) {
		ret = -EBUSY;
	} else {
		data->tx = tx_bufs->buffers[0].buf;
		data->tx_len = tx_bufs->buffers[0].len;
		data->rx = rx_bufs->buffers[0].buf;
		data->rx_len = rx_bufs->buffers[0].len;
		data->cb = cb;
		data->cb_data = userdata;
		data->armed = true;
	}
	k_spin_unlock(&data->lock, key);

	return ret;
}

static int fake_spis_release(const struct device *dev, const struct spi_config *config)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(config);

	return 0;
}

static DEVICE_API(spi, fake_spis_api) = {
	.transceive = fake_spis_transceive,
	.transceive_async = fake_spis_transceive_async,
	.release = fake_spis_release,
};

int fake_spis_clock(const struct device *dev, const uint8_t *mosi, uint8_t *miso, size_t len)
{
	struct fake_spis_data *data = dev->data;
	k_spinlock_key_t key;
	spi_callback_t cb;
	void *cb_data;
	const uint8_t *tx;
	size_t tx_len;
	uint8_t *rx;
	size_t rx_len;

	key = k_spin_lock(&data->lock);
	if (!data->armed) {
		k_spin_unlock(&data->lock, key);
		memset(miso, 0xff, len);
		return -EAGAIN;
	}
	data->armed = false;
	cb = data->cb;
	cb_data = data->cb_data;
	tx = data->tx;
	tx_len = data->tx_len;
	rx = data->rx;
	rx_len = data->rx_len;
	k_spin_unlock(&data->lock, key);

	size_t from_tx = MIN(len, tx_len);
	size_t to_rx = MIN(len, rx_len);

	memcpy(miso, tx, from_tx);
	memset(&miso[from_tx], 0xfe, len - from_tx);
	memcpy(rx, mosi, to_rx);

	/* Like a real SPI slave, report the bytes received, not the bytes clocked. */
	cb(dev, (int)to_rx, cb_data);

	return (int)len;
}

bool fake_spis_armed(const struct device *dev)
{
	struct fake_spis_data *data = dev->data;
	k_spinlock_key_t key;
	bool armed;

	key = k_spin_lock(&data->lock);
	armed = data->armed;
	k_spin_unlock(&data->lock, key);

	return armed;
}

#define FAKE_SPIS_INIT(inst)                                                                       \
	static struct fake_spis_data fake_spis_data_##inst;                                        \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &fake_spis_data_##inst, NULL, POST_KERNEL,         \
			      CONFIG_SPI_INIT_PRIORITY, &fake_spis_api);

DT_INST_FOREACH_STATUS_OKAY(FAKE_SPIS_INIT)

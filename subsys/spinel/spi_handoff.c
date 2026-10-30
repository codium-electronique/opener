/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include "spi_handoff.h"

void spinel_spi_handoff_init(struct spinel_spi_handoff *h, void (*wake)(void))
{
	k_mutex_init(&h->lock);
	k_sem_init(&h->sent, 0, 1);
	h->wake = wake;
	h->frame = NULL;
	h->len = 0;
	h->seq = 0;
}

int spinel_spi_handoff_send(struct spinel_spi_handoff *h, const uint8_t *frame, size_t len,
			    k_timeout_t timeout)
{
	uint32_t seq;
	int ret;

	k_mutex_lock(&h->lock, K_FOREVER);
	k_sem_reset(&h->sent);
	h->frame = frame;
	h->len = len;
	h->seq++;
	seq = h->seq;
	k_mutex_unlock(&h->lock);

	h->wake();

	ret = k_sem_take(&h->sent, timeout);
	if (ret == 0) {
		return 0;
	}

	k_mutex_lock(&h->lock, K_FOREVER);
	bool still_pending = (h->seq == seq) && (h->frame != NULL);

	if (still_pending) {
		h->frame = NULL;
		h->len = 0;
	}
	k_mutex_unlock(&h->lock);

	if (still_pending) {
		return -ETIMEDOUT;
	}

	/* Completed between the timeout and the lock. */
	return 0;
}

int spinel_spi_handoff_arm(struct spinel_spi_handoff *h, struct spinel_spi_link *link, bool rx_room,
			   uint8_t *tx_buf, size_t tx_size, size_t *total, uint32_t *seq)
{
	size_t armed;
	int ret = 0;

	k_mutex_lock(&h->lock, K_FOREVER);
	*seq = h->seq;
	armed = spinel_spi_link_arm(link, h->frame, h->len, rx_room, tx_buf, tx_size);
	if (armed == 0) {
		armed = spinel_spi_link_arm(link, NULL, 0, rx_room, tx_buf, tx_size);
		ret = -EMSGSIZE;
	}
	k_mutex_unlock(&h->lock);

	*total = armed;
	return ret;
}

bool spinel_spi_handoff_done(struct spinel_spi_handoff *h, uint32_t seq)
{
	bool completed = false;

	k_mutex_lock(&h->lock, K_FOREVER);
	if (h->frame != NULL && h->seq == seq) {
		h->frame = NULL;
		h->len = 0;
		k_sem_give(&h->sent);
		completed = true;
	}
	k_mutex_unlock(&h->lock);

	return completed;
}

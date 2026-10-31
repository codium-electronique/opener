/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Hand-off of Spinel frames from senders to the SPI thread.
 *
 * One mutex covers the copy of the pending frame into the transmit buffer (arm), the clearing of
 * the pending frame with the completion signal (done), and the publishing and dropping of a
 * frame (send). A sender that returns, sent or timed out, is never read afterwards, and a
 * completion always belongs to the frame it completes.
 *
 * There is one pending frame: only one thread may send at a time (the Spinel TX thread). A
 * second concurrent sender would replace the pending frame of the first, which would then
 * time out or be reported sent with the second frame.
 */

#ifndef DECTNRP_SPINEL_SPI_HANDOFF_H_
#define DECTNRP_SPINEL_SPI_HANDOFF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "spi_link.h"

/** Hand-off state. */
struct spinel_spi_handoff {
	/** Covers every field below and the copy of the frame. */
	struct k_mutex lock;
	/** Given when the pending frame was sent. */
	struct k_sem sent;
	/** Called by the sender, without the lock, once a frame is pending. */
	void (*wake)(void);
	/** Pending frame, NULL for none. */
	const uint8_t *frame;
	/** Length of @ref frame. */
	size_t len;
	/** Incremented by each send. */
	uint32_t seq;
};

/**
 * @brief Initialize the hand-off.
 *
 * @param h Hand-off.
 * @param wake Called by each send after it published its frame, to wake the SPI thread.
 */
void spinel_spi_handoff_init(struct spinel_spi_handoff *h, void (*wake)(void));

/**
 * @brief Publish a frame, wake the SPI thread and wait until it was sent.
 *
 * After any return the hand-off never reads @p frame again. Not reentrant: callers must
 * serialize their sends, see the file description.
 *
 * @param h Hand-off.
 * @param frame Frame to send.
 * @param len Length of @p frame.
 * @param timeout Time to wait for the frame to be sent.
 *
 * @retval 0 The frame was sent.
 * @retval -ETIMEDOUT No transaction completed the frame in time, it is no longer pending. A
 *         copy armed before the timeout may still be clocked out to the host by the transaction
 *         in progress; that delivery is not reported.
 */
int spinel_spi_handoff_send(struct spinel_spi_handoff *h, const uint8_t *frame, size_t len,
			    k_timeout_t timeout);

/**
 * @brief Arm the transmit buffer with the pending frame, or with none.
 *
 * The frame is copied under the lock.
 *
 * @param h Hand-off.
 * @param link Link state.
 * @param rx_room Whether a host frame can be received, see spinel_spi_link_arm().
 * @param tx_buf Transmit buffer.
 * @param tx_size Size of @p tx_buf.
 * @param total Number of bytes to arm (header and frame).
 * @param seq Number of the pending frame, to pass to spinel_spi_handoff_done().
 * @param frame_len Length of the pending frame, 0 for none.
 *
 * @retval 0 Armed.
 * @retval -EMSGSIZE The pending frame does not fit: armed without it.
 */
int spinel_spi_handoff_arm(struct spinel_spi_handoff *h, struct spinel_spi_link *link, bool rx_room,
			   uint8_t *tx_buf, size_t tx_size, size_t *total, uint32_t *seq,
			   size_t *frame_len);

/**
 * @brief Complete the frame of @p seq, if it is still pending.
 *
 * @param h Hand-off.
 * @param seq Number returned by spinel_spi_handoff_arm().
 *
 * @return true if the frame was pending and is now completed.
 */
bool spinel_spi_handoff_done(struct spinel_spi_handoff *h, uint32_t seq);

#ifdef SPINEL_SPI_HANDOFF_TEST_HOOK
/**
 * @brief Test hook, called by spinel_spi_handoff_arm() between taking the pending frame and
 * copying it.
 *
 * Defined by the hand-off test (which defines SPINEL_SPI_HANDOFF_TEST_HOOK) to let other
 * threads run in that window, so that a copy made outside the lock is caught.
 */
void spinel_spi_handoff_test_copy_window(void);
#endif

#endif /* DECTNRP_SPINEL_SPI_HANDOFF_H_ */

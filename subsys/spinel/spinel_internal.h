/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Internal interfaces of the DECT NR+ Spinel subsystem.
 */

#ifndef DECTNRP_SPINEL_INTERNAL_H_
#define DECTNRP_SPINEL_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/net/dectnrp_driver.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net_buf.h>

#include "spinel.h"

/**
 * @name Transport contract
 *
 * Exactly one transport is linked in.
 * @{
 */

/** Initialize the transport, before the core threads start. */
int spinel_transport_init(void);

#ifndef CONFIG_DECTNRP_SPINEL_TRANSPORT_PULLS_TX
/**
 * Send one Spinel frame to the host, blocking until it is handed to the
 * hardware. Called from the TX thread only. A transport that takes its
 * frames itself (CONFIG_DECTNRP_SPINEL_TRANSPORT_PULLS_TX) has none: the
 * core starts no TX thread.
 */
int spinel_transport_send(const uint8_t *frame, size_t len);
#endif

/**
 * Take the next frame the core queued for the host, for a transport that
 * pulls its frames. The transport owns the buffer until spinel_tx_done().
 *
 * @return the frame, or NULL on timeout.
 */
struct net_buf *spinel_tx_get(k_timeout_t timeout);

#ifdef CONFIG_DECTNRP_SPINEL_TRANSPORT_PULLS_TX
/**
 * Initialize @p event to wait, with k_poll(), for a frame in the TX queue.
 * The frame is then taken with spinel_tx_get().
 */
void spinel_tx_poll_init(struct k_poll_event *event);
#endif

/**
 * Release a frame of spinel_tx_get(). A nonzero @p err (the frame was not
 * sent) counts a transport error.
 */
void spinel_tx_done(struct net_buf *buf, int err);

/**
 * Hand one Spinel frame received from the host to the core. ISR-safe: the
 * frame is copied, so @p frame can be reused on return.
 */
void spinel_rx_submit(const uint8_t *frame, size_t len);

/**
 * Take a buffer for one frame from the host, for a transport that receives
 * into it directly. @return the buffer, or NULL if none is free.
 */
struct net_buf *spinel_rx_buf_alloc(void);

/** Hand a frame received in a buffer of spinel_rx_buf_alloc() to the core. */
void spinel_rx_buf_submit(struct net_buf *buf);

/** @} */

/** Interface counters, in DRV_STATS field order. */
enum spinel_stat {
	SPINEL_STAT_OPS_ACCEPTED,
	SPINEL_STAT_OPS_REJECTED,
	SPINEL_STAT_EVENTS_SENT,
	SPINEL_STAT_EVENTS_DROPPED,
	SPINEL_STAT_FRAMES_RX,
	SPINEL_STAT_FRAMES_REJECTED,
	SPINEL_STAT_TRANSPORT_ERRORS,
	SPINEL_STAT_COUNT,
};

/** Increment counter @p s. ISR-safe. */
void spinel_stat_inc(enum spinel_stat s);

/** Queue @p buf, a complete Spinel frame, for the host. The transport unrefs it. */
int spinel_send_frame(struct net_buf *buf);

/**
 * Pack and queue a frame: header with @p tid, @p cmd, @p prop, then the
 * values described by the Spinel format @p fmt. Blocks for a TX buffer.
 */
int spinel_reply(uint8_t tid, uint32_t cmd, spinel_prop_key_t prop, const char *fmt, ...);

/** Reply PROP_VALUE_IS(LAST_STATUS, @p status). */
int spinel_reply_status(uint8_t tid, uint32_t status);

/** Spinel status for a negative errno returned by the driver. */
uint32_t spinel_status_from_errno(int err);

/** @return the DECT NR+ network interface driven by Spinel. */
struct net_if *spinel_iface(void);

/**
 * @name Driver operation broker
 *
 * Driver operations queued by the host each own a slot until the driver
 * reports them finished.
 * @{
 */

/**
 * Schedule the operation described by a DRV_OP_QUEUE value and reply to the
 * host: PROP_VALUE_INSERTED with the token on success, LAST_STATUS otherwise.
 */
void spinel_ops_insert(uint8_t tid, const uint8_t *value, size_t len);

/** Driver event handler: forwards the event to the host as DRV_EVENT. */
void spinel_ops_event_cb(struct net_if *iface, const struct dectnrp_driver_event *evt);

/**
 * Wait until every operation has finished, after the driver was stopped.
 * Slots still in use after @p timeout are freed anyway.
 *
 * @return 0, or -ETIMEDOUT if slots had to be freed.
 */
int spinel_ops_reset(k_timeout_t timeout);

/** @return the number of operations in flight. */
size_t spinel_ops_in_flight(void);

/** @} */

#ifdef CONFIG_DECTNRP_SPINEL_TRANSPORT_NONE
/**
 * Pop the next frame the core sent (test transport).
 *
 * @return the frame length, -EAGAIN on timeout, or -EMSGSIZE if the frame
 *         does not fit in @p size bytes (it is dropped).
 */
int spinel_test_recv(uint8_t *buf, size_t size, k_timeout_t timeout);

/**
 * Block or unblock the TX thread (test transport). While blocked, frames
 * stay queued in the core.
 */
void spinel_test_block_tx(bool block);
#endif

#endif /* DECTNRP_SPINEL_INTERNAL_H_ */

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

/**
 * Send one Spinel frame to the host, blocking until it is handed to the
 * hardware. Called from the TX thread only.
 */
int spinel_transport_send(const uint8_t *frame, size_t len);

/**
 * Hand one Spinel frame received from the host to the core. ISR-safe: the
 * frame is copied, so @p frame can be reused on return.
 */
void spinel_rx_submit(const uint8_t *frame, size_t len);

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

/** Queue @p buf, a complete Spinel frame, for the TX thread, which unrefs it. */
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

#ifdef CONFIG_DECTNRP_SPINEL_TRANSPORT_NONE
/**
 * Pop the next frame the core sent (test transport).
 *
 * @return the frame length, -EAGAIN on timeout, or -EMSGSIZE if the frame
 *         does not fit in @p size bytes (it is dropped).
 */
int spinel_test_recv(uint8_t *buf, size_t size, k_timeout_t timeout);
#endif

#endif /* DECTNRP_SPINEL_INTERNAL_H_ */

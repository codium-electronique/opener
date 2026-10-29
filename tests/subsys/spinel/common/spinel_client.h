/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Host side of the Spinel tests.
 *
 * Builds requests, hands them to the core as if they came from the host, and
 * reads what the core sent back through the test transport.
 */

#ifndef DECTNRP_TESTS_SPINEL_CLIENT_H_
#define DECTNRP_TESTS_SPINEL_CLIENT_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "spinel.h"
#include "spinel_internal.h"

/** Time to wait for a frame from the core. */
#define CLIENT_TIMEOUT K_MSEC(500)

/** Hand a raw frame to the core. */
void send_raw(const uint8_t *frame, size_t len);

/** Send a host request: header with @p tid, @p cmd, @p prop, then @p fmt values. */
void send_req(uint8_t tid, uint32_t cmd, spinel_prop_key_t prop, const char *fmt, ...);

/**
 * Parse the header of a frame sent by the core.
 *
 * @return the payload length; @p payload points to it.
 */
int parse_reply(const uint8_t *frame, int len, uint8_t *tid, unsigned int *cmd, unsigned int *prop,
		const uint8_t **payload);

/**
 * Receive the next frame sent by the core and parse its header. The payload
 * stays valid until the next receive.
 *
 * @return the payload length.
 */
int recv_reply(uint8_t *tid, unsigned int *cmd, unsigned int *prop, const uint8_t **payload);

/** Receive a PROP_VALUE_IS for @p prop with @p tid; return its payload length. */
int recv_is(uint8_t tid, spinel_prop_key_t prop, const uint8_t **payload);

/** Receive LAST_STATUS with @p tid and check it is @p status. */
void expect_status(uint8_t tid, unsigned int status);

/** Check that the core sends nothing within @p timeout. */
void expect_no_frame(k_timeout_t timeout);

/** Read counter @p stat through DRV_STATS. */
uint32_t read_stat(enum spinel_stat stat);

/** Drop every frame the core already sent. */
void drain_frames(void);

#endif /* DECTNRP_TESTS_SPINEL_CLIENT_H_ */

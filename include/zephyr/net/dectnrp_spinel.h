/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief DECT NR+ Spinel identifiers.
 *
 * Property, capability, status and wire values of the DECT NR+ Spinel
 * interface. This header is the single source of truth: host tools mirror it.
 * The protocol is described in doc/spinel.rst.
 */

#ifndef ZEPHYR_INCLUDE_NET_DECTNRP_SPINEL_H_
#define ZEPHYR_INCLUDE_NET_DECTNRP_SPINEL_H_

#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief DECT NR+ vendor properties.
 *
 * The driver level uses the vendor sub-range 0x3D00-0x3DFF.
 */
enum dectnrp_spinel_prop {
	/** Driver started (b), get/set. */
	DECTNRP_SPINEL_PROP_DRV_ENABLED = 0x3D00,
	/** Driver capabilities (DECTNRP_SPINEL_FMT_DRV_CAPS), get. */
	DECTNRP_SPINEL_PROP_DRV_CAPS = 0x3D01,
	/** Modem time in modem ticks (X), get. */
	DECTNRP_SPINEL_PROP_DRV_TIME = 0x3D02,
	/** Network ID (L), get/set. */
	DECTNRP_SPINEL_PROP_DRV_NETWORK_ID = 0x3D03,
	/** Short network ID filter (bC: active, value), get/set. */
	DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_NWID = 0x3D04,
	/** Short receiver address filter (bS: active, value), get/set. */
	DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_RXADDR = 0x3D05,
	/** Driver operation queue, insert. */
	DECTNRP_SPINEL_PROP_DRV_OP_QUEUE = 0x3D10,
	/** Driver event stream, unsolicited. */
	DECTNRP_SPINEL_PROP_DRV_EVENT = 0x3D11,
	/** Interface counters (DECTNRP_SPINEL_FMT_DRV_STATS), get. */
	DECTNRP_SPINEL_PROP_DRV_STATS = 0x3D20,
};

/** Capability: driver-level properties. */
#define DECTNRP_SPINEL_CAP_DRV 0x3D00
/** Capability: MAC-level properties, reserved. */
#define DECTNRP_SPINEL_CAP_MAC 0x3E00
/** Capability: DLC-level properties, reserved. */
#define DECTNRP_SPINEL_CAP_DLC 0x3F00
/** Capability: CVG-level properties, reserved. */
#define DECTNRP_SPINEL_CAP_CVG 0x3F80

/** Vendor status of a driver errno: base + positive errno, capped at 0x3CFF. */
#define DECTNRP_SPINEL_STATUS_ERRNO_BASE 0x3C80

/** PROP_INTERFACE_TYPE value for DECT NR+ ("NR"). */
#define DECTNRP_SPINEL_INTERFACE_TYPE 0x4E52

/** Driver operation type, DRV_OP_QUEUE type field. */
enum dectnrp_spinel_op_type {
	/** Transmission. */
	DECTNRP_SPINEL_OP_TX = 0,
	/** Reception. */
	DECTNRP_SPINEL_OP_RX = 1,
	/** RSSI measurement. */
	DECTNRP_SPINEL_OP_RSSI1 = 2,
};

/** Op flag: start at start_time, else immediately. */
#define DECTNRP_SPINEL_OP_FLAG_SCHEDULED          BIT(0)
/** Op flag: listen before talk, TX only. */
#define DECTNRP_SPINEL_OP_FLAG_LBT                BIT(1)
/** Op flag: HARQ, TX only. */
#define DECTNRP_SPINEL_OP_FLAG_HARQ               BIT(2)
/** Op flag: beacon, TX and RX. */
#define DECTNRP_SPINEL_OP_FLAG_BEACON             BIT(3)
/** Op flags: RX reception mode, enum dectnrp_rx_reception, 0 for TX/RSSI1. */
#define DECTNRP_SPINEL_OP_FLAG_RX_RECEPTION_SHIFT 4
/** Op flags: RX reception mode mask. */
#define DECTNRP_SPINEL_OP_FLAG_RX_RECEPTION_MASK  (0x3 << DECTNRP_SPINEL_OP_FLAG_RX_RECEPTION_SHIFT)

/**
 * @brief DRV_EVENT code.
 *
 * The values differ from enum dectnrp_event_type: always map explicitly.
 */
enum dectnrp_spinel_event_code {
	/** Message received. */
	DECTNRP_SPINEL_EVENT_MSG_RECEIVED = 1,
	/** Reception error. */
	DECTNRP_SPINEL_EVENT_MSG_ERROR = 2,
	/** RSSI measurement results. */
	DECTNRP_SPINEL_EVENT_OP_RESULTS = 3,
	/** Operation finished, always the last event of an op. */
	DECTNRP_SPINEL_EVENT_OP_FINISHED = 4,
};

/** MSG_RECEIVED / MSG_ERROR valid bit: rssi2. */
#define DECTNRP_SPINEL_RX_VALID_RSSI2 BIT(0)
/** MSG_RECEIVED / MSG_ERROR valid bit: snr. */
#define DECTNRP_SPINEL_RX_VALID_SNR   BIT(1)
/** MSG_ERROR valid bit: pcc. */
#define DECTNRP_SPINEL_RX_VALID_PCC   BIT(2)

/** DRV_OP_QUEUE prefix: token, type, channel, flags, start_time. */
#define DECTNRP_SPINEL_FMT_OP_PREFIX "SCSCX"
/**
 * DRV_OP_QUEUE TX suffix: phy_header_type, tbs, mu, beta, mcs, slots,
 * lbt_period, lbt_rssi_threshold, data.
 */
#define DECTNRP_SPINEL_FMT_OP_TX     "CSCCCCLCd"
/** DRV_OP_QUEUE RX suffix: duration, expected_rssi. */
#define DECTNRP_SPINEL_FMT_OP_RX     "LC"
/** DRV_OP_QUEUE RSSI1 suffix: subslots. */
#define DECTNRP_SPINEL_FMT_OP_RSSI1  "L"

/** DRV_EVENT prefix: token, code, status, dropped. */
#define DECTNRP_SPINEL_FMT_EVT_PREFIX       "SClS"
/**
 * DRV_EVENT MSG_RECEIVED suffix: phy_type, valid, start_time, rssi2, snr,
 * channel, pcc, pdc.
 */
#define DECTNRP_SPINEL_FMT_EVT_MSG_RECEIVED "CCXCcSdd"
/**
 * DRV_EVENT MSG_ERROR suffix: phy_type, valid, start_time, rssi2, snr,
 * channel, pcc.
 */
#define DECTNRP_SPINEL_FMT_EVT_MSG_ERROR    "CCXCcSd"
/** DRV_EVENT OP_RESULTS suffix: channel, rssi1. */
#define DECTNRP_SPINEL_FMT_EVT_OP_RESULTS   "Sd"

/**
 * DRV_CAPS: hw_caps, release, power class max, NSS RX max, RX/TX diversity
 * max, RX gain, MCS max, soft buffer size, HARQ processes max, HARQ feedback
 * delay, mu max, beta max, modem ticks per symbol, max frame size, op slots,
 * then the latencies in modem ticks: scheduled-op transition and startup, RX
 * startup and shutdown, RSSI1 shutdown, TX startup and shutdown, stop to RF
 * off.
 */
#define DECTNRP_SPINEL_FMT_DRV_CAPS  "LCCCCcCLCCCCLSCLLLLLLLL"
/**
 * DRV_STATS: ops accepted, ops rejected, events sent, events dropped, frames
 * received, frames rejected, transport errors.
 */
#define DECTNRP_SPINEL_FMT_DRV_STATS "LLLLLLL"

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NET_DECTNRP_SPINEL_H_ */

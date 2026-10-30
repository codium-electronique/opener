/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Spinel core: RX and TX threads, command dispatch, identity and
 * driver-level properties. Driver operations and events are handled by the
 * op broker (spinel_ops.c).
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dectnrp_driver.h>
#include <zephyr/net/dectnrp_net_l2.h>
#include <zephyr/net/dectnrp_spinel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#ifdef OPENER_HAS_BUILD_INFO
#include <opener/build_info.h>
#endif

#include "spinel_internal.h"

LOG_MODULE_REGISTER(dectnrp_spinel, CONFIG_DECTNRP_SPINEL_LOG_LEVEL);

#define STATUS_ERRNO_MAX 0x3CFF

NET_BUF_POOL_DEFINE(spinel_rx_pool, CONFIG_DECTNRP_SPINEL_RX_BUFS,
		    CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE, 0, NULL);
NET_BUF_POOL_DEFINE(spinel_tx_pool, CONFIG_DECTNRP_SPINEL_TX_BUFS,
		    CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE, 0, NULL);

static K_FIFO_DEFINE(rx_fifo);
static K_FIFO_DEFINE(tx_fifo);

static atomic_t stats[SPINEL_STAT_COUNT];

static struct net_if *iface;
static char ncp_version[96];

/* Driver settings, as last set by the host. */
static struct {
	bool enabled;
	uint32_t network_id;
	bool nwid_filter;
	uint8_t nwid;
	bool rxaddr_filter;
	uint16_t rxaddr;
} drv;

static const struct device *drv_dev(void)
{
	return net_if_get_device(iface);
}

static const struct dectnrp_driver_api *drv_api(void)
{
	const struct device *dev = drv_dev();

	return dev->api;
}

struct net_if *spinel_iface(void)
{
	return iface;
}

void spinel_stat_inc(enum spinel_stat s)
{
	atomic_inc(&stats[s]);
}

uint32_t spinel_status_from_errno(int err)
{
	uint32_t errno_value;
	uint32_t status;

	switch (err) {
	case 0:
		return SPINEL_STATUS_OK;
	case -EALREADY:
		return SPINEL_STATUS_ALREADY;
	case -ENOMEM:
		return SPINEL_STATUS_NOMEM;
	case -EBUSY:
		return SPINEL_STATUS_BUSY;
	case -EINVAL:
		return SPINEL_STATUS_INVALID_ARGUMENT;
	default:
		break;
	}

	/* Other errnos map to the vendor range: base + errno, capped. */
	if (err < 0) {
		errno_value = -err;
	} else {
		errno_value = err;
	}
	status = DECTNRP_SPINEL_STATUS_ERRNO_BASE + errno_value;
	if (status > STATUS_ERRNO_MAX) {
		status = STATUS_ERRNO_MAX;
	}

	return status;
}

int spinel_send_frame(struct net_buf *buf)
{
	k_fifo_put(&tx_fifo, buf);

	return 0;
}

int spinel_reply(uint8_t tid, uint32_t cmd, spinel_prop_key_t prop, const char *fmt, ...)
{
	struct net_buf *buf;
	uint8_t header;
	size_t room;
	spinel_ssize_t n;
	va_list ap;

	buf = net_buf_alloc(&spinel_tx_pool, K_FOREVER);
	header = SPINEL_HEADER_FLAG | tid;

	/* Spinel header, command and property. */
	room = net_buf_tailroom(buf);
	n = spinel_datatype_pack(net_buf_tail(buf), room, "Cii", header, cmd, prop);
	if (n < 0 || n > room) {
		goto overflow;
	}
	net_buf_add(buf, n);

	/* Property value. */
	room = net_buf_tailroom(buf);
	va_start(ap, fmt);
	n = spinel_datatype_vpack(net_buf_tail(buf), room, fmt, ap);
	va_end(ap);
	if (n < 0 || n > room) {
		goto overflow;
	}
	net_buf_add(buf, n);

	return spinel_send_frame(buf);

overflow:
	LOG_ERR("reply to prop 0x%x does not fit", prop);
	net_buf_unref(buf);
	return -ENOMEM;
}

int spinel_reply_status(uint8_t tid, uint32_t status)
{
	return spinel_reply(tid, SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_LAST_STATUS,
			    SPINEL_DATATYPE_UINT_PACKED_S, status);
}

struct net_buf *spinel_rx_buf_alloc(void)
{
	return net_buf_alloc(&spinel_rx_pool, K_NO_WAIT);
}

void spinel_rx_buf_submit(struct net_buf *buf)
{
	k_fifo_put(&rx_fifo, buf);
}

void spinel_rx_submit(const uint8_t *frame, size_t len)
{
	struct net_buf *buf;

	if (len > CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE) {
		spinel_stat_inc(SPINEL_STAT_FRAMES_REJECTED);
		return;
	}

	buf = spinel_rx_buf_alloc();
	if (buf == NULL) {
		spinel_stat_inc(SPINEL_STAT_TRANSPORT_ERRORS);
		return;
	}
	net_buf_add_mem(buf, frame, len);
	spinel_rx_buf_submit(buf);
}

static void reply_drv_enabled(uint8_t tid)
{
	spinel_reply(tid, SPINEL_CMD_PROP_VALUE_IS, DECTNRP_SPINEL_PROP_DRV_ENABLED,
		     SPINEL_DATATYPE_BOOL_S, drv.enabled);
}

static void reply_drv_caps(uint8_t tid)
{
	const struct device *dev = drv_dev();
	const struct dectnrp_driver_api *api = drv_api();
	struct dectnrp_device_capabilities caps;
	uint32_t status;
	int err;

	err = api->get_capabilities(dev, &caps);
	if (err != 0) {
		status = spinel_status_from_errno(err);
		spinel_reply_status(tid, status);
		return;
	}

	/* Field order: DECTNRP_SPINEL_FMT_DRV_CAPS. */
	spinel_reply(
		tid, SPINEL_CMD_PROP_VALUE_IS, DECTNRP_SPINEL_PROP_DRV_CAPS,
		DECTNRP_SPINEL_FMT_DRV_CAPS,
		/* Driver features. */
		(uint32_t)caps.hw_caps,
		/* Radio device capabilities. */
		CONFIG_DECTNRP_DRIVER_RDCAP_DECTNRP_RELEASE,
		CONFIG_DECTNRP_DRIVER_RDCAP_POWERCLASS_MAX, CONFIG_DECTNRP_DRIVER_RDCAP_NSS_RX_MAX,
		CONFIG_DECTNRP_DRIVER_RDCAP_RXTX_DIVERSITY_MAX,
		CONFIG_DECTNRP_DRIVER_RDCAP_RXGAIN_DB, CONFIG_DECTNRP_DRIVER_RDCAP_MCS_MAX,
		(uint32_t)CONFIG_DECTNRP_DRIVER_RDCAP_SOFTBUFFERSIZE_BYTES_MAX,
		CONFIG_DECTNRP_DRIVER_RDCAP_HARQ_PROC_MAX,
		CONFIG_DECTNRP_DRIVER_RDCAP_HARQ_FEEDBACKDELAY, CONFIG_DECTNRP_DRIVER_RDCAP_MU_MAX,
		CONFIG_DECTNRP_DRIVER_RDCAP_BETA_MAX,
		(uint32_t)CONFIG_DECTNRP_DRIVER_MODEMTICKS_PER_SYMBOL,
		/* Spinel interface limits. */
		CONFIG_DECTNRP_SPINEL_MAX_FRAME_SIZE, CONFIG_DECTNRP_SPINEL_OP_SLOTS,
		/* Radio latencies, in modem ticks. */
		caps.sched_op_transition, caps.sched_op_startup, caps.rx_startup, caps.rx_shutdown,
		caps.rssi_shutdown, caps.tx_startup, caps.tx_shutdown, caps.stop_to_rf_off);
}

static void reply_drv_time(uint8_t tid)
{
	const struct device *dev = drv_dev();
	const struct dectnrp_driver_api *api = drv_api();
	net_time_t time;
	uint32_t status;
	int err;

	err = api->get_time(dev, &time);
	if (err != 0) {
		status = spinel_status_from_errno(err);
		spinel_reply_status(tid, status);
		return;
	}

	spinel_reply(tid, SPINEL_CMD_PROP_VALUE_IS, DECTNRP_SPINEL_PROP_DRV_TIME,
		     SPINEL_DATATYPE_UINT64_S, (uint64_t)time);
}

static void reply_drv_stats(uint8_t tid)
{
	uint32_t values[SPINEL_STAT_COUNT];

	for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
		values[i] = atomic_get(&stats[i]);
	}

	spinel_reply(tid, SPINEL_CMD_PROP_VALUE_IS, DECTNRP_SPINEL_PROP_DRV_STATS,
		     DECTNRP_SPINEL_FMT_DRV_STATS, values[SPINEL_STAT_OPS_ACCEPTED],
		     values[SPINEL_STAT_OPS_REJECTED], values[SPINEL_STAT_EVENTS_SENT],
		     values[SPINEL_STAT_EVENTS_DROPPED], values[SPINEL_STAT_FRAMES_RX],
		     values[SPINEL_STAT_FRAMES_REJECTED], values[SPINEL_STAT_TRANSPORT_ERRORS]);
}

static void prop_get(uint8_t tid, spinel_prop_key_t prop)
{
	switch (prop) {
	case SPINEL_PROP_LAST_STATUS:
		spinel_reply_status(tid, SPINEL_STATUS_OK);
		break;
	case SPINEL_PROP_PROTOCOL_VERSION:
		spinel_reply(tid, SPINEL_CMD_PROP_VALUE_IS, prop, "ii",
			     SPINEL_PROTOCOL_VERSION_THREAD_MAJOR,
			     SPINEL_PROTOCOL_VERSION_THREAD_MINOR);
		break;
	case SPINEL_PROP_NCP_VERSION:
		spinel_reply(tid, SPINEL_CMD_PROP_VALUE_IS, prop, SPINEL_DATATYPE_UTF8_S,
			     ncp_version);
		break;
	case SPINEL_PROP_INTERFACE_TYPE:
		spinel_reply(tid, SPINEL_CMD_PROP_VALUE_IS, prop, SPINEL_DATATYPE_UINT_PACKED_S,
			     DECTNRP_SPINEL_INTERFACE_TYPE);
		break;
	case SPINEL_PROP_VENDOR_ID:
		spinel_reply(tid, SPINEL_CMD_PROP_VALUE_IS, prop, SPINEL_DATATYPE_UINT_PACKED_S, 0);
		break;
	case SPINEL_PROP_CAPS:
		spinel_reply(tid, SPINEL_CMD_PROP_VALUE_IS, prop, SPINEL_DATATYPE_UINT_PACKED_S,
			     DECTNRP_SPINEL_CAP_DRV);
		break;
	case DECTNRP_SPINEL_PROP_DRV_ENABLED:
		reply_drv_enabled(tid);
		break;
	case DECTNRP_SPINEL_PROP_DRV_CAPS:
		reply_drv_caps(tid);
		break;
	case DECTNRP_SPINEL_PROP_DRV_TIME:
		reply_drv_time(tid);
		break;
	case DECTNRP_SPINEL_PROP_DRV_NETWORK_ID:
		spinel_reply(tid, SPINEL_CMD_PROP_VALUE_IS, prop, SPINEL_DATATYPE_UINT32_S,
			     drv.network_id);
		break;
	case DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_NWID:
		spinel_reply(tid, SPINEL_CMD_PROP_VALUE_IS, prop, "bC", drv.nwid_filter, drv.nwid);
		break;
	case DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_RXADDR:
		spinel_reply(tid, SPINEL_CMD_PROP_VALUE_IS, prop, "bS", drv.rxaddr_filter,
			     drv.rxaddr);
		break;
	case DECTNRP_SPINEL_PROP_DRV_STATS:
		reply_drv_stats(tid);
		break;
	default:
		spinel_reply_status(tid, SPINEL_STATUS_PROP_NOT_FOUND);
		break;
	}
}

static int set_drv_enabled(bool enable)
{
	const struct device *dev = drv_dev();
	const struct dectnrp_driver_api *api = drv_api();
	int err;

	if (enable) {
		err = api->start(dev);
	} else {
		err = api->stop(dev);
	}

	/* Already in the requested state is not an error. */
	if (err == -EALREADY) {
		err = 0;
	}
	if (err == 0) {
		drv.enabled = enable;
	}

	return err;
}

static int drv_configure(enum dectnrp_config_type type, const struct dectnrp_config *config)
{
	const struct device *dev = drv_dev();
	const struct dectnrp_driver_api *api = drv_api();

	return api->configure(dev, type, config);
}

/*
 * Property setters: unpack the value, apply it to the driver, and return a
 * Spinel status. On success the caller replies with the new value.
 */

static uint32_t set_prop_drv_enabled(const uint8_t *data, size_t len)
{
	bool enable;
	spinel_ssize_t n;
	int err;

	n = spinel_datatype_unpack(data, len, SPINEL_DATATYPE_BOOL_S, &enable);
	if (n < 0) {
		return SPINEL_STATUS_PARSE_ERROR;
	}

	err = set_drv_enabled(enable);

	return spinel_status_from_errno(err);
}

static uint32_t set_prop_drv_network_id(const uint8_t *data, size_t len)
{
	struct dectnrp_config config;
	spinel_ssize_t n;
	int err;

	n = spinel_datatype_unpack(data, len, SPINEL_DATATYPE_UINT32_S, &config.network_id);
	if (n < 0) {
		return SPINEL_STATUS_PARSE_ERROR;
	}

	err = drv_configure(DECTNRP_CONFIG_TYPE_NETWORK_ID, &config);
	if (err == 0) {
		drv.network_id = config.network_id;
	}

	return spinel_status_from_errno(err);
}

static uint32_t set_prop_drv_filter_short_nwid(const uint8_t *data, size_t len)
{
	struct dectnrp_config config;
	spinel_ssize_t n;
	int err;

	n = spinel_datatype_unpack(data, len, "bC", &config.filter_short_network_id.activate,
				   &config.filter_short_network_id.value);
	if (n < 0) {
		return SPINEL_STATUS_PARSE_ERROR;
	}

	err = drv_configure(DECTNRP_CONFIG_TYPE_FILTER_SHORT_NETWORK_ID, &config);
	if (err == 0) {
		drv.nwid_filter = config.filter_short_network_id.activate;
		drv.nwid = config.filter_short_network_id.value;
	}

	return spinel_status_from_errno(err);
}

static uint32_t set_prop_drv_filter_short_rxaddr(const uint8_t *data, size_t len)
{
	struct dectnrp_config config;
	spinel_ssize_t n;
	int err;

	n = spinel_datatype_unpack(data, len, "bS", &config.filter_short_receiver_addr.activate,
				   &config.filter_short_receiver_addr.value);
	if (n < 0) {
		return SPINEL_STATUS_PARSE_ERROR;
	}

	err = drv_configure(DECTNRP_CONFIG_TYPE_FILTER_SHORT_RECEIVER_ADDR, &config);
	if (err == 0) {
		drv.rxaddr_filter = config.filter_short_receiver_addr.activate;
		drv.rxaddr = config.filter_short_receiver_addr.value;
	}

	return spinel_status_from_errno(err);
}

static void prop_set(uint8_t tid, spinel_prop_key_t prop, const uint8_t *data, size_t len)
{
	uint32_t status;

	switch (prop) {
	case DECTNRP_SPINEL_PROP_DRV_ENABLED:
		status = set_prop_drv_enabled(data, len);
		break;
	case DECTNRP_SPINEL_PROP_DRV_NETWORK_ID:
		status = set_prop_drv_network_id(data, len);
		break;
	case DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_NWID:
		status = set_prop_drv_filter_short_nwid(data, len);
		break;
	case DECTNRP_SPINEL_PROP_DRV_FILTER_SHORT_RXADDR:
		status = set_prop_drv_filter_short_rxaddr(data, len);
		break;
	default:
		status = SPINEL_STATUS_PROP_NOT_FOUND;
		break;
	}

	if (status != SPINEL_STATUS_OK) {
		spinel_reply_status(tid, status);
		return;
	}

	/* Reply with the value now in effect. */
	prop_get(tid, prop);
}

static void prop_insert(uint8_t tid, spinel_prop_key_t prop, const uint8_t *data, size_t len)
{
	switch (prop) {
	case DECTNRP_SPINEL_PROP_DRV_OP_QUEUE:
		spinel_ops_insert(tid, data, len);
		break;
	default:
		spinel_reply_status(tid, SPINEL_STATUS_PROP_NOT_FOUND);
		break;
	}
}

/* Deactivate the RX filters, so that a new host receives every frame. */
static void clear_filters(void)
{
	struct dectnrp_config config;
	int err;

	config.filter_short_network_id.activate = false;
	config.filter_short_network_id.value = 0;
	err = drv_configure(DECTNRP_CONFIG_TYPE_FILTER_SHORT_NETWORK_ID, &config);
	if (err == 0) {
		drv.nwid_filter = false;
		drv.nwid = 0;
	} else {
		LOG_WRN("cannot clear the short network ID filter (%d)", err);
	}

	config.filter_short_receiver_addr.activate = false;
	config.filter_short_receiver_addr.value = 0;
	err = drv_configure(DECTNRP_CONFIG_TYPE_FILTER_SHORT_RECEIVER_ADDR, &config);
	if (err == 0) {
		drv.rxaddr_filter = false;
		drv.rxaddr = 0;
	} else {
		LOG_WRN("cannot clear the short receiver address filter (%d)", err);
	}
}

static void cmd_reset(void)
{
	k_timeout_t timeout = K_MSEC(CONFIG_DECTNRP_SPINEL_RESET_TIMEOUT_MS);
	int err;

	/* Stopping the driver cancels its operations: each one then finishes. */
	err = set_drv_enabled(false);
	if (err != 0) {
		LOG_WRN("cannot stop the driver (%d)", err);
	}

	/* The OP_FINISHED events are sent before the reset notification. */
	(void)spinel_ops_reset(timeout);

	clear_filters();

	spinel_reply_status(0, SPINEL_STATUS_RESET_SOFTWARE);
}

static void dispatch(const uint8_t *frame, size_t len)
{
	uint8_t header;
	unsigned int cmd;
	unsigned int prop;
	uint8_t flags;
	uint8_t iid;
	uint8_t tid;
	spinel_ssize_t n;

	spinel_stat_inc(SPINEL_STAT_FRAMES_RX);

	n = spinel_datatype_unpack(frame, len, "Ci", &header, &cmd);
	if (n < 0) {
		spinel_stat_inc(SPINEL_STAT_FRAMES_REJECTED);
		return;
	}

	/* Only host requests (FLG = 0b10) on interface 0 are accepted. */
	flags = header & SPINEL_HEADER_FLAGS_MASK;
	iid = SPINEL_HEADER_GET_IID(header);
	if (flags != SPINEL_HEADER_FLAG || iid != 0) {
		spinel_stat_inc(SPINEL_STAT_FRAMES_REJECTED);
		return;
	}
	tid = SPINEL_HEADER_GET_TID(header);
	frame += n;
	len -= n;

	switch (cmd) {
	case SPINEL_CMD_NOOP:
		spinel_reply_status(tid, SPINEL_STATUS_OK);
		return;
	case SPINEL_CMD_RESET:
		cmd_reset();
		return;
	case SPINEL_CMD_PROP_VALUE_GET:
	case SPINEL_CMD_PROP_VALUE_SET:
	case SPINEL_CMD_PROP_VALUE_INSERT:
		break;
	default:
		spinel_reply_status(tid, SPINEL_STATUS_INVALID_COMMAND);
		return;
	}

	n = spinel_datatype_unpack(frame, len, SPINEL_DATATYPE_UINT_PACKED_S, &prop);
	if (n < 0) {
		spinel_reply_status(tid, SPINEL_STATUS_PARSE_ERROR);
		return;
	}
	frame += n;
	len -= n;

	if (cmd == SPINEL_CMD_PROP_VALUE_GET) {
		prop_get(tid, prop);
	} else if (cmd == SPINEL_CMD_PROP_VALUE_SET) {
		prop_set(tid, prop, frame, len);
	} else {
		prop_insert(tid, prop, frame, len);
	}
}

static void rx_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct net_buf *buf;

	while (true) {
		buf = k_fifo_get(&rx_fifo, K_FOREVER);
		dispatch(buf->data, buf->len);
		net_buf_unref(buf);
	}
}

static void tx_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct net_buf *buf;
	int err;

	while (true) {
		buf = k_fifo_get(&tx_fifo, K_FOREVER);
		err = spinel_transport_send(buf->data, buf->len);
		if (err != 0) {
			spinel_stat_inc(SPINEL_STAT_TRANSPORT_ERRORS);
		}
		net_buf_unref(buf);
	}
}

static K_THREAD_STACK_DEFINE(rx_stack, CONFIG_DECTNRP_SPINEL_THREAD_STACK_SIZE);
static K_THREAD_STACK_DEFINE(tx_stack, CONFIG_DECTNRP_SPINEL_THREAD_STACK_SIZE);
static struct k_thread rx_thread;
static struct k_thread tx_thread;

static void init_ncp_version(void)
{
	const struct device *dev = drv_dev();

#ifdef OPENER_HAS_BUILD_INFO
	const char *dirty_mark;

	if (OPENER_DIRTY) {
		dirty_mark = "+";
	} else {
		dirty_mark = "";
	}

	/* "OPENER/<major>.<minor>.<patch>-<commit>[+]; <driver>; <build date>" */
	snprintf(ncp_version, sizeof(ncp_version), "OPENER/%d.%d.%d-%s%s; %s; %s",
		 OPENER_VERSION_MAJOR, OPENER_VERSION_MINOR, OPENER_PATCHLEVEL,
		 STRINGIFY(OPENER_GIT_COMMIT), dirty_mark, dev->name, __DATE__);
#else
	snprintf(ncp_version, sizeof(ncp_version), "OPENER/unknown; %s; %s", dev->name, __DATE__);
#endif
}

static int spinel_init(void)
{
	struct dectnrp_config config = {.event_handler = spinel_ops_event_cb};
	int err;

	iface = net_if_get_first_by_type(&NET_L2_GET_NAME(DECTNRP));
	if (iface == NULL) {
		LOG_ERR("no DECT NR+ interface, Spinel not started");
		return 0;
	}

	init_ncp_version();

	err = drv_configure(DECTNRP_CONFIG_TYPE_EVENT_HANDLER, &config);
	if (err != 0) {
		LOG_ERR("cannot register the driver event handler (%d)", err);
		return 0;
	}

	err = spinel_transport_init();
	if (err != 0) {
		LOG_ERR("transport init failed (%d)", err);
		return 0;
	}

	/* Static threads (K_THREAD_DEFINE) are only created after APPLICATION init. */
	k_thread_create(&rx_thread, rx_stack, K_THREAD_STACK_SIZEOF(rx_stack), rx_thread_fn, NULL,
			NULL, NULL, CONFIG_DECTNRP_SPINEL_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread, "spinel_rx");
	k_thread_create(&tx_thread, tx_stack, K_THREAD_STACK_SIZEOF(tx_stack), tx_thread_fn, NULL,
			NULL, NULL, CONFIG_DECTNRP_SPINEL_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&tx_thread, "spinel_tx");

	spinel_reply(0, SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_LAST_STATUS,
		     SPINEL_DATATYPE_UINT_PACKED_S, SPINEL_STATUS_RESET_POWER_ON);

	return 0;
}

SYS_INIT(spinel_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

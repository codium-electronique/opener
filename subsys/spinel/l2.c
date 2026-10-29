/*
 * Copyright (c) 2026 Codium Electronique
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DECTNRP L2 for the driver-level Spinel interface.
 *
 * At driver level the L2 lives on the Spinel host, so the Spinel interface
 * owns the device-side L2 hook the driver calls at init. Without Spinel at
 * driver level the application provides the L2 instead.
 */

#include <errno.h>

#include <zephyr/net/dectnrp.h>
#include <zephyr/net/dectnrp_driver.h>
#include <zephyr/net/dectnrp_net_l2.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_l2.h>

static enum net_verdict dectnrp_recv(struct net_if *iface, struct net_pkt *pkt)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(pkt);

	return NET_DROP;
}

static int dectnrp_send(struct net_if *iface, struct net_pkt *pkt)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(pkt);

	return -ENOTSUP;
}

static int dectnrp_enable(struct net_if *iface, bool state)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(state);

	return 0;
}

static enum net_l2_flags dectnrp_flags(struct net_if *iface)
{
	ARG_UNUSED(iface);

	return 0;
}

NET_L2_INIT(DECTNRP_L2, dectnrp_recv, dectnrp_send, dectnrp_enable, dectnrp_flags);

void dectnrp_l2_init(struct net_if *iface)
{
	struct dectnrp_context *ctx = net_if_l2_data(iface);

	ctx->iface = iface;

	/* The host starts the radio. The interface stays down: it has no link
	 * address, which Zephyr requires to bring it up.
	 */
	net_if_flag_set(iface, NET_IF_NO_AUTO_START);
}

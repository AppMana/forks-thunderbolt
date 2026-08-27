// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests
 *
 * Copyright (C) 2020, Intel Corporation
 * Author: Mika Westerberg <mika.westerberg@linux.intel.com>
 */

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/idr.h>
#include <linux/kthread.h>

#include "tb.h"
#include "tunnel.h"
#include "dma_port.h"
#include "nhi.h"
#include "nhi_regs.h"
#include "tests/negotiation_model.h"

static int __ida_init(struct kunit_resource *res, void *context)
{
	struct ida *ida = context;

	ida_init(ida);
	res->data = ida;
	return 0;
}

static void __ida_destroy(struct kunit_resource *res)
{
	struct ida *ida = res->data;

	ida_destroy(ida);
}

static void kunit_ida_init(struct kunit *test, struct ida *ida)
{
	kunit_alloc_resource(test, __ida_init, __ida_destroy, GFP_KERNEL, ida);
}

static struct tb_switch *alloc_switch(struct kunit *test, u64 route,
				      u8 upstream_port, u8 max_port_number)
{
	struct tb_switch *sw;
	size_t size;
	int i;

	sw = kunit_kzalloc(test, sizeof(*sw), GFP_KERNEL);
	if (!sw)
		return NULL;

	sw->config.upstream_port_number = upstream_port;
	sw->config.depth = tb_route_length(route);
	sw->config.route_hi = upper_32_bits(route);
	sw->config.route_lo = lower_32_bits(route);
	sw->config.enabled = 0;
	sw->config.max_port_number = max_port_number;

	size = (sw->config.max_port_number + 1) * sizeof(*sw->ports);
	sw->ports = kunit_kzalloc(test, size, GFP_KERNEL);
	if (!sw->ports)
		return NULL;

	for (i = 0; i <= sw->config.max_port_number; i++) {
		sw->ports[i].sw = sw;
		sw->ports[i].port = i;
		sw->ports[i].config.port_number = i;
		if (i) {
			kunit_ida_init(test, &sw->ports[i].in_hopids);
			kunit_ida_init(test, &sw->ports[i].out_hopids);
		}
	}

	return sw;
}

static struct tb_switch *alloc_host(struct kunit *test)
{
	struct tb_switch *sw;

	sw = alloc_switch(test, 0, 7, 13);
	if (!sw)
		return NULL;

	sw->config.vendor_id = 0x8086;
	sw->config.device_id = 0x9a1b;

	sw->ports[0].config.type = TB_TYPE_PORT;
	sw->ports[0].config.max_in_hop_id = 7;
	sw->ports[0].config.max_out_hop_id = 7;

	sw->ports[1].config.type = TB_TYPE_PORT;
	sw->ports[1].config.max_in_hop_id = 19;
	sw->ports[1].config.max_out_hop_id = 19;
	sw->ports[1].total_credits = 60;
	sw->ports[1].ctl_credits = 2;
	sw->ports[1].dual_link_port = &sw->ports[2];

	sw->ports[2].config.type = TB_TYPE_PORT;
	sw->ports[2].config.max_in_hop_id = 19;
	sw->ports[2].config.max_out_hop_id = 19;
	sw->ports[2].total_credits = 60;
	sw->ports[2].ctl_credits = 2;
	sw->ports[2].dual_link_port = &sw->ports[1];
	sw->ports[2].link_nr = 1;

	sw->ports[3].config.type = TB_TYPE_PORT;
	sw->ports[3].config.max_in_hop_id = 19;
	sw->ports[3].config.max_out_hop_id = 19;
	sw->ports[3].total_credits = 60;
	sw->ports[3].ctl_credits = 2;
	sw->ports[3].dual_link_port = &sw->ports[4];

	sw->ports[4].config.type = TB_TYPE_PORT;
	sw->ports[4].config.max_in_hop_id = 19;
	sw->ports[4].config.max_out_hop_id = 19;
	sw->ports[4].total_credits = 60;
	sw->ports[4].ctl_credits = 2;
	sw->ports[4].dual_link_port = &sw->ports[3];
	sw->ports[4].link_nr = 1;

	sw->ports[5].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[5].config.max_in_hop_id = 9;
	sw->ports[5].config.max_out_hop_id = 9;
	sw->ports[5].cap_adap = -1;

	sw->ports[6].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[6].config.max_in_hop_id = 9;
	sw->ports[6].config.max_out_hop_id = 9;
	sw->ports[6].cap_adap = -1;

	sw->ports[7].config.type = TB_TYPE_NHI;
	sw->ports[7].config.max_in_hop_id = 11;
	sw->ports[7].config.max_out_hop_id = 11;
	sw->ports[7].config.nfc_credits = 0x41800000;

	sw->ports[8].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[8].config.max_in_hop_id = 8;
	sw->ports[8].config.max_out_hop_id = 8;

	sw->ports[9].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[9].config.max_in_hop_id = 8;
	sw->ports[9].config.max_out_hop_id = 8;

	sw->ports[10].disabled = true;
	sw->ports[11].disabled = true;

	sw->ports[12].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[12].config.max_in_hop_id = 8;
	sw->ports[12].config.max_out_hop_id = 8;

	sw->ports[13].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[13].config.max_in_hop_id = 8;
	sw->ports[13].config.max_out_hop_id = 8;

	return sw;
}

static struct tb_switch *alloc_host_usb4(struct kunit *test)
{
	struct tb_switch *sw;

	sw = alloc_host(test);
	if (!sw)
		return NULL;

	sw->generation = 4;
	sw->credit_allocation = true;
	sw->max_usb3_credits = 32;
	sw->min_dp_aux_credits = 1;
	sw->min_dp_main_credits = 0;
	sw->max_pcie_credits = 64;
	sw->max_dma_credits = 14;

	return sw;
}

static struct tb_switch *alloc_host_br(struct kunit *test)
{
	struct tb_switch *sw;

	sw = alloc_host_usb4(test);
	if (!sw)
		return NULL;

	sw->ports[10].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[10].config.max_in_hop_id = 9;
	sw->ports[10].config.max_out_hop_id = 9;
	sw->ports[10].cap_adap = -1;
	sw->ports[10].disabled = false;

	return sw;
}

static struct tb_switch *alloc_dev_default(struct kunit *test,
					   struct tb_switch *parent,
					   u64 route, bool bonded)
{
	struct tb_port *port, *upstream_port;
	struct tb_switch *sw;

	sw = alloc_switch(test, route, 1, 19);
	if (!sw)
		return NULL;

	sw->config.vendor_id = 0x8086;
	sw->config.device_id = 0x15ef;

	sw->ports[0].config.type = TB_TYPE_PORT;
	sw->ports[0].config.max_in_hop_id = 8;
	sw->ports[0].config.max_out_hop_id = 8;

	sw->ports[1].config.type = TB_TYPE_PORT;
	sw->ports[1].config.max_in_hop_id = 19;
	sw->ports[1].config.max_out_hop_id = 19;
	sw->ports[1].total_credits = 60;
	sw->ports[1].ctl_credits = 2;
	sw->ports[1].dual_link_port = &sw->ports[2];

	sw->ports[2].config.type = TB_TYPE_PORT;
	sw->ports[2].config.max_in_hop_id = 19;
	sw->ports[2].config.max_out_hop_id = 19;
	sw->ports[2].total_credits = 60;
	sw->ports[2].ctl_credits = 2;
	sw->ports[2].dual_link_port = &sw->ports[1];
	sw->ports[2].link_nr = 1;

	sw->ports[3].config.type = TB_TYPE_PORT;
	sw->ports[3].config.max_in_hop_id = 19;
	sw->ports[3].config.max_out_hop_id = 19;
	sw->ports[3].total_credits = 60;
	sw->ports[3].ctl_credits = 2;
	sw->ports[3].dual_link_port = &sw->ports[4];

	sw->ports[4].config.type = TB_TYPE_PORT;
	sw->ports[4].config.max_in_hop_id = 19;
	sw->ports[4].config.max_out_hop_id = 19;
	sw->ports[4].total_credits = 60;
	sw->ports[4].ctl_credits = 2;
	sw->ports[4].dual_link_port = &sw->ports[3];
	sw->ports[4].link_nr = 1;

	sw->ports[5].config.type = TB_TYPE_PORT;
	sw->ports[5].config.max_in_hop_id = 19;
	sw->ports[5].config.max_out_hop_id = 19;
	sw->ports[5].total_credits = 60;
	sw->ports[5].ctl_credits = 2;
	sw->ports[5].dual_link_port = &sw->ports[6];

	sw->ports[6].config.type = TB_TYPE_PORT;
	sw->ports[6].config.max_in_hop_id = 19;
	sw->ports[6].config.max_out_hop_id = 19;
	sw->ports[6].total_credits = 60;
	sw->ports[6].ctl_credits = 2;
	sw->ports[6].dual_link_port = &sw->ports[5];
	sw->ports[6].link_nr = 1;

	sw->ports[7].config.type = TB_TYPE_PORT;
	sw->ports[7].config.max_in_hop_id = 19;
	sw->ports[7].config.max_out_hop_id = 19;
	sw->ports[7].total_credits = 60;
	sw->ports[7].ctl_credits = 2;
	sw->ports[7].dual_link_port = &sw->ports[8];

	sw->ports[8].config.type = TB_TYPE_PORT;
	sw->ports[8].config.max_in_hop_id = 19;
	sw->ports[8].config.max_out_hop_id = 19;
	sw->ports[8].total_credits = 60;
	sw->ports[8].ctl_credits = 2;
	sw->ports[8].dual_link_port = &sw->ports[7];
	sw->ports[8].link_nr = 1;

	sw->ports[9].config.type = TB_TYPE_PCIE_UP;
	sw->ports[9].config.max_in_hop_id = 8;
	sw->ports[9].config.max_out_hop_id = 8;

	sw->ports[10].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[10].config.max_in_hop_id = 8;
	sw->ports[10].config.max_out_hop_id = 8;

	sw->ports[11].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[11].config.max_in_hop_id = 8;
	sw->ports[11].config.max_out_hop_id = 8;

	sw->ports[12].config.type = TB_TYPE_PCIE_DOWN;
	sw->ports[12].config.max_in_hop_id = 8;
	sw->ports[12].config.max_out_hop_id = 8;

	sw->ports[13].config.type = TB_TYPE_DP_HDMI_OUT;
	sw->ports[13].config.max_in_hop_id = 9;
	sw->ports[13].config.max_out_hop_id = 9;
	sw->ports[13].cap_adap = -1;

	sw->ports[14].config.type = TB_TYPE_DP_HDMI_OUT;
	sw->ports[14].config.max_in_hop_id = 9;
	sw->ports[14].config.max_out_hop_id = 9;
	sw->ports[14].cap_adap = -1;

	sw->ports[15].disabled = true;

	sw->ports[16].config.type = TB_TYPE_USB3_UP;
	sw->ports[16].config.max_in_hop_id = 8;
	sw->ports[16].config.max_out_hop_id = 8;

	sw->ports[17].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[17].config.max_in_hop_id = 8;
	sw->ports[17].config.max_out_hop_id = 8;

	sw->ports[18].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[18].config.max_in_hop_id = 8;
	sw->ports[18].config.max_out_hop_id = 8;

	sw->ports[19].config.type = TB_TYPE_USB3_DOWN;
	sw->ports[19].config.max_in_hop_id = 8;
	sw->ports[19].config.max_out_hop_id = 8;

	if (!parent)
		return sw;

	/* Link them */
	upstream_port = tb_upstream_port(sw);
	port = tb_port_at(route, parent);
	port->remote = upstream_port;
	upstream_port->remote = port;
	if (port->dual_link_port && upstream_port->dual_link_port) {
		port->dual_link_port->remote = upstream_port->dual_link_port;
		upstream_port->dual_link_port->remote = port->dual_link_port;

		if (bonded) {
			/* Bonding is used */
			port->bonded = true;
			port->total_credits *= 2;
			port->dual_link_port->bonded = true;
			port->dual_link_port->total_credits = 0;
			upstream_port->bonded = true;
			upstream_port->total_credits *= 2;
			upstream_port->dual_link_port->bonded = true;
			upstream_port->dual_link_port->total_credits = 0;
		}
	}

	return sw;
}

static struct tb_switch *alloc_dev_with_dpin(struct kunit *test,
					     struct tb_switch *parent,
					     u64 route, bool bonded)
{
	struct tb_switch *sw;

	sw = alloc_dev_default(test, parent, route, bonded);
	if (!sw)
		return NULL;

	sw->ports[13].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[13].config.max_in_hop_id = 9;
	sw->ports[13].config.max_out_hop_id = 9;

	sw->ports[14].config.type = TB_TYPE_DP_HDMI_IN;
	sw->ports[14].config.max_in_hop_id = 9;
	sw->ports[14].config.max_out_hop_id = 9;

	return sw;
}

static struct tb_switch *alloc_dev_without_dp(struct kunit *test,
					      struct tb_switch *parent,
					      u64 route, bool bonded)
{
	struct tb_switch *sw;
	int i;

	sw = alloc_dev_default(test, parent, route, bonded);
	if (!sw)
		return NULL;
	/*
	 * Device with:
	 * 2x USB4 Adapters (adapters 1,2 and 3,4),
	 * 1x PCIe Upstream (adapter 9),
	 * 1x PCIe Downstream (adapter 10),
	 * 1x USB3 Upstream (adapter 16),
	 * 1x USB3 Downstream (adapter 17)
	 */
	for (i = 5; i <= 8; i++)
		sw->ports[i].disabled = true;

	for (i = 11; i <= 14; i++)
		sw->ports[i].disabled = true;

	sw->ports[13].cap_adap = 0;
	sw->ports[14].cap_adap = 0;

	for (i = 18; i <= 19; i++)
		sw->ports[i].disabled = true;

	sw->generation = 4;
	sw->credit_allocation = true;
	sw->max_usb3_credits = 109;
	sw->min_dp_aux_credits = 0;
	sw->min_dp_main_credits = 0;
	sw->max_pcie_credits = 30;
	sw->max_dma_credits = 1;

	return sw;
}

static struct tb_switch *alloc_dev_usb4(struct kunit *test,
					struct tb_switch *parent,
					u64 route, bool bonded)
{
	struct tb_switch *sw;

	sw = alloc_dev_default(test, parent, route, bonded);
	if (!sw)
		return NULL;

	sw->generation = 4;
	sw->credit_allocation = true;
	sw->max_usb3_credits = 14;
	sw->min_dp_aux_credits = 1;
	sw->min_dp_main_credits = 18;
	sw->max_pcie_credits = 32;
	sw->max_dma_credits = 14;

	return sw;
}

static void tb_test_path_basic(struct kunit *test)
{
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host;

	host = alloc_host(test);

	src_port = &host->ports[5];
	dst_port = src_port;

	p = tb_next_port_on_path(src_port, dst_port, NULL);
	KUNIT_EXPECT_PTR_EQ(test, p, dst_port);

	p = tb_next_port_on_path(src_port, dst_port, p);
	KUNIT_EXPECT_TRUE(test, !p);
}

static void tb_test_path_not_connected_walk(struct kunit *test)
{
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host, *dev;

	host = alloc_host(test);
	/* No connection between host and dev */
	dev = alloc_dev_default(test, NULL, 3, true);

	src_port = &host->ports[12];
	dst_port = &dev->ports[16];

	p = tb_next_port_on_path(src_port, dst_port, NULL);
	KUNIT_EXPECT_PTR_EQ(test, p, src_port);

	p = tb_next_port_on_path(src_port, dst_port, p);
	KUNIT_EXPECT_PTR_EQ(test, p, &host->ports[3]);

	p = tb_next_port_on_path(src_port, dst_port, p);
	KUNIT_EXPECT_TRUE(test, !p);

	/* Other direction */

	p = tb_next_port_on_path(dst_port, src_port, NULL);
	KUNIT_EXPECT_PTR_EQ(test, p, dst_port);

	p = tb_next_port_on_path(dst_port, src_port, p);
	KUNIT_EXPECT_PTR_EQ(test, p, &dev->ports[1]);

	p = tb_next_port_on_path(dst_port, src_port, p);
	KUNIT_EXPECT_TRUE(test, !p);
}

struct port_expectation {
	u64 route;
	u8 port;
	enum tb_port_type type;
};

static void tb_test_path_single_hop_walk(struct kunit *test)
{
	/*
	 * Walks from Host PCIe downstream port to Device #1 PCIe
	 * upstream port.
	 *
	 *   [Host]
	 *   1 |
	 *   1 |
	 *  [Device]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x0, .port = 8, .type = TB_TYPE_PCIE_DOWN },
		{ .route = 0x0, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 9, .type = TB_TYPE_PCIE_UP },
	};
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host, *dev;
	int i;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 1, true);

	src_port = &host->ports[8];
	dst_port = &dev->ports[9];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_daisy_chain_walk(struct kunit *test)
{
	/*
	 * Walks from Host DP IN to Device #2 DP OUT.
	 *
	 *           [Host]
	 *            1 |
	 *            1 |
	 *         [Device #1]
	 *       3 /
	 *      1 /
	 * [Device #2]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x0, .port = 5, .type = TB_TYPE_DP_HDMI_IN },
		{ .route = 0x0, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 13, .type = TB_TYPE_DP_HDMI_OUT },
	};
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host, *dev1, *dev2;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x301, true);

	src_port = &host->ports[5];
	dst_port = &dev2->ports[13];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_simple_tree_walk(struct kunit *test)
{
	/*
	 * Walks from Host DP IN to Device #3 DP OUT.
	 *
	 *           [Host]
	 *            1 |
	 *            1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x0, .port = 5, .type = TB_TYPE_DP_HDMI_IN },
		{ .route = 0x0, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 5, .type = TB_TYPE_PORT },
		{ .route = 0x501, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x501, .port = 13, .type = TB_TYPE_DP_HDMI_OUT },
	};
	struct tb_port *src_port, *dst_port, *p;
	struct tb_switch *host, *dev1, *dev3;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	alloc_dev_default(test, dev1, 0x301, true);
	dev3 = alloc_dev_default(test, dev1, 0x501, true);
	alloc_dev_default(test, dev1, 0x701, true);

	src_port = &host->ports[5];
	dst_port = &dev3->ports[13];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_complex_tree_walk(struct kunit *test)
{
	/*
	 * Walks from Device #3 DP IN to Device #9 DP OUT.
	 *
	 *           [Host]
	 *            1 |
	 *            1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #5]
	 *    5 |      | 1         \ 7
	 *    1 |  [Device #4]      \ 1
	 * [Device #3]             [Device #6]
	 *                       3 /
	 *                      1 /
	 *                    [Device #7]
	 *                  3 /      | 5
	 *                 1 /       |
	 *               [Device #8] | 1
	 *                       [Device #9]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x50301, .port = 13, .type = TB_TYPE_DP_HDMI_IN },
		{ .route = 0x50301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 5, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 7, .type = TB_TYPE_PORT },
		{ .route = 0x701, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x701, .port = 7, .type = TB_TYPE_PORT },
		{ .route = 0x70701, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x70701, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x3070701, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x3070701, .port = 5, .type = TB_TYPE_PORT },
		{ .route = 0x503070701, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x503070701, .port = 14, .type = TB_TYPE_DP_HDMI_OUT },
	};
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev5, *dev6, *dev7, *dev9;
	struct tb_port *src_port, *dst_port, *p;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x301, true);
	dev3 = alloc_dev_with_dpin(test, dev2, 0x50301, true);
	alloc_dev_default(test, dev1, 0x501, true);
	dev5 = alloc_dev_default(test, dev1, 0x701, true);
	dev6 = alloc_dev_default(test, dev5, 0x70701, true);
	dev7 = alloc_dev_default(test, dev6, 0x3070701, true);
	alloc_dev_default(test, dev7, 0x303070701, true);
	dev9 = alloc_dev_default(test, dev7, 0x503070701, true);

	src_port = &dev3->ports[13];
	dst_port = &dev9->ports[14];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_max_length_walk(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4, *dev5, *dev6;
	struct tb_switch *dev7, *dev8, *dev9, *dev10, *dev11, *dev12;
	struct tb_port *src_port, *dst_port, *p;
	int i;

	/*
	 * Walks from Device #6 DP IN to Device #12 DP OUT.
	 *
	 *          [Host]
	 *         1 /  \ 3
	 *        1 /    \ 1
	 * [Device #1]   [Device #7]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #2]   [Device #8]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #3]   [Device #9]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #4]   [Device #10]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #5]   [Device #11]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #6]   [Device #12]
	 */
	static const struct port_expectation test_data[] = {
		{ .route = 0x30303030301, .port = 13, .type = TB_TYPE_DP_HDMI_IN },
		{ .route = 0x30303030301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x303030301, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x303030301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x3030301, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x3030301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x30301, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x30301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x301, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x1, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x0, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x0, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x3, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x3, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x303, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x30303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x30303, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x3030303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x3030303, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x303030303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x303030303, .port = 3, .type = TB_TYPE_PORT },
		{ .route = 0x30303030303, .port = 1, .type = TB_TYPE_PORT },
		{ .route = 0x30303030303, .port = 13, .type = TB_TYPE_DP_HDMI_OUT },
	};

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x301, true);
	dev3 = alloc_dev_default(test, dev2, 0x30301, true);
	dev4 = alloc_dev_default(test, dev3, 0x3030301, true);
	dev5 = alloc_dev_default(test, dev4, 0x303030301, true);
	dev6 = alloc_dev_with_dpin(test, dev5, 0x30303030301, true);
	dev7 = alloc_dev_default(test, host, 0x3, true);
	dev8 = alloc_dev_default(test, dev7, 0x303, true);
	dev9 = alloc_dev_default(test, dev8, 0x30303, true);
	dev10 = alloc_dev_default(test, dev9, 0x3030303, true);
	dev11 = alloc_dev_default(test, dev10, 0x303030303, true);
	dev12 = alloc_dev_default(test, dev11, 0x30303030303, true);

	src_port = &dev6->ports[13];
	dst_port = &dev12->ports[13];

	/* Walk both directions */

	i = 0;
	tb_for_each_port_on_path(src_port, dst_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i++;
	}

	KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(test_data));

	i = ARRAY_SIZE(test_data) - 1;
	tb_for_each_port_on_path(dst_port, src_port, p) {
		KUNIT_EXPECT_TRUE(test, i < ARRAY_SIZE(test_data));
		KUNIT_EXPECT_EQ(test, tb_route(p->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, p->port, test_data[i].port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)p->config.type,
				test_data[i].type);
		i--;
	}

	KUNIT_EXPECT_EQ(test, i, -1);
}

static void tb_test_path_not_connected(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2;
	struct tb_port *down, *up;
	struct tb_path *path;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x3, false);
	/* Not connected to anything */
	dev2 = alloc_dev_default(test, NULL, 0x303, false);

	down = &dev1->ports[10];
	up = &dev2->ports[9];

	path = tb_path_alloc(NULL, down, 8, up, 8, 0, "PCIe Down");
	KUNIT_ASSERT_NULL(test, path);
	path = tb_path_alloc(NULL, down, 8, up, 8, 1, "PCIe Down");
	KUNIT_ASSERT_NULL(test, path);
}

struct hop_expectation {
	u64 route;
	u8 in_port;
	enum tb_port_type in_type;
	u8 out_port;
	enum tb_port_type out_type;
};

static void tb_test_path_not_bonded_lane0(struct kunit *test)
{
	/*
	 * PCIe path from host to device using lane 0.
	 *
	 *   [Host]
	 *   3 |: 4
	 *   1 |: 2
	 *  [Device]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x0,
			.in_port = 9,
			.in_type = TB_TYPE_PCIE_DOWN,
			.out_port = 3,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x3,
			.in_port = 1,
			.in_type = TB_TYPE_PORT,
			.out_port = 9,
			.out_type = TB_TYPE_PCIE_UP,
		},
	};
	struct tb_switch *host, *dev;
	struct tb_port *down, *up;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x3, false);

	down = &host->ports[9];
	up = &dev->ports[9];

	path = tb_path_alloc(NULL, down, 8, up, 8, 0, "PCIe Down");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_not_bonded_lane1(struct kunit *test)
{
	/*
	 * DP Video path from host to device using lane 1. Paths like
	 * these are only used with Thunderbolt 1 devices where lane
	 * bonding is not possible. USB4 specifically does not allow
	 * paths like this (you either use lane 0 where lane 1 is
	 * disabled or both lanes are bonded).
	 *
	 *   [Host]
	 *   1 :| 2
	 *   1 :| 2
	 *  [Device]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x0,
			.in_port = 5,
			.in_type = TB_TYPE_DP_HDMI_IN,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 13,
			.out_type = TB_TYPE_DP_HDMI_OUT,
		},
	};
	struct tb_switch *host, *dev;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x1, false);

	in = &host->ports[5];
	out = &dev->ports[13];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_not_bonded_lane1_chain(struct kunit *test)
{
	/*
	 * DP Video path from host to device 3 using lane 1.
	 *
	 *    [Host]
	 *    1 :| 2
	 *    1 :| 2
	 *  [Device #1]
	 *    7 :| 8
	 *    1 :| 2
	 *  [Device #2]
	 *    5 :| 6
	 *    1 :| 2
	 *  [Device #3]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x0,
			.in_port = 5,
			.in_type = TB_TYPE_DP_HDMI_IN,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 8,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x701,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 6,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x50701,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 13,
			.out_type = TB_TYPE_DP_HDMI_OUT,
		},
	};
	struct tb_switch *host, *dev1, *dev2, *dev3;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, false);
	dev2 = alloc_dev_default(test, dev1, 0x701, false);
	dev3 = alloc_dev_default(test, dev2, 0x50701, false);

	in = &host->ports[5];
	out = &dev3->ports[13];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_not_bonded_lane1_chain_reverse(struct kunit *test)
{
	/*
	 * DP Video path from device 3 to host using lane 1.
	 *
	 *    [Host]
	 *    1 :| 2
	 *    1 :| 2
	 *  [Device #1]
	 *    7 :| 8
	 *    1 :| 2
	 *  [Device #2]
	 *    5 :| 6
	 *    1 :| 2
	 *  [Device #3]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x50701,
			.in_port = 13,
			.in_type = TB_TYPE_DP_HDMI_IN,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x701,
			.in_port = 6,
			.in_type = TB_TYPE_PORT,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 8,
			.in_type = TB_TYPE_PORT,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x0,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 5,
			.out_type = TB_TYPE_DP_HDMI_IN,
		},
	};
	struct tb_switch *host, *dev1, *dev2, *dev3;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, false);
	dev2 = alloc_dev_default(test, dev1, 0x701, false);
	dev3 = alloc_dev_with_dpin(test, dev2, 0x50701, false);

	in = &dev3->ports[13];
	out = &host->ports[5];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_mixed_chain(struct kunit *test)
{
	/*
	 * DP Video path from host to device 4 where first and last link
	 * is bonded.
	 *
	 *    [Host]
	 *    1 |
	 *    1 |
	 *  [Device #1]
	 *    7 :| 8
	 *    1 :| 2
	 *  [Device #2]
	 *    5 :| 6
	 *    1 :| 2
	 *  [Device #3]
	 *    3 |
	 *    1 |
	 *  [Device #4]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x0,
			.in_port = 5,
			.in_type = TB_TYPE_DP_HDMI_IN,
			.out_port = 1,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 1,
			.in_type = TB_TYPE_PORT,
			.out_port = 8,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x701,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 6,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x50701,
			.in_port = 2,
			.in_type = TB_TYPE_PORT,
			.out_port = 3,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x3050701,
			.in_port = 1,
			.in_type = TB_TYPE_PORT,
			.out_port = 13,
			.out_type = TB_TYPE_DP_HDMI_OUT,
		},
	};
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x701, false);
	dev3 = alloc_dev_default(test, dev2, 0x50701, false);
	dev4 = alloc_dev_default(test, dev3, 0x3050701, true);

	in = &host->ports[5];
	out = &dev4->ports[13];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_path_mixed_chain_reverse(struct kunit *test)
{
	/*
	 * DP Video path from device 4 to host where first and last link
	 * is bonded.
	 *
	 *    [Host]
	 *    1 |
	 *    1 |
	 *  [Device #1]
	 *    7 :| 8
	 *    1 :| 2
	 *  [Device #2]
	 *    5 :| 6
	 *    1 :| 2
	 *  [Device #3]
	 *    3 |
	 *    1 |
	 *  [Device #4]
	 */
	static const struct hop_expectation test_data[] = {
		{
			.route = 0x3050701,
			.in_port = 13,
			.in_type = TB_TYPE_DP_HDMI_OUT,
			.out_port = 1,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x50701,
			.in_port = 3,
			.in_type = TB_TYPE_PORT,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x701,
			.in_port = 6,
			.in_type = TB_TYPE_PORT,
			.out_port = 2,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x1,
			.in_port = 8,
			.in_type = TB_TYPE_PORT,
			.out_port = 1,
			.out_type = TB_TYPE_PORT,
		},
		{
			.route = 0x0,
			.in_port = 1,
			.in_type = TB_TYPE_PORT,
			.out_port = 5,
			.out_type = TB_TYPE_DP_HDMI_IN,
		},
	};
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4;
	struct tb_port *in, *out;
	struct tb_path *path;
	int i;

	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x701, false);
	dev3 = alloc_dev_default(test, dev2, 0x50701, false);
	dev4 = alloc_dev_default(test, dev3, 0x3050701, true);

	in = &dev4->ports[13];
	out = &host->ports[5];

	path = tb_path_alloc(NULL, in, 9, out, 9, 1, "Video");
	KUNIT_ASSERT_NOT_NULL(test, path);
	KUNIT_ASSERT_EQ(test, path->path_length, ARRAY_SIZE(test_data));
	for (i = 0; i < ARRAY_SIZE(test_data); i++) {
		const struct tb_port *in_port, *out_port;

		in_port = path->hops[i].in_port;
		out_port = path->hops[i].out_port;

		KUNIT_EXPECT_EQ(test, tb_route(in_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, in_port->port, test_data[i].in_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)in_port->config.type,
				test_data[i].in_type);
		KUNIT_EXPECT_EQ(test, tb_route(out_port->sw), test_data[i].route);
		KUNIT_EXPECT_EQ(test, out_port->port, test_data[i].out_port);
		KUNIT_EXPECT_EQ(test, (enum tb_port_type)out_port->config.type,
				test_data[i].out_type);
	}
	tb_path_free(path);
}

static void tb_test_tunnel_pcie(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2;
	struct tb_tunnel *tunnel1, *tunnel2;
	struct tb_port *down, *up;

	/*
	 * Create PCIe tunnel between host and two devices.
	 *
	 *   [Host]
	 *    1 |
	 *    1 |
	 *  [Device #1]
	 *    5 |
	 *    1 |
	 *  [Device #2]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x501, true);

	down = &host->ports[8];
	up = &dev1->ports[9];
	tunnel1 = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel1);
	KUNIT_EXPECT_EQ(test, tunnel1->type, TB_TUNNEL_PCI);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->src_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->dst_port, up);
	KUNIT_ASSERT_EQ(test, tunnel1->npaths, 2);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[0]->hops[0].in_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[0]->hops[1].out_port, up);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[1]->hops[0].in_port, up);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[1]->hops[1].out_port, down);

	down = &dev1->ports[10];
	up = &dev2->ports[9];
	tunnel2 = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel2);
	KUNIT_EXPECT_EQ(test, tunnel2->type, TB_TUNNEL_PCI);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->src_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->dst_port, up);
	KUNIT_ASSERT_EQ(test, tunnel2->npaths, 2);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[0]->hops[0].in_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[0]->hops[1].out_port, up);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[1]->hops[0].in_port, up);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[1]->hops[1].out_port, down);

	tb_tunnel_put(tunnel2);
	tb_tunnel_put(tunnel1);
}

static void tb_test_tunnel_dp(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;

	/*
	 * Create DP tunnel between Host and Device
	 *
	 *   [Host]
	 *   1 |
	 *   1 |
	 *  [Device]
	 */
	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x3, true);

	in = &host->ports[5];
	out = &dev->ports[13];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0, NULL, NULL);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[1].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[1].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[2]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[0].in_port, out);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[1].out_port, in);
	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dp_chain(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev4;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;

	/*
	 * Create DP tunnel from Host DP IN to Device #4 DP OUT.
	 *
	 *           [Host]
	 *            1 |
	 *            1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	alloc_dev_default(test, dev1, 0x301, true);
	alloc_dev_default(test, dev1, 0x501, true);
	dev4 = alloc_dev_default(test, dev1, 0x701, true);

	in = &host->ports[5];
	out = &dev4->ports[14];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0, NULL, NULL);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[2].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[2].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[2]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[0].in_port, out);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[2].out_port, in);
	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dp_tree(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev5;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;

	/*
	 * Create DP tunnel from Device #2 DP IN to Device #5 DP OUT.
	 *
	 *          [Host]
	 *           3 |
	 *           1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 *             | 5
	 *             | 1
	 *         [Device #5]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x3, true);
	dev2 = alloc_dev_with_dpin(test, dev1, 0x303, true);
	dev3 = alloc_dev_default(test, dev1, 0x503, true);
	alloc_dev_default(test, dev1, 0x703, true);
	dev5 = alloc_dev_default(test, dev3, 0x50503, true);

	in = &dev2->ports[13];
	out = &dev5->ports[13];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0, NULL, NULL);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 4);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[3].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 4);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[3].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[2]->path_length, 4);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[0].in_port, out);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[3].out_port, in);
	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dp_max_length(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4, *dev5, *dev6;
	struct tb_switch *dev7, *dev8, *dev9, *dev10, *dev11, *dev12;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;

	/*
	 * Creates DP tunnel from Device #6 to Device #12.
	 *
	 *          [Host]
	 *         1 /  \ 3
	 *        1 /    \ 1
	 * [Device #1]   [Device #7]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #2]   [Device #8]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #3]   [Device #9]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #4]   [Device #10]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #5]   [Device #11]
	 *     3 |           | 3
	 *     1 |           | 1
	 * [Device #6]   [Device #12]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x301, true);
	dev3 = alloc_dev_default(test, dev2, 0x30301, true);
	dev4 = alloc_dev_default(test, dev3, 0x3030301, true);
	dev5 = alloc_dev_default(test, dev4, 0x303030301, true);
	dev6 = alloc_dev_with_dpin(test, dev5, 0x30303030301, true);
	dev7 = alloc_dev_default(test, host, 0x3, true);
	dev8 = alloc_dev_default(test, dev7, 0x303, true);
	dev9 = alloc_dev_default(test, dev8, 0x30303, true);
	dev10 = alloc_dev_default(test, dev9, 0x3030303, true);
	dev11 = alloc_dev_default(test, dev10, 0x303030303, true);
	dev12 = alloc_dev_default(test, dev11, 0x30303030303, true);

	in = &dev6->ports[13];
	out = &dev12->ports[13];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0, NULL, NULL);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 13);
	/* First hop */
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, in);
	/* Middle */
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[6].in_port,
			    &host->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[6].out_port,
			    &host->ports[3]);
	/* Last */
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[12].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 13);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, in);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[6].in_port,
			    &host->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[6].out_port,
			    &host->ports[3]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[12].out_port, out);
	KUNIT_ASSERT_EQ(test, tunnel->paths[2]->path_length, 13);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[0].in_port, out);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[6].in_port,
			    &host->ports[3]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[6].out_port,
			    &host->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[2]->hops[12].out_port, in);
	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_3dp(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4, *dev5;
	struct tb_port *in1, *in2, *in3, *out1, *out2, *out3;
	struct tb_tunnel *tunnel1, *tunnel2, *tunnel3;

	/*
	 * Create 3 DP tunnels from Host to Devices #2, #5 and #4.
	 *
	 *          [Host]
	 *           3 |
	 *           1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 *             | 5
	 *             | 1
	 *         [Device #5]
	 */
	host = alloc_host_br(test);
	dev1 = alloc_dev_default(test, host, 0x3, true);
	dev2 = alloc_dev_default(test, dev1, 0x303, true);
	dev3 = alloc_dev_default(test, dev1, 0x503, true);
	dev4 = alloc_dev_default(test, dev1, 0x703, true);
	dev5 = alloc_dev_default(test, dev3, 0x50503, true);

	in1 = &host->ports[5];
	in2 = &host->ports[6];
	in3 = &host->ports[10];

	out1 = &dev2->ports[13];
	out2 = &dev5->ports[13];
	out3 = &dev4->ports[14];

	tunnel1 = tb_tunnel_alloc_dp(NULL, in1, out1, 1, 0, 0, NULL, NULL);
	KUNIT_ASSERT_TRUE(test, tunnel1 != NULL);
	KUNIT_EXPECT_EQ(test, tunnel1->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->src_port, in1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->dst_port, out1);
	KUNIT_ASSERT_EQ(test, tunnel1->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[0]->path_length, 3);

	tunnel2 = tb_tunnel_alloc_dp(NULL, in2, out2, 1, 0, 0, NULL, NULL);
	KUNIT_ASSERT_TRUE(test, tunnel2 != NULL);
	KUNIT_EXPECT_EQ(test, tunnel2->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->src_port, in2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->dst_port, out2);
	KUNIT_ASSERT_EQ(test, tunnel2->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[0]->path_length, 4);

	tunnel3 = tb_tunnel_alloc_dp(NULL, in3, out3, 1, 0, 0, NULL, NULL);
	KUNIT_ASSERT_TRUE(test, tunnel3 != NULL);
	KUNIT_EXPECT_EQ(test, tunnel3->type, TB_TUNNEL_DP);
	KUNIT_EXPECT_PTR_EQ(test, tunnel3->src_port, in3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel3->dst_port, out3);
	KUNIT_ASSERT_EQ(test, tunnel3->npaths, 3);
	KUNIT_ASSERT_EQ(test, tunnel3->paths[0]->path_length, 3);

	tb_tunnel_put(tunnel2);
	tb_tunnel_put(tunnel1);
}

static void tb_test_tunnel_usb3(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2;
	struct tb_tunnel *tunnel1, *tunnel2;
	struct tb_port *down, *up;

	/*
	 * Create USB3 tunnel between host and two devices.
	 *
	 *   [Host]
	 *    1 |
	 *    1 |
	 *  [Device #1]
	 *          \ 7
	 *           \ 1
	 *         [Device #2]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x701, true);

	down = &host->ports[12];
	up = &dev1->ports[16];
	tunnel1 = tb_tunnel_alloc_usb3(NULL, up, down, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel1);
	KUNIT_EXPECT_EQ(test, tunnel1->type, TB_TUNNEL_USB3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->src_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->dst_port, up);
	KUNIT_ASSERT_EQ(test, tunnel1->npaths, 2);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[0]->hops[0].in_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[0]->hops[1].out_port, up);
	KUNIT_ASSERT_EQ(test, tunnel1->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[1]->hops[0].in_port, up);
	KUNIT_EXPECT_PTR_EQ(test, tunnel1->paths[1]->hops[1].out_port, down);

	down = &dev1->ports[17];
	up = &dev2->ports[16];
	tunnel2 = tb_tunnel_alloc_usb3(NULL, up, down, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel2);
	KUNIT_EXPECT_EQ(test, tunnel2->type, TB_TUNNEL_USB3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->src_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->dst_port, up);
	KUNIT_ASSERT_EQ(test, tunnel2->npaths, 2);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[0]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[0]->hops[0].in_port, down);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[0]->hops[1].out_port, up);
	KUNIT_ASSERT_EQ(test, tunnel2->paths[1]->path_length, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[1]->hops[0].in_port, up);
	KUNIT_EXPECT_PTR_EQ(test, tunnel2->paths[1]->hops[1].out_port, down);

	tb_tunnel_put(tunnel2);
	tb_tunnel_put(tunnel1);
}

static void tb_test_tunnel_port_on_path(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2, *dev3, *dev4, *dev5;
	struct tb_port *in, *out, *port;
	struct tb_tunnel *dp_tunnel;

	/*
	 *          [Host]
	 *           3 |
	 *           1 |
	 *         [Device #1]
	 *       3 /   | 5  \ 7
	 *      1 /    |     \ 1
	 * [Device #2] |    [Device #4]
	 *             | 1
	 *         [Device #3]
	 *             | 5
	 *             | 1
	 *         [Device #5]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x3, true);
	dev2 = alloc_dev_with_dpin(test, dev1, 0x303, true);
	dev3 = alloc_dev_default(test, dev1, 0x503, true);
	dev4 = alloc_dev_default(test, dev1, 0x703, true);
	dev5 = alloc_dev_default(test, dev3, 0x50503, true);

	in = &dev2->ports[13];
	out = &dev5->ports[13];

	dp_tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0, NULL, NULL);
	KUNIT_ASSERT_NOT_NULL(test, dp_tunnel);

	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, in));
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, out));

	port = &host->ports[8];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &host->ports[3];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev1->ports[1];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev1->ports[3];
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev1->ports[5];
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev1->ports[7];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev3->ports[1];
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev5->ports[1];
	KUNIT_EXPECT_TRUE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	port = &dev4->ports[1];
	KUNIT_EXPECT_FALSE(test, tb_tunnel_port_on_path(dp_tunnel, port));

	tb_tunnel_put(dp_tunnel);
}

static void tb_test_tunnel_dma(struct kunit *test)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_switch *host;

	/*
	 * Create DMA tunnel from NHI to port 1 and back.
	 *
	 *   [Host 1]
	 *    1 ^ In HopID 1 -> Out HopID 8
	 *      |
	 *      v In HopID 8 -> Out HopID 1
	 * ............ Domain border
	 *      |
	 *   [Host 2]
	 */
	host = alloc_host(test);
	nhi = &host->ports[7];
	port = &host->ports[1];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DMA);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, nhi);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, port);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 2);
	/* RX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].in_hop_index, 8);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].out_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].next_hop_index, 1);
	/* TX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[1]->hops[0].in_hop_index, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].out_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[1]->hops[0].next_hop_index, 8);

	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dma_rx(struct kunit *test)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_switch *host;

	/*
	 * Create DMA RX tunnel from port 1 to NHI.
	 *
	 *   [Host 1]
	 *    1 ^
	 *      |
	 *      | In HopID 15 -> Out HopID 2
	 * ............ Domain border
	 *      |
	 *   [Host 2]
	 */
	host = alloc_host(test);
	nhi = &host->ports[7];
	port = &host->ports[1];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, -1, -1, 15, 2);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DMA);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, nhi);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, port);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 1);
	/* RX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].in_hop_index, 15);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].out_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].next_hop_index, 2);

	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dma_tx(struct kunit *test)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_switch *host;

	/*
	 * Create DMA TX tunnel from NHI to port 1.
	 *
	 *   [Host 1]
	 *    1 | In HopID 2 -> Out HopID 15
	 *      |
	 *      v
	 * ............ Domain border
	 *      |
	 *   [Host 2]
	 */
	host = alloc_host(test);
	nhi = &host->ports[7];
	port = &host->ports[1];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 15, 2, -1, -1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DMA);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, nhi);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, port);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 1);
	/* TX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].in_hop_index, 2);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].out_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].next_hop_index, 15);

	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dma_chain(struct kunit *test)
{
	struct tb_switch *host, *dev1, *dev2;
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;

	/*
	 * Create DMA tunnel from NHI to Device #2 port 3 and back.
	 *
	 *   [Host 1]
	 *    1 ^ In HopID 1 -> Out HopID x
	 *      |
	 *    1 | In HopID x -> Out HopID 1
	 *  [Device #1]
	 *         7 \
	 *          1 \
	 *         [Device #2]
	 *           3 | In HopID x -> Out HopID 8
	 *             |
	 *             v In HopID 8 -> Out HopID x
	 * ............ Domain border
	 *             |
	 *          [Host 2]
	 */
	host = alloc_host(test);
	dev1 = alloc_dev_default(test, host, 0x1, true);
	dev2 = alloc_dev_default(test, dev1, 0x701, true);

	nhi = &host->ports[7];
	port = &dev2->ports[3];
	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_EXPECT_EQ(test, tunnel->type, TB_TUNNEL_DMA);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->src_port, nhi);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->dst_port, port);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, 2);
	/* RX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[0]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].in_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[0].in_hop_index, 8);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[0].out_port,
			    &dev2->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[1].in_port,
			    &dev1->ports[7]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[1].out_port,
			    &dev1->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[2].in_port,
			    &host->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[0]->hops[2].out_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[0]->hops[2].next_hop_index, 1);
	/* TX path */
	KUNIT_ASSERT_EQ(test, tunnel->paths[1]->path_length, 3);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[0].in_port, nhi);
	KUNIT_EXPECT_EQ(test, tunnel->paths[1]->hops[0].in_hop_index, 1);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[1].in_port,
			    &dev1->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[1].out_port,
			    &dev1->ports[7]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[2].in_port,
			    &dev2->ports[1]);
	KUNIT_EXPECT_PTR_EQ(test, tunnel->paths[1]->hops[2].out_port, port);
	KUNIT_EXPECT_EQ(test, tunnel->paths[1]->hops[2].next_hop_index, 8);

	tb_tunnel_put(tunnel);
}

static void tb_test_tunnel_dma_match(struct kunit *test)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_switch *host;

	host = alloc_host(test);
	nhi = &host->ports[7];
	port = &host->ports[1];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 15, 1, 15, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);

	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, 1, 15, 1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 8, 1, 15, 1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, 1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, 1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, -1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, 1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, 1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, -1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 8, -1, 8, -1));

	tb_tunnel_put(tunnel);

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 15, 1, -1, -1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, 1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, 15, -1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, 1, -1, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, -1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 15, 1, 15, 1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, 1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 15, 11, -1, -1));

	tb_tunnel_put(tunnel);

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, -1, -1, 15, 11);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, 11));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, -1));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, 11));
	KUNIT_ASSERT_TRUE(test, tb_tunnel_match_dma(tunnel, -1, -1, -1, -1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, -1, -1, 15, 1));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, -1, -1, 10, 11));
	KUNIT_ASSERT_FALSE(test, tb_tunnel_match_dma(tunnel, 15, 11, -1, -1));

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_legacy_not_bonded(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x1, false);

	down = &host->ports[8];
	up = &dev->ports[9];
	tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 16U);

	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 16U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_legacy_bonded(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host(test);
	dev = alloc_dev_default(test, host, 0x1, true);

	down = &host->ports[8];
	up = &dev->ports[9];
	tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_pcie(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	down = &host->ports[8];
	up = &dev->ports[9];
	tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 64U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_without_dp(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_without_dp(test, host, 0x1, true);

	/*
	 * The device has no DP therefore baMinDPmain = baMinDPaux = 0
	 *
	 * Create PCIe path with buffers less than baMaxPCIe.
	 *
	 * For a device with buffers configurations:
	 * baMaxUSB3 = 109
	 * baMinDPaux = 0
	 * baMinDPmain = 0
	 * baMaxPCIe = 30
	 * baMaxHI = 1
	 * Remaining Buffers = Total - (CP + DP) = 120 - (2 + 0) = 118
	 * PCIe Credits = Max(6, Min(baMaxPCIe, Remaining Buffers - baMaxUSB3)
	 *		= Max(6, Min(30, 9) = 9
	 */
	down = &host->ports[8];
	up = &dev->ports[9];
	tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_TRUE(test, tunnel != NULL);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	/* PCIe downstream path */
	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 9U);

	/* PCIe upstream path */
	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 64U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_dp(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *in, *out;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	in = &host->ports[5];
	out = &dev->ports[14];

	tunnel = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0, NULL, NULL);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)3);

	/* Video (main) path */
	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 12U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 18U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 0U);

	/* AUX TX */
	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	/* AUX RX */
	path = tunnel->paths[2];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_usb3(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *up, *down;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	down = &host->ports[12];
	up = &dev->ports[16];
	tunnel = tb_tunnel_alloc_usb3(NULL, up, down, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_dma(struct kunit *test)
{
	struct tb_switch *host, *dev;
	struct tb_port *nhi, *port;
	struct tb_tunnel *tunnel;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	nhi = &host->ports[7];
	port = &dev->ports[3];

	tunnel = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel);
	KUNIT_ASSERT_EQ(test, tunnel->npaths, (size_t)2);

	/* DMA RX */
	path = tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	/* DMA TX */
	path = tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	tb_tunnel_put(tunnel);
}

static void tb_test_credit_alloc_dma_multiple(struct kunit *test)
{
	struct tb_tunnel *tunnel1, *tunnel2, *tunnel3;
	struct tb_switch *host, *dev;
	struct tb_port *nhi, *port;
	struct tb_path *path;

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	nhi = &host->ports[7];
	port = &dev->ports[3];

	/*
	 * Create three DMA tunnels through the same ports. With the
	 * default buffers we should be able to create two and the last
	 * one fails.
	 *
	 * For default host we have following buffers for DMA:
	 *
	 *   120 - (2 + 2 * (1 + 0) + 32 + 64 + spare) = 20
	 *
	 * For device we have following:
	 *
	 *  120 - (2 + 2 * (1 + 18) + 14 + 32 + spare) = 34
	 *
	 * spare = 14 + 1 = 15
	 *
	 * So on host the first tunnel gets 14 and the second gets the
	 * remaining 1 and then we run out of buffers.
	 */
	tunnel1 = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, tunnel1);
	KUNIT_ASSERT_EQ(test, tunnel1->npaths, (size_t)2);

	path = tunnel1->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = tunnel1->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	tunnel2 = tb_tunnel_alloc_dma(NULL, nhi, port, 9, 2, 9, 2);
	KUNIT_ASSERT_NOT_NULL(test, tunnel2);
	KUNIT_ASSERT_EQ(test, tunnel2->npaths, (size_t)2);

	path = tunnel2->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	path = tunnel2->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	tunnel3 = tb_tunnel_alloc_dma(NULL, nhi, port, 10, 3, 10, 3);
	KUNIT_ASSERT_NULL(test, tunnel3);

	/*
	 * Release the first DMA tunnel. That should make 14 buffers
	 * available for the next tunnel.
	 */
	tb_tunnel_put(tunnel1);

	tunnel3 = tb_tunnel_alloc_dma(NULL, nhi, port, 10, 3, 10, 3);
	KUNIT_ASSERT_NOT_NULL(test, tunnel3);

	path = tunnel3->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = tunnel3->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	tb_tunnel_put(tunnel3);
	tb_tunnel_put(tunnel2);
}

static struct tb_tunnel *TB_TEST_PCIE_TUNNEL(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *up, *down;
	struct tb_tunnel *pcie_tunnel;
	struct tb_path *path;

	down = &host->ports[8];
	up = &dev->ports[9];
	pcie_tunnel = tb_tunnel_alloc_pci(NULL, up, down);
	KUNIT_ASSERT_NOT_NULL(test, pcie_tunnel);
	KUNIT_ASSERT_EQ(test, pcie_tunnel->npaths, (size_t)2);

	path = pcie_tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	path = pcie_tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 64U);

	return pcie_tunnel;
}

static struct tb_tunnel *TB_TEST_DP_TUNNEL1(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *in, *out;
	struct tb_tunnel *dp_tunnel1;
	struct tb_path *path;

	in = &host->ports[5];
	out = &dev->ports[13];
	dp_tunnel1 = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0, NULL, NULL);
	KUNIT_ASSERT_NOT_NULL(test, dp_tunnel1);
	KUNIT_ASSERT_EQ(test, dp_tunnel1->npaths, (size_t)3);

	path = dp_tunnel1->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 12U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 18U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 0U);

	path = dp_tunnel1->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	path = dp_tunnel1->paths[2];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	return dp_tunnel1;
}

static struct tb_tunnel *TB_TEST_DP_TUNNEL2(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *in, *out;
	struct tb_tunnel *dp_tunnel2;
	struct tb_path *path;

	in = &host->ports[6];
	out = &dev->ports[14];
	dp_tunnel2 = tb_tunnel_alloc_dp(NULL, in, out, 1, 0, 0, NULL, NULL);
	KUNIT_ASSERT_NOT_NULL(test, dp_tunnel2);
	KUNIT_ASSERT_EQ(test, dp_tunnel2->npaths, (size_t)3);

	path = dp_tunnel2->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 12U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 18U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 0U);

	path = dp_tunnel2->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	path = dp_tunnel2->paths[2];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 1U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	return dp_tunnel2;
}

static struct tb_tunnel *TB_TEST_USB3_TUNNEL(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *up, *down;
	struct tb_tunnel *usb3_tunnel;
	struct tb_path *path;

	down = &host->ports[12];
	up = &dev->ports[16];
	usb3_tunnel = tb_tunnel_alloc_usb3(NULL, up, down, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, usb3_tunnel);
	KUNIT_ASSERT_EQ(test, usb3_tunnel->npaths, (size_t)2);

	path = usb3_tunnel->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = usb3_tunnel->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 7U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 32U);

	return usb3_tunnel;
}

static struct tb_tunnel *TB_TEST_DMA_TUNNEL1(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *dma_tunnel1;
	struct tb_path *path;

	nhi = &host->ports[7];
	port = &dev->ports[3];
	dma_tunnel1 = tb_tunnel_alloc_dma(NULL, nhi, port, 8, 1, 8, 1);
	KUNIT_ASSERT_NOT_NULL(test, dma_tunnel1);
	KUNIT_ASSERT_EQ(test, dma_tunnel1->npaths, (size_t)2);

	path = dma_tunnel1->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	path = dma_tunnel1->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 14U);

	return dma_tunnel1;
}

static struct tb_tunnel *TB_TEST_DMA_TUNNEL2(struct kunit *test,
			struct tb_switch *host, struct tb_switch *dev)
{
	struct tb_port *nhi, *port;
	struct tb_tunnel *dma_tunnel2;
	struct tb_path *path;

	nhi = &host->ports[7];
	port = &dev->ports[3];
	dma_tunnel2 = tb_tunnel_alloc_dma(NULL, nhi, port, 9, 2, 9, 2);
	KUNIT_ASSERT_NOT_NULL(test, dma_tunnel2);
	KUNIT_ASSERT_EQ(test, dma_tunnel2->npaths, (size_t)2);

	path = dma_tunnel2->paths[0];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 14U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	path = dma_tunnel2->paths[1];
	KUNIT_ASSERT_EQ(test, path->path_length, 2);
	KUNIT_EXPECT_EQ(test, path->hops[0].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[0].initial_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].nfc_credits, 0U);
	KUNIT_EXPECT_EQ(test, path->hops[1].initial_credits, 1U);

	return dma_tunnel2;
}

static void tb_test_credit_alloc_all(struct kunit *test)
{
	struct tb_tunnel *pcie_tunnel, *dp_tunnel1, *dp_tunnel2, *usb3_tunnel;
	struct tb_tunnel *dma_tunnel1, *dma_tunnel2;
	struct tb_switch *host, *dev;

	/*
	 * Create PCIe, 2 x DP, USB 3.x and two DMA tunnels from host to
	 * device. Expectation is that all these can be established with
	 * the default credit allocation found in Intel hardware.
	 */

	host = alloc_host_usb4(test);
	dev = alloc_dev_usb4(test, host, 0x1, true);

	pcie_tunnel = TB_TEST_PCIE_TUNNEL(test, host, dev);
	dp_tunnel1 = TB_TEST_DP_TUNNEL1(test, host, dev);
	dp_tunnel2 = TB_TEST_DP_TUNNEL2(test, host, dev);
	usb3_tunnel = TB_TEST_USB3_TUNNEL(test, host, dev);
	dma_tunnel1 = TB_TEST_DMA_TUNNEL1(test, host, dev);
	dma_tunnel2 = TB_TEST_DMA_TUNNEL2(test, host, dev);

	tb_tunnel_put(dma_tunnel2);
	tb_tunnel_put(dma_tunnel1);
	tb_tunnel_put(usb3_tunnel);
	tb_tunnel_put(dp_tunnel2);
	tb_tunnel_put(dp_tunnel1);
	tb_tunnel_put(pcie_tunnel);
}

static const u32 root_directory[] = {
	0x55584401,	/* "UXD" v1 */
	0x00000018,	/* Root directory length */
	0x76656e64,	/* "vend" */
	0x6f726964,	/* "orid" */
	0x76000001,	/* "v" R 1 */
	0x00000a27,	/* Immediate value, ! Vendor ID */
	0x76656e64,	/* "vend" */
	0x6f726964,	/* "orid" */
	0x74000003,	/* "t" R 3 */
	0x0000001a,	/* Text leaf offset, (“Apple Inc.”) */
	0x64657669,	/* "devi" */
	0x63656964,	/* "ceid" */
	0x76000001,	/* "v" R 1 */
	0x0000000a,	/* Immediate value, ! Device ID */
	0x64657669,	/* "devi" */
	0x63656964,	/* "ceid" */
	0x74000003,	/* "t" R 3 */
	0x0000001d,	/* Text leaf offset, (“Macintosh”) */
	0x64657669,	/* "devi" */
	0x63657276,	/* "cerv" */
	0x76000001,	/* "v" R 1 */
	0x80000100,	/* Immediate value, Device Revision */
	0x6e657477,	/* "netw" */
	0x6f726b00,	/* "ork" */
	0x44000014,	/* "D" R 20 */
	0x00000021,	/* Directory data offset, (Network Directory) */
	0x4170706c,	/* "Appl" */
	0x6520496e,	/* "e In" */
	0x632e0000,	/* "c." ! */
	0x4d616369,	/* "Maci" */
	0x6e746f73,	/* "ntos" */
	0x68000000,	/* "h" */
	0x00000000,	/* padding */
	0xca8961c6,	/* Directory UUID, Network Directory */
	0x9541ce1c,	/* Directory UUID, Network Directory */
	0x5949b8bd,	/* Directory UUID, Network Directory */
	0x4f5a5f2e,	/* Directory UUID, Network Directory */
	0x70727463,	/* "prtc" */
	0x69640000,	/* "id" */
	0x76000001,	/* "v" R 1 */
	0x00000001,	/* Immediate value, Network Protocol ID */
	0x70727463,	/* "prtc" */
	0x76657273,	/* "vers" */
	0x76000001,	/* "v" R 1 */
	0x00000001,	/* Immediate value, Network Protocol Version */
	0x70727463,	/* "prtc" */
	0x72657673,	/* "revs" */
	0x76000001,	/* "v" R 1 */
	0x00000001,	/* Immediate value, Network Protocol Revision */
	0x70727463,	/* "prtc" */
	0x73746e73,	/* "stns" */
	0x76000001,	/* "v" R 1 */
	0x00000000,	/* Immediate value, Network Protocol Settings */
};

static const uuid_t network_dir_uuid =
	UUID_INIT(0xc66189ca, 0x1cce, 0x4195,
		  0xbd, 0xb8, 0x49, 0x59, 0x2e, 0x5f, 0x5a, 0x4f);

static void tb_test_property_parse(struct kunit *test)
{
	struct tb_property_dir *dir, *network_dir;
	struct tb_property *p;

	dir = tb_property_parse_dir(root_directory, ARRAY_SIZE(root_directory));
	KUNIT_ASSERT_NOT_NULL(test, dir);

	p = tb_property_find(dir, "foo", TB_PROPERTY_TYPE_TEXT);
	KUNIT_ASSERT_NULL(test, p);

	p = tb_property_find(dir, "vendorid", TB_PROPERTY_TYPE_TEXT);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_STREQ(test, p->value.text, "Apple Inc.");

	p = tb_property_find(dir, "vendorid", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0xa27);

	p = tb_property_find(dir, "deviceid", TB_PROPERTY_TYPE_TEXT);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_STREQ(test, p->value.text, "Macintosh");

	p = tb_property_find(dir, "deviceid", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0xa);

	p = tb_property_find(dir, "missing", TB_PROPERTY_TYPE_DIRECTORY);
	KUNIT_ASSERT_NULL(test, p);

	p = tb_property_find(dir, "network", TB_PROPERTY_TYPE_DIRECTORY);
	KUNIT_ASSERT_NOT_NULL(test, p);

	network_dir = p->value.dir;
	KUNIT_EXPECT_TRUE(test, uuid_equal(network_dir->uuid, &network_dir_uuid));

	p = tb_property_find(network_dir, "prtcid", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0x1);

	p = tb_property_find(network_dir, "prtcvers", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0x1);

	p = tb_property_find(network_dir, "prtcrevs", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0x1);

	p = tb_property_find(network_dir, "prtcstns", TB_PROPERTY_TYPE_VALUE);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, p->value.immediate, 0x0);

	p = tb_property_find(network_dir, "deviceid", TB_PROPERTY_TYPE_VALUE);
	KUNIT_EXPECT_TRUE(test, !p);
	p = tb_property_find(network_dir, "deviceid", TB_PROPERTY_TYPE_TEXT);
	KUNIT_EXPECT_TRUE(test, !p);

	tb_property_free_dir(dir);
}

static void tb_test_property_format(struct kunit *test)
{
	struct tb_property_dir *dir;
	ssize_t block_len;
	u32 *block;
	int ret, i;

	dir = tb_property_parse_dir(root_directory, ARRAY_SIZE(root_directory));
	KUNIT_ASSERT_NOT_NULL(test, dir);

	ret = tb_property_format_dir(dir, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, ARRAY_SIZE(root_directory));

	block_len = ret;

	block = kunit_kzalloc(test, block_len * sizeof(u32), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, block);

	ret = tb_property_format_dir(dir, block, block_len);
	KUNIT_EXPECT_EQ(test, ret, 0);

	for (i = 0; i < ARRAY_SIZE(root_directory); i++)
		KUNIT_EXPECT_EQ(test, root_directory[i], block[i]);

	tb_property_free_dir(dir);
}

static void compare_dirs(struct kunit *test, struct tb_property_dir *d1,
			 struct tb_property_dir *d2)
{
	struct tb_property *p1, *p2, *tmp;
	int n1, n2, i;

	if (d1->uuid) {
		KUNIT_ASSERT_NOT_NULL(test, d2->uuid);
		KUNIT_ASSERT_TRUE(test, uuid_equal(d1->uuid, d2->uuid));
	} else {
		KUNIT_ASSERT_NULL(test, d2->uuid);
	}

	n1 = 0;
	tb_property_for_each(d1, tmp)
		n1++;
	KUNIT_ASSERT_NE(test, n1, 0);

	n2 = 0;
	tb_property_for_each(d2, tmp)
		n2++;
	KUNIT_ASSERT_NE(test, n2, 0);

	KUNIT_ASSERT_EQ(test, n1, n2);

	p1 = NULL;
	p2 = NULL;
	for (i = 0; i < n1; i++) {
		p1 = tb_property_get_next(d1, p1);
		KUNIT_ASSERT_NOT_NULL(test, p1);
		p2 = tb_property_get_next(d2, p2);
		KUNIT_ASSERT_NOT_NULL(test, p2);

		KUNIT_ASSERT_STREQ(test, &p1->key[0], &p2->key[0]);
		KUNIT_ASSERT_EQ(test, p1->type, p2->type);
		KUNIT_ASSERT_EQ(test, p1->length, p2->length);

		switch (p1->type) {
		case TB_PROPERTY_TYPE_DIRECTORY:
			KUNIT_ASSERT_NOT_NULL(test, p1->value.dir);
			KUNIT_ASSERT_NOT_NULL(test, p2->value.dir);
			compare_dirs(test, p1->value.dir, p2->value.dir);
			break;

		case TB_PROPERTY_TYPE_DATA:
			KUNIT_ASSERT_NOT_NULL(test, p1->value.data);
			KUNIT_ASSERT_NOT_NULL(test, p2->value.data);
			KUNIT_ASSERT_TRUE(test,
				!memcmp(p1->value.data, p2->value.data,
					p1->length * 4)
			);
			break;

		case TB_PROPERTY_TYPE_TEXT:
			KUNIT_ASSERT_NOT_NULL(test, p1->value.text);
			KUNIT_ASSERT_NOT_NULL(test, p2->value.text);
			KUNIT_ASSERT_STREQ(test, p1->value.text, p2->value.text);
			break;

		case TB_PROPERTY_TYPE_VALUE:
			KUNIT_ASSERT_EQ(test, p1->value.immediate,
					p2->value.immediate);
			break;
		default:
			KUNIT_FAIL(test, "unexpected property type");
			break;
		}
	}
}

static void tb_test_property_copy(struct kunit *test)
{
	struct tb_property_dir *src, *dst;
	u32 *block;
	int ret, i;

	src = tb_property_parse_dir(root_directory, ARRAY_SIZE(root_directory));
	KUNIT_ASSERT_NOT_NULL(test, src);

	dst = tb_property_copy_dir(src);
	KUNIT_ASSERT_NOT_NULL(test, dst);

	/* Compare the structures */
	compare_dirs(test, src, dst);

	/* Compare the resulting property block */
	ret = tb_property_format_dir(dst, NULL, 0);
	KUNIT_ASSERT_EQ(test, ret, ARRAY_SIZE(root_directory));

	block = kunit_kzalloc(test, sizeof(root_directory), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, block);

	ret = tb_property_format_dir(dst, block, ARRAY_SIZE(root_directory));
	KUNIT_EXPECT_TRUE(test, !ret);

	for (i = 0; i < ARRAY_SIZE(root_directory); i++)
		KUNIT_EXPECT_EQ(test, root_directory[i], block[i]);

	tb_property_free_dir(dst);
	tb_property_free_dir(src);
}

static void tb_test_xdomain_properties_stale(struct kunit *test)
{
	/*
	 * Models the generation gate in tb_xdomain_get_properties(). The peer's
	 * generation is monotonic WITHIN A SESSION (the remote always answers with
	 * its current/highest gen) and reseeds randomly on REBOOT, so the gate
	 * drops only an exact-duplicate re-read.
	 */

	/* First read (no cached properties) is always accepted. */
	KUNIT_EXPECT_FALSE(test, tb_xdomain_generation_stale(false, 1, 0));
	KUNIT_EXPECT_FALSE(test, tb_xdomain_generation_stale(false, 7, 0));

	/* Strictly newer generation is accepted. */
	KUNIT_EXPECT_FALSE(test, tb_xdomain_generation_stale(true, 8, 5));

	/* An exact duplicate (same gen we already hold) is dropped (skip re-parse). */
	KUNIT_EXPECT_TRUE(test, tb_xdomain_generation_stale(true, 5, 5));

	/*
	 * A LOWER generation can only mean the peer REBOOTED: the local block
	 * generation is seeded with get_random_u32() and only incremented, so a
	 * fresh boot reseeds to a value frequently below what a non-rebooted peer
	 * still caches. The directory genuinely changed (new boot, re-registered
	 * services), so it MUST be accepted -- the old `gen <= cached` gate stranded
	 * the peer forever when the best-effort PROPERTIES_CHANGED reset was lost
	 * (the appmana-002<->018 stranding: 002 rebooted, 018 dropped every re-read
	 * of 002's new ThunderboltIP service).
	 */
	KUNIT_EXPECT_FALSE(test, tb_xdomain_generation_stale(true, 5, 7));
	KUNIT_EXPECT_FALSE(test, tb_xdomain_generation_stale(true, 0x2a, 0xc0ffee99));

	/*
	 * Reconnect-recovery still works: resetting the cached generation to 0
	 * (done on PROPERTIES_CHANGED_REQUEST and by the rescan sysfs trigger)
	 * forces any real block to be accepted again, so services re-probe.
	 */
	KUNIT_EXPECT_FALSE(test, tb_xdomain_generation_stale(true, 5, 0));
	KUNIT_EXPECT_FALSE(test, tb_xdomain_generation_stale(true, 1, 0));
}

/*
 * Reproduces the real appmana-002<->018 stranding on a faithful two-host +
 * firmware model (link-edge-only XDOMAIN_CONNECTED, no re-notify on a
 * property-only change -- per the Titan/Maple Ridge disassembly). There is NO
 * "fix" toggle: the only lever is the real production predicate
 * tb_xdomain_generation_stale(). Reverting it (gen <= cached) strands the
 * rebooted-lower-gen peer below, exactly as reverting xdomain.c would.
 */
static void tb_test_xdomain_reboot_stranding(struct kunit *test)
{
	struct model_link L;

	/* Cold boot: link edge -> firmware event -> both hosts enumerate. */
	memset(&L, 0, sizeof(L));
	model_cold_boot(&L, 0xC0FFEE99u, 0x11111111u);
	KUNIT_EXPECT_TRUE(test, model_established(&L));

	/*
	 * Host A reboots to a LOWER generation and B gets no fresh firmware edge,
	 * so B's only recovery is the gated software re-read. The real gate
	 * accepts the lower generation and B re-enumerates; reverting the gate
	 * strands B here forever (the bug).
	 */
	memset(&L, 0, sizeof(L));
	model_cold_boot(&L, 0xC0FFEE99u, 0x11111111u);
	model_peer_reboot(&L.a, 0x0000002Au);
	model_run(&L, 50);
	KUNIT_EXPECT_TRUE(test, model_established(&L));

	/*
	 * Not rigged to always recover: a reboot to a HIGHER generation is
	 * accepted by both the old and new gate, so it must establish regardless
	 * -- proving the recovery above is specifically the lower-gen fix.
	 */
	memset(&L, 0, sizeof(L));
	model_cold_boot(&L, 0x00000005u, 0x11111111u);
	model_peer_reboot(&L.a, 0x00000009u);
	model_run(&L, 50);
	KUNIT_EXPECT_TRUE(test, model_established(&L));

	/* The recovery came from the gate (no firmware fallback exists). */
	KUNIT_EXPECT_FALSE(test,
			   tb_xdomain_generation_stale(true, 0x2Au, 0xC0FFEE99u));
	KUNIT_EXPECT_TRUE(test,
			  tb_xdomain_generation_stale(true, 0x2Au, 0x2Au));
}

/*
 * Co-reset strand: both peers reboot at once. The kernel reads the peer's
 * properties with a bounded budget (XDOMAIN_RETRIES) and, on exhaustion, today
 * goes to XDOMAIN_STATE_ERROR -> __stop_handshake() (terminal). On a simultaneous
 * reboot both peers are still booting, so each exhausts its budget before the
 * other answers, both stop, and a stopped host sends no XDP request so the Maple
 * ICM never re-arms -> permanent strand (the live appmana-020<->009 failure).
 * Reproduced here; the fix re-reads instead of stopping.
 */
static void tb_test_xdomain_coreset_strand(struct kunit *test)
{
	struct coreset_link L;

	/*
	 * Simultaneous reboot: both peers answer only at round 15, but the read
	 * budget is 10, so each exhausts before the other has booted. The link
	 * MUST still establish once both finish booting. With today's terminal
	 * give-up (__stop_handshake) it never re-reads and strands -- this is red
	 * until xdomain.c reschedules the read instead of failing.
	 */
	coreset_boot(&L, 15, 15);
	KUNIT_EXPECT_TRUE(test, coreset_run(&L, 100));
}

/*
 * Late host-to-host link: a chain node has two ports. One neighbour is up at
 * boot (enumerated by the initial scan); the other boots later, so its link
 * trains during this node's init window -- after its port's initial scan,
 * before hotplug is armed. tb_handle_hotplug drops events while
 * !tcm->hotplug_active, so that late link is enumerated by neither path and the
 * node never sees the second neighbour. Both ports MUST end up enumerated; red
 * until tb.c re-scans ports when hotplug is armed (appmana-009<->020).
 */
static void tb_test_xdomain_late_second_link(struct kunit *test)
{
	struct cm_host h;

	memset(&h, 0, sizeof(h));
	h.port[0].link_up = true;	/* neighbour 0 (025) up at boot */
	/* neighbour 1 (020) still booting: its link is down during the scan */

	cm_scan_port(&h, 0);		/* enumerates port 0 */
	cm_scan_port(&h, 1);		/* port 1 link down -> nothing */
	cm_link_up(&h, 1);		/* 020 boots: link trains, hotplug not armed -> dropped */
	cm_arm_hotplug(&h);		/* armed, but nothing re-scans port 1 */

	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);
	KUNIT_EXPECT_TRUE(test, h.port[1].xdomain);	/* red: late link missed */
}

/*
 * Firmware-proven model of the ICM warm/cold reset behaviour (kept in lockstep
 * with the userspace mirror tests/icm_reset_userspace.c). Reverse-engineering of
 * the Titan Ridge ICM firmware (AppMana/intel-thunderbolt-firmwares,
 * out/ICM_8051_FINDINGS.md) established that the NVM authentication asserting
 * REG_FW_STS_NVM_AUTH_DONE is performed ONLY by the on-die mask ROM at a true
 * chip reset -- it is absent from the flashed 8051/ARC application image. The
 * kernel's icm_firmware_reset() (ICM_EN_CPU) is a warm CPU restart that
 * re-enters the application image, which reads READ-ONLY reset-cause registers
 * (CA41/CB5E), sees "warm", and skips re-init -- so on Alpine/Titan Ridge it
 * never re-authenticates. Other families are deliberately not assigned an
 * outcome without a documented reset contract and direct hardware evidence.
 */
struct icm_fw_model {
	bool icm_en;	/* REG_FW_STS_ICM_EN: firmware running */
	bool authed;	/* REG_FW_STS_NVM_AUTH_DONE (set only by the mask ROM) */
	bool responsive; /* ICM message loop alive (a wedged ICM keeps ICM_EN) */
	bool cfg_open;	/* config space served (only after DRIVER_READY) */
	unsigned int warm_restarts; /* ICM_EN_CPU/ARC restarts since cold boot */
};

/* Cold boot / board power cycle: the mask ROM runs, authenticates, hands off. */
static void icm_fw_cold_boot(struct icm_fw_model *fw)
{
	fw->icm_en = true;
	fw->authed = true;
	fw->responsive = true;
	fw->cfg_open = false;
	fw->warm_restarts = 0;
}

/*
 * The observed wedge: the ICM stops servicing its message loop (DRIVER_READY
 * times out) but its status register still advertises ICM_EN -- the running
 * flag is NOT a liveness signal. icm_firmware_start()'s "already running ->
 * skip reset" gate therefore cannot distinguish a healthy ICM from this state.
 */
static void icm_fw_wedge(struct icm_fw_model *fw)
{
	fw->responsive = false;
}

/*
 * Alpine/Titan Ridge warm CPU restart. The mask ROM is not re-entered, so the
 * application image's reset-cause gate leaves the controller unauthenticated.
 */
static void icm_fw_warm_restart(struct icm_fw_model *fw)
{
	fw->icm_en = true;
	fw->authed = false;
	fw->responsive = true;
	fw->warm_restarts++;
}

/* ICM_DRIVER_READY succeeds only if running AND authenticated AND alive. */
static bool icm_fw_driver_ready(struct icm_fw_model *fw)
{
	if (!(fw->icm_en && fw->authed && fw->responsive))
		return false;
	/* Seeing DRIVER_READY is what makes the firmware serve config space. */
	fw->cfg_open = true;
	return true;
}

/*
 * Can the host read router config space over ring 0? With no firmware
 * resident the routers answer directly; a resident ICM serves (proxies)
 * config access only after it has seen DRIVER_READY.
 *
 * A WEDGED firmware is the exception: interception requires a live message
 * loop. A firmware whose loop is dead (DRIVER_READY times out) answers
 * nothing on ring 0 -- it neither serves nor blocks config space, so the
 * routers answer directly, exactly as if no firmware were resident. ICM_EN
 * alone is a latched status bit, not an active gate (appmana-009: a live
 * module reload latched ICM_EN with nothing behind it).
 */
static bool icm_fw_cfg_space_open(struct icm_fw_model *fw)
{
	return !fw->icm_en || !fw->responsive || fw->cfg_open;
}

static void tb_test_icm_warm_restart_loses_auth(struct kunit *test)
{
	struct icm_fw_model fw;

	/* Cold boot: mask ROM authenticated -> driver_ready succeeds. */
	icm_fw_cold_boot(&fw);
	KUNIT_EXPECT_TRUE(test, icm_fw_driver_ready(&fw));

	/*
	 * Alpine/Titan Ridge live rmmod+modprobe (a warm restart): the mask-ROM
	 * auth is skipped, so the firmware comes back NOT authenticated and
	 * driver_ready fails. No driver action on the unload path can recover it
	 * (the reason the reset-on-unload approach was reverted) -- it is terminal
	 * until a board power cycle.
	 */
	icm_fw_cold_boot(&fw);
	icm_fw_warm_restart(&fw);
	KUNIT_EXPECT_FALSE(test, icm_fw_driver_ready(&fw));

	/* Only a cold boot (reboot / power cycle) re-authenticates AR/TR. */
	icm_fw_cold_boot(&fw);
	KUNIT_EXPECT_TRUE(test, icm_fw_driver_ready(&fw));

}

/*
 * A wedged-but-running ICM: the message loop is dead (DRIVER_READY times out)
 * while REG_FW_STS still advertises ICM_EN. icm_firmware_start()'s "already
 * running -> skip reset" gate cannot see the difference, and a dead NHI whose
 * MMIO reads all-ones ALSO advertises ICM_EN. The first must produce an
 * explicit terminal diagnosis (warm reset provably cannot re-authenticate
 * AR/TR -- out/ICM_8051_FINDINGS.md); the second must not be treated as
 * "firmware running" at all.
 */
static void tb_test_icm_wedged_running(struct kunit *test)
{
	struct icm_fw_model fw;

	/* The wedge keeps ICM_EN: the reset gate's blind spot is real. */
	icm_fw_cold_boot(&fw);
	icm_fw_wedge(&fw);
	KUNIT_EXPECT_TRUE(test, fw.icm_en);
	KUNIT_EXPECT_FALSE(test, icm_fw_driver_ready(&fw));

	/*
	 * Forcing the warm reset the task-obvious "fix" would do: on AR/TR the
	 * firmware comes back UNAUTHENTICATED (mask ROM not re-entered), so the
	 * reset converts "wedged" into "unauthenticated" -- still dead, plus a
	 * multi-second stall. This is why icm.c must NOT blindly reset and must
	 * say "cold power cycle required" instead.
	 */
	icm_fw_cold_boot(&fw);
	icm_fw_wedge(&fw);
	icm_fw_warm_restart(&fw);
	KUNIT_EXPECT_FALSE(test, icm_fw_driver_ready(&fw));

	/*
	 * Dead NHI: MMIO reads all-ones, so REG_FW_STS spuriously asserts
	 * ICM_EN. The REAL predicate icm_firmware_running() is built on must
	 * treat ~0 as "no firmware", or a hung controller is routed into the
	 * firmware CM where DRIVER_READY can only time out.
	 */
	KUNIT_EXPECT_TRUE(test, tb_icm_fw_sts_running(REG_FW_STS_ICM_EN));
	KUNIT_EXPECT_FALSE(test, tb_icm_fw_sts_running(0));
	KUNIT_EXPECT_FALSE(test, tb_icm_fw_sts_running((u32)~0U));
}

/*
 * Model of the connection-manager selection pipeline: nhi_select_cm()'s
 * gate (tb_nhi_use_software_cm()) composed with the firmware gate of
 * icm_ar_is_supported() (icm_firmware_running() || start_icm). Built on the
 * same two shared predicates the driver uses, so the model cannot drift
 * from the code on the decision itself. Returns true when the domain ends
 * up with the software CM.
 */
static bool cm_model_selects_software(bool force_sw_cm, bool acpi_native,
				      u32 fw_sts, bool start_icm)
{
	if (tb_nhi_use_software_cm(force_sw_cm, acpi_native))
		return true;
	/* icm_probe(): AR/TR is_supported gate */
	if (tb_icm_fw_sts_running(fw_sts))
		return false;
	if (start_icm)
		return false;
	/* icm_probe() declined -> nhi_select_cm() falls back to tb_probe() */
	return true;
}

/*
 * A board can boot with ICM firmware resident (UEFI starts it pre-boot)
 * yet broken: links train, but the firmware never emits
 * device-connected, so nothing is ever enumerated (ASRock X570 Creator,
 * Titan Ridge NVM 45.0 -- unfixable: no NVM update exists and the resident
 * firmware cannot be stopped, see tb_test_icm_warm_restart_reauth). Without
 * an override, ICM_EN routes such a board into the firmware CM forever.
 * force_sw_cm must win over EVERY firmware state, including the poisoned
 * all-ones NHI; the unforced behavior must be unchanged.
 */
static void tb_test_cm_select_forced_software(struct kunit *test)
{
	static const u32 fw_states[] = { 0, REG_FW_STS_ICM_EN, ~0U };
	int i;

	for (i = 0; i < ARRAY_SIZE(fw_states); i++) {
		u32 fw_sts = fw_states[i];

		/* Forced: software CM regardless of firmware state. */
		KUNIT_EXPECT_TRUE(test,
			cm_model_selects_software(true, false, fw_sts, false));
		KUNIT_EXPECT_TRUE(test,
			cm_model_selects_software(true, false, fw_sts, true));
		KUNIT_EXPECT_TRUE(test,
			cm_model_selects_software(true, true, fw_sts, false));

		/* Native USB4 control: software CM, force not needed. */
		KUNIT_EXPECT_TRUE(test,
			cm_model_selects_software(false, true, fw_sts, false));
	}

	/* Unforced, non-native: running firmware still wins (stock behavior). */
	KUNIT_EXPECT_FALSE(test,
		cm_model_selects_software(false, false, REG_FW_STS_ICM_EN, false));
	/* start_icm still routes to the firmware CM when nothing is running. */
	KUNIT_EXPECT_FALSE(test,
		cm_model_selects_software(false, false, 0, true));
	/* No firmware, no start_icm: fallback to the software CM. */
	KUNIT_EXPECT_TRUE(test,
		cm_model_selects_software(false, false, 0, false));
	/* Poisoned NHI is not "firmware running": falls back to software. */
	KUNIT_EXPECT_TRUE(test,
		cm_model_selects_software(false, false, ~0U, false));
}

/*
 * Regression: the first forced-software boot on the X570 Creator failed
 * probe with -ETIMEDOUT because the software CM read the root switch
 * config space while the resident ICM -- which serves config access only
 * after DRIVER_READY -- had never been sent one. The forced takeover must
 * therefore send DRIVER_READY before the first read
 * (tb.c tb_driver_ready() -> icm_unlock_config_space()), while boards
 * with no resident firmware need nothing.
 */
/*
 * Model of the software CM takeover as tb.c's driver_ready hook performs
 * it (tb_driver_ready() -> icm_unlock_config_space()): with firmware
 * resident, send DRIVER_READY and report whether the config space opened;
 * with no firmware there is nothing to do. Kept in lockstep with the
 * driver -- the pre-fix driver sent nothing, which is the red state of
 * this test and the -ETIMEDOUT probe observed on the X570 Creator.
 */
static bool cm_model_forced_takeover(struct icm_fw_model *fw)
{
	if (!fw->icm_en)
		return true;
	if (!icm_fw_driver_ready(fw))
		return false;
	return icm_fw_cfg_space_open(fw);
}

static void tb_test_cm_forced_takeover_unlocks_config(struct kunit *test)
{
	struct icm_fw_model fw;

	/*
	 * Resident healthy ICM: config space is shut until DRIVER_READY,
	 * so the takeover must send it -- reading first is the regression.
	 */
	icm_fw_cold_boot(&fw);
	KUNIT_EXPECT_FALSE(test, icm_fw_cfg_space_open(&fw));
	KUNIT_EXPECT_TRUE(test, cm_model_forced_takeover(&fw));
	KUNIT_EXPECT_TRUE(test, icm_fw_cfg_space_open(&fw));

	/* No resident firmware (Apple boot): nothing to unlock. */
	icm_fw_cold_boot(&fw);
	fw.icm_en = false;
	KUNIT_EXPECT_TRUE(test, cm_model_forced_takeover(&fw));
	KUNIT_EXPECT_TRUE(test, icm_fw_cfg_space_open(&fw));

	/*
	 * A failed DRIVER_READY cannot establish exclusive ownership. Even if
	 * direct config reads happen to work, firmware may still consume another
	 * ring-0 protocol, so the forced takeover must fail.
	 */
	icm_fw_cold_boot(&fw);
	icm_fw_wedge(&fw);
	KUNIT_EXPECT_FALSE(test, cm_model_forced_takeover(&fw));
	KUNIT_EXPECT_TRUE(test, icm_fw_cfg_space_open(&fw));
}

/*
 * Model of the whole probe pipeline as nhi_probe() runs it: CM selection
 * (nhi_select_cm(), built on the same shared predicates as
 * cm_model_selects_software) followed by tb_domain_add() ->
 * cm_ops->driver_ready. Kept in lockstep with the driver on the wedged
 * branch (icm_driver_ready() + nhi_probe()).
 */
struct cm_probe_model_result {
	bool ok;		/* nhi_probe() succeeded */
	bool software;		/* the domain ended up with the software CM */
	bool wedged_diag;	/* the wedged-firmware diagnostic was emitted */
};

static struct cm_probe_model_result
cm_model_probe(struct icm_fw_model *fw, bool force_sw_cm, bool acpi_native)
{
	u32 fw_sts = fw->icm_en ? REG_FW_STS_ICM_EN : 0;
	struct cm_probe_model_result r = { };

	/* nhi_select_cm(): forced/native goes straight to the software CM. */
	if (tb_nhi_use_software_cm(force_sw_cm, acpi_native)) {
		r.software = true;
		r.ok = cm_model_forced_takeover(fw);
		return r;
	}

	/* icm_probe()'s is_supported gate (start_icm=N fleet-wide). */
	if (!tb_icm_fw_sts_running(fw_sts)) {
		r.software = true;
		r.ok = icm_fw_cfg_space_open(fw);
		return r;
	}

	/* Firmware CM selected: tb_domain_add() -> icm_driver_ready(). */
	if (icm_fw_driver_ready(fw)) {
		r.software = false;
		r.ok = true;
		return r;
	}

	/*
	 * DRIVER_READY failed with ICM_EN still latched. This diagnoses the
	 * firmware failure, but cannot prove that every ring-0 path has released
	 * ownership. Keep the firmware domain selected and fail the probe.
	 */
	r.wedged_diag = tb_icm_wedged(-ETIMEDOUT, fw_sts);
	return r;
}

/*
 * A DRIVER_READY timeout does not prove that the resident connection manager
 * is wholly dead. A partially responsive firmware can stop serving config
 * requests while it continues to accept local XDomain commands and consume
 * peer responses. Software takeover would then create two owners for ring 0
 * and leave a misleading, unusable domain behind. Refuse the takeover unless
 * exclusive ownership can be established.
 */
static void tb_test_icm_partial_wedge_refuses_software_takeover(struct kunit *test)
{
	struct cm_probe_model_result r;
	struct icm_fw_model fw;

	/* ICM_EN remains set after the firmware stops answering DRIVER_READY. */
	icm_fw_cold_boot(&fw);
	icm_fw_wedge(&fw);

	r = cm_model_probe(&fw, false, false);

	/* A timeout alone cannot establish exclusive software-CM ownership. */
	KUNIT_EXPECT_FALSE(test, r.ok);
	KUNIT_EXPECT_FALSE(test, r.software);
	/* The wedged-firmware diagnosis must still reach the operator. */
	KUNIT_EXPECT_TRUE(test, r.wedged_diag);
	/* The warm-reset refusal stays: no ARC/ICM restart was attempted... */
	KUNIT_EXPECT_EQ(test, fw.warm_restarts, 0u);
	/* ...so the NVM authentication was never thrown away. */
	KUNIT_EXPECT_TRUE(test, fw.authed);
}

/*
 * Firmware status, mailbox mode, DriverReady completion, and router config
 * access are independent startup proofs. In particular, a ready-looking
 * status register must not skip the later control-path checks.
 */
static void tb_test_icm_startup_proofs_are_separate(struct kunit *test)
{
	struct tb_icm_startup_proof proof;

	proof = tb_icm_startup_proof_begin(0x800001a1, NHI_FW_CM_MODE);
	KUNIT_EXPECT_TRUE(test, proof.firmware_ready);
	KUNIT_EXPECT_TRUE(test, proof.mailbox_ready);
	KUNIT_EXPECT_FALSE(test, proof.driver_ready);
	KUNIT_EXPECT_FALSE(test, proof.root_config_ready);

	/* A config response cannot complete a startup whose command did not. */
	tb_icm_startup_proof_advance(&proof, TB_ICM_PROOF_ROOT_CONFIG);
	KUNIT_EXPECT_FALSE(test, proof.root_config_ready);

	tb_icm_startup_proof_advance(&proof, TB_ICM_PROOF_DRIVER_READY);
	KUNIT_EXPECT_TRUE(test, proof.driver_ready);
	KUNIT_EXPECT_FALSE(test, proof.root_config_ready);

	tb_icm_startup_proof_advance(&proof, TB_ICM_PROOF_ROOT_CONFIG);
	KUNIT_EXPECT_TRUE(test, proof.root_config_ready);
}

/*
 * The generic config helper's retry group advances the protocol sequence.
 * Replacing it with independent one-request passes collapses that progression.
 */
static void tb_test_icm_root_config_probe_preserves_sequence_retries(struct kunit *test)
{
	/*
	 * Each readiness pass must retain the normal four-request sequence group.
	 * Recreating a one-request helper on every pass sends sequence zero over
	 * and over, which a controller may correctly treat as a duplicate.
	 */
	KUNIT_EXPECT_EQ(test, tb_icm_root_config_request_count(50), 50u);
	KUNIT_EXPECT_EQ(test, tb_icm_root_config_request_budget(50, 4), 200u);
	KUNIT_EXPECT_EQ(test, tb_icm_root_config_budget_ms(50, 4), 22500u);
}

/*
 * Recovery is a second state machine, not another startup-proof transition.
 * Only the proven post-DriverReady root-config timeout may request one cycle;
 * every other failure and every failure after the retry is terminal.
 */
static void tb_test_icm_root_recovery_is_separate_and_bounded(struct kunit *test)
{
	enum tb_icm_root_recovery_state state = TB_ICM_ROOT_RECOVERY_IDLE;
	enum tb_icm_root_recovery_state next;

	next = tb_icm_root_recovery_next(
		state, TB_ICM_ROOT_RECOVERY_OTHER_FAILURE);
	KUNIT_EXPECT_EQ(test, next, TB_ICM_ROOT_RECOVERY_TERMINAL);

	state = tb_icm_root_recovery_next(
		state, TB_ICM_ROOT_RECOVERY_ROOT_CONFIG_TIMEOUT);
	KUNIT_ASSERT_EQ(test, state,
			TB_ICM_ROOT_RECOVERY_POWER_CYCLE_PENDING);
	state = tb_icm_root_recovery_next(
		state, TB_ICM_ROOT_RECOVERY_POWER_CYCLE_DISPATCHED);
	KUNIT_ASSERT_EQ(test, state, TB_ICM_ROOT_RECOVERY_REPROBE_REQUIRED);
	next = tb_icm_root_recovery_next(
		state, TB_ICM_ROOT_RECOVERY_ROOT_CONFIG_TIMEOUT);
	KUNIT_EXPECT_EQ(test, next, TB_ICM_ROOT_RECOVERY_REPROBE_REQUIRED);
}

static void tb_test_nhi_recovery_is_exact_and_one_shot(struct kunit *test)
{
	enum tb_nhi_recovery_state state = TB_NHI_RECOVERY_IDLE;
	enum tb_nhi_recovery_state next;

	next = tb_nhi_recovery_next(state, TB_NHI_RECOVERY_OTHER_FAILURE);
	KUNIT_EXPECT_EQ(test, next, TB_NHI_RECOVERY_EXHAUSTED);

	state = tb_nhi_recovery_next(state,
				     TB_NHI_RECOVERY_ROOT_POWER_CYCLE_DISPATCHED);
	KUNIT_ASSERT_EQ(test, state, TB_NHI_RECOVERY_REPROBE_PENDING);
	state = tb_nhi_recovery_next(state,
				     TB_NHI_RECOVERY_REPROBE_DISPATCHED);
	KUNIT_ASSERT_EQ(test, state, TB_NHI_RECOVERY_RETRYING);
	next = tb_nhi_recovery_next(state,
				    TB_NHI_RECOVERY_ROOT_CONFIG_TIMEOUT);
	KUNIT_EXPECT_EQ(test, next, TB_NHI_RECOVERY_EXHAUSTED);
	KUNIT_EXPECT_TRUE(test,
		tb_nhi_startup_recovery_may_dispatch(TB_NHI_RECOVERY_IDLE));
	KUNIT_EXPECT_FALSE(test,
		tb_nhi_startup_recovery_may_dispatch(TB_NHI_RECOVERY_RETRYING));
}

static void tb_test_nhi_recovery_command_failure_is_terminal(struct kunit *test)
{
	enum tb_icm_root_recovery_state root =
		TB_ICM_ROOT_RECOVERY_POWER_CYCLE_PENDING;
	enum tb_nhi_recovery_state state;
	enum tb_nhi_recovery_state next;

	root = tb_icm_root_recovery_next(
		root, TB_ICM_ROOT_RECOVERY_POWER_CYCLE_FAILED);
	KUNIT_EXPECT_EQ(test, root, TB_ICM_ROOT_RECOVERY_TERMINAL);

	state = tb_nhi_recovery_next(
		TB_NHI_RECOVERY_IDLE,
		TB_NHI_RECOVERY_ROOT_POWER_CYCLE_FAILED);
	KUNIT_EXPECT_EQ(test, state, TB_NHI_RECOVERY_EXHAUSTED);
	next = tb_nhi_recovery_next(state,
				    TB_NHI_RECOVERY_ROOT_CONFIG_TIMEOUT);
	KUNIT_EXPECT_EQ(test, next, TB_NHI_RECOVERY_EXHAUSTED);

	state = TB_NHI_RECOVERY_REPROBE_PENDING;
	state = tb_nhi_recovery_next(state,
				     TB_NHI_RECOVERY_REPROBE_QUEUE_FAILED);
	KUNIT_EXPECT_EQ(test, state, TB_NHI_RECOVERY_EXHAUSTED);
}

/*
 * A response timeout does not say whether the controller consumed the command.
 * Recovery may schedule a proof reprobe only when either the response arrived
 * or the command's own TX descriptor completed. Keeping those observations
 * separate prevents a queued-but-stuck write from masquerading as recovery.
 */
static void tb_test_root_power_cycle_timeout_requires_consumption(struct kunit *test)
{
	enum tb_icm_root_recovery_event event;

	event = tb_icm_root_recovery_command_event(-ETIMEDOUT, false);
	KUNIT_EXPECT_EQ(test, event,
			TB_ICM_ROOT_RECOVERY_POWER_CYCLE_FAILED);

	event = tb_icm_root_recovery_command_event(-ETIMEDOUT, true);
	KUNIT_EXPECT_EQ(test, event,
			TB_ICM_ROOT_RECOVERY_POWER_CYCLE_DISPATCHED);

	/* A matched response independently proves command delivery. */
	event = tb_icm_root_recovery_command_event(0, false);
	KUNIT_EXPECT_EQ(test, event,
			TB_ICM_ROOT_RECOVERY_POWER_CYCLE_DISPATCHED);
}

static void tb_test_cfg_tx_consumption_is_independent_and_monotonic(struct kunit *test)
{
	struct tb_cfg_request_state state = { 0 };

	tb_cfg_request_tx_step(&state, TB_CFG_TX_EVENT_QUEUED);
	KUNIT_ASSERT_EQ(test, state.tx, TB_CFG_TX_QUEUED);
	tb_cfg_request_tx_step(&state, TB_CFG_TX_EVENT_CONSUMED);
	KUNIT_ASSERT_EQ(test, state.tx, TB_CFG_TX_CONSUMED);

	/* A later cancellation cannot erase observed hardware consumption. */
	tb_cfg_request_tx_step(&state, TB_CFG_TX_EVENT_CANCELED);
	KUNIT_EXPECT_EQ(test, state.tx, TB_CFG_TX_CONSUMED);
	KUNIT_EXPECT_EQ(test, state.local, TB_CFG_LOCAL_DISABLED);
	KUNIT_EXPECT_EQ(test, state.peer, TB_CFG_PEER_DISABLED);
}

static void tb_test_tx_index_progress_is_not_descriptor_completion(struct kunit *test)
{
	struct tb_ring_snapshot snapshot = {
		.running = true,
		.indices_valid = true,
		.in_flight = 1,
		.hw_producer = 3,
		.hw_consumer = 3,
	};

	KUNIT_EXPECT_FALSE(test, tb_nhi_tx_descriptor_completed(&snapshot));

	snapshot.tail_attributes = (u32)RING_DESC_COMPLETED << 20;
	KUNIT_EXPECT_TRUE(test, tb_nhi_tx_descriptor_completed(&snapshot));
}

static void tb_test_nhi_startup_recovery_requires_power_cycle_dispatch(struct kunit *test)
{
	enum tb_nhi_recovery_action action;
	enum tb_nhi_recovery_state state;

	action = tb_nhi_recovery_action(TB_NHI_RECOVERY_REPROBE_PENDING);
	KUNIT_EXPECT_EQ(test, action, TB_NHI_RECOVERY_ACTION_REPROBE);

	state = tb_nhi_recovery_next(TB_NHI_RECOVERY_IDLE,
				     TB_NHI_RECOVERY_DRIVER_READY_TIMEOUT);
	KUNIT_EXPECT_EQ(test, state, TB_NHI_RECOVERY_EXHAUSTED);
	state = tb_nhi_recovery_next(TB_NHI_RECOVERY_IDLE,
				     TB_NHI_RECOVERY_ROOT_CONFIG_TIMEOUT);
	KUNIT_EXPECT_EQ(test, state, TB_NHI_RECOVERY_EXHAUSTED);
}

static void tb_test_maple_root_power_cycle_encoding(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, DMA_PORT_MAIL_IN, 0x4fu);
	KUNIT_EXPECT_EQ(test, DMA_PORT_POWER_CYCLE_REQUEST, 0x40000001u);
	KUNIT_EXPECT_EQ(test,
			dma_port_for_nhi(PCI_VENDOR_ID_INTEL,
				PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_2C_NHI), 7);
	KUNIT_EXPECT_EQ(test,
			dma_port_for_nhi(PCI_VENDOR_ID_INTEL,
				PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_4C_NHI), 7);
	KUNIT_EXPECT_LT(test,
			dma_port_for_nhi(PCI_VENDOR_ID_INTEL,
				PCI_DEVICE_ID_INTEL_TITAN_RIDGE_4C_NHI), 0);
}

static void tb_test_pcie2cio_completion_states(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
			tb_pcie2cio_completion_state(PCIE2CIO_CMD_START),
			TB_PCIE2CIO_COMPLETION_BUSY);
	KUNIT_EXPECT_EQ(test, tb_pcie2cio_completion_state(0),
			TB_PCIE2CIO_COMPLETION_COMPLETE);
	KUNIT_EXPECT_EQ(test,
			tb_pcie2cio_completion_state(PCIE2CIO_CMD_TIMEOUT),
			TB_PCIE2CIO_COMPLETION_TARGET_TIMEOUT);
}

static void tb_test_nhi_recovery_success_completes_episode(struct kunit *test)
{
	enum tb_nhi_recovery_state state = TB_NHI_RECOVERY_RETRYING;
	enum tb_nhi_recovery_state next;

	state = tb_nhi_recovery_next(state,
				     TB_NHI_RECOVERY_PROBE_SUCCEEDED);
	KUNIT_EXPECT_EQ(test, state, TB_NHI_RECOVERY_COMPLETE);
	next = tb_nhi_recovery_next(state,
				    TB_NHI_RECOVERY_ROOT_CONFIG_TIMEOUT);
	KUNIT_EXPECT_EQ(test, next, TB_NHI_RECOVERY_COMPLETE);
}

static void tb_test_nhi_recovery_is_limited_to_proven_hardware(struct kunit *test)
{
	u16 vendor = PCI_VENDOR_ID_INTEL;
	u16 device;
	bool supported;

	device = PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_2C_NHI;
	supported = tb_nhi_recovery_supported(vendor, device);
	KUNIT_EXPECT_TRUE(test, supported);
	KUNIT_EXPECT_FALSE(test,
		tb_nhi_arc_cio_recovery_supported(vendor, device));
	device = PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_4C_NHI;
	supported = tb_nhi_recovery_supported(vendor, device);
	KUNIT_EXPECT_TRUE(test, supported);
	KUNIT_EXPECT_FALSE(test,
		tb_nhi_arc_cio_recovery_supported(vendor, device));
	device = PCI_DEVICE_ID_INTEL_TITAN_RIDGE_4C_NHI;
	supported = tb_nhi_recovery_supported(vendor, device);
	KUNIT_EXPECT_FALSE(test, supported);
	vendor = PCI_VENDOR_ID_AMD;
	device = PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_4C_NHI;
	supported = tb_nhi_recovery_supported(vendor, device);
	KUNIT_EXPECT_FALSE(test, supported);
}

static void tb_test_nhi_recovery_does_not_claim_reclaim_safety(struct kunit *test)
{
	unsigned int flags = tb_test_nhi_probe_recovery_wq_flags();

	KUNIT_EXPECT_EQ(test, flags & WQ_MEM_RECLAIM, 0u);
}

/*
 * Runtime data-path recovery is independent from startup proof and probe
 * recovery. A controller can complete both of those and later stop consuming
 * non-control TX descriptors while ring 0 remains healthy. Reusing the
 * completed startup state would make that failure unactionable.
 */
static void tb_test_nhi_runtime_recovery_is_a_separate_machine(struct kunit *test)
{
	enum tb_nhi_recovery_state startup = TB_NHI_RECOVERY_COMPLETE;
	enum tb_nhi_runtime_recovery_state runtime =
		TB_NHI_RUNTIME_RECOVERY_IDLE;

	runtime = tb_nhi_runtime_recovery_next(runtime, TB_NHI_RUNTIME_RECOVERY_DATA_TX_STALLED);

	KUNIT_EXPECT_EQ(test, startup, TB_NHI_RECOVERY_COMPLETE);
	KUNIT_EXPECT_EQ(test, runtime,
			TB_NHI_RUNTIME_RECOVERY_QUIESCE_PENDING);
}

static void tb_test_nhi_runtime_recovery_is_bounded(struct kunit *test)
{
	enum tb_nhi_runtime_recovery_state state =
		TB_NHI_RUNTIME_RECOVERY_IDLE;

	state = tb_nhi_runtime_recovery_next(state, TB_NHI_RUNTIME_RECOVERY_DATA_TX_STALLED);
	KUNIT_ASSERT_EQ(test, state,
			TB_NHI_RUNTIME_RECOVERY_QUIESCE_PENDING);
	state = tb_nhi_runtime_recovery_next(state, TB_NHI_RUNTIME_RECOVERY_QUIESCE_SUCCEEDED);
	KUNIT_ASSERT_EQ(test, state,
			TB_NHI_RUNTIME_RECOVERY_ICM_RESET_PENDING);
	state = tb_nhi_runtime_recovery_next(state, TB_NHI_RUNTIME_RECOVERY_ICM_RESET_SUCCEEDED);
	KUNIT_ASSERT_EQ(test, state,
			TB_NHI_RUNTIME_RECOVERY_REPROBE_PENDING);
	state = tb_nhi_runtime_recovery_next(state, TB_NHI_RUNTIME_RECOVERY_REPROBE_SUCCEEDED);
	KUNIT_ASSERT_EQ(test, state, TB_NHI_RUNTIME_RECOVERY_VERIFYING);
	state = tb_nhi_runtime_recovery_next(state, TB_NHI_RUNTIME_RECOVERY_DATA_TX_STALLED);
	KUNIT_EXPECT_EQ(test, state,
			TB_NHI_RUNTIME_RECOVERY_POWER_REQUIRED);
	state = tb_nhi_runtime_recovery_next(state, TB_NHI_RUNTIME_RECOVERY_DATA_PATH_PROVEN);
	KUNIT_EXPECT_EQ(test, state,
			TB_NHI_RUNTIME_RECOVERY_POWER_REQUIRED);
}

static void tb_test_nhi_runtime_recovery_requires_data_proof(struct kunit *test)
{
	enum tb_nhi_runtime_recovery_state state =
		TB_NHI_RUNTIME_RECOVERY_VERIFYING;

	state = tb_nhi_runtime_recovery_next(state, TB_NHI_RUNTIME_RECOVERY_DATA_PATH_PROVEN);
	KUNIT_EXPECT_EQ(test, state, TB_NHI_RUNTIME_RECOVERY_COMPLETE);
}

static void tb_test_nhi_runtime_recovery_failures_are_terminal(struct kunit *test)
{
	enum tb_nhi_runtime_recovery_state state;

	state = TB_NHI_RUNTIME_RECOVERY_QUIESCE_PENDING;
	state = tb_nhi_runtime_recovery_next(state, TB_NHI_RUNTIME_RECOVERY_QUIESCE_FAILED);
	KUNIT_EXPECT_EQ(test, state,
			TB_NHI_RUNTIME_RECOVERY_POWER_REQUIRED);

	state = TB_NHI_RUNTIME_RECOVERY_ICM_RESET_PENDING;
	state = tb_nhi_runtime_recovery_next(state, TB_NHI_RUNTIME_RECOVERY_ICM_RESET_FAILED);
	KUNIT_EXPECT_EQ(test, state,
			TB_NHI_RUNTIME_RECOVERY_POWER_REQUIRED);

	state = TB_NHI_RUNTIME_RECOVERY_REPROBE_PENDING;
	state = tb_nhi_runtime_recovery_next(state, TB_NHI_RUNTIME_RECOVERY_REPROBE_FAILED);
	KUNIT_EXPECT_EQ(test, state,
			TB_NHI_RUNTIME_RECOVERY_POWER_REQUIRED);
}

static void tb_test_nhi_dma_misc_policy_is_mutually_exclusive(struct kunit *test)
{
	u32 both = REG_DMA_MISC_INT_AUTO_CLEAR |
		   REG_DMA_MISC_DISABLE_AUTO_CLEAR;
	u32 unrelated = BIT(3) | BIT(27);
	u32 value;

	value = tb_nhi_dma_misc_interrupt_policy(both | unrelated, true);
	KUNIT_EXPECT_TRUE(test, value & REG_DMA_MISC_INT_AUTO_CLEAR);
	KUNIT_EXPECT_FALSE(test, value & REG_DMA_MISC_DISABLE_AUTO_CLEAR);
	KUNIT_EXPECT_EQ(test, value & unrelated, unrelated);

	value = tb_nhi_dma_misc_interrupt_policy(both | unrelated, false);
	KUNIT_EXPECT_FALSE(test, value & REG_DMA_MISC_INT_AUTO_CLEAR);
	KUNIT_EXPECT_TRUE(test, value & REG_DMA_MISC_DISABLE_AUTO_CLEAR);
	KUNIT_EXPECT_EQ(test, value & unrelated, unrelated);

	value = tb_nhi_dma_misc_interrupt_policy(0, true);
	KUNIT_EXPECT_EQ(test, value, (u32)REG_DMA_MISC_INT_AUTO_CLEAR);
	value = tb_nhi_dma_misc_interrupt_policy(0, false);
	KUNIT_EXPECT_EQ(test, value,
			(u32)REG_DMA_MISC_DISABLE_AUTO_CLEAR);
}

static void tb_test_maple_ridge_disables_interrupt_clear_on_read(struct kunit *test)
{
	u16 device;
	bool auto_clear;

	device = PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_2C_NHI;
	auto_clear = tb_nhi_uses_auto_clear(PCI_VENDOR_ID_INTEL, device);
	KUNIT_EXPECT_FALSE(test, auto_clear);
	device = PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_4C_NHI;
	auto_clear = tb_nhi_uses_auto_clear(PCI_VENDOR_ID_INTEL, device);
	KUNIT_EXPECT_FALSE(test, auto_clear);
	device = PCI_DEVICE_ID_INTEL_TITAN_RIDGE_4C_NHI;
	auto_clear = tb_nhi_uses_auto_clear(PCI_VENDOR_ID_INTEL, device);
	KUNIT_EXPECT_TRUE(test, auto_clear);
}

static void tb_test_nhi_tx_stall_requires_hardware_consumer_proof(struct kunit *test)
{
	struct tb_ring_snapshot first = {
		.size = 16,
		.hw_producer = 4,
		.hw_consumer = 0,
		.in_flight = 4,
		.options = RING_FLAG_ENABLE,
		.running = true,
		.indices_valid = true,
	};
	struct tb_ring_snapshot last = first;

	last.hw_producer = 5;
	last.in_flight = 5;
	KUNIT_EXPECT_TRUE(test, tb_nhi_tx_stalled(&first, &last, true));

	last.hw_consumer = 1;
	KUNIT_EXPECT_FALSE(test, tb_nhi_tx_stalled(&first, &last, true));
	last.hw_consumer = 0;
	KUNIT_EXPECT_FALSE(test, tb_nhi_tx_stalled(&first, &last, false));
	last.running = false;
	KUNIT_EXPECT_FALSE(test, tb_nhi_tx_stalled(&first, &last, true));
}

/*
 * ---------------------------------------------------------------------------
 * Domain teardown lock order (appmana-008, kdump 202608241850, 6.17.0-42).
 *
 * A wedged-ICM board failed tb_domain_add() at driver_ready, and the FAILURE
 * CLEANUP deadlocked:
 *
 *   (udev-worker):365   nhi_probe -> tb_domain_add -> tb_ctl_stop
 *                       -> tb_ring_stop -> flush_work -> wait_for_completion [D]
 *   kworker/u128:0:12   process_one_work -> icm_handle_notification
 *                       -> mutex_lock(&tb->lock)                            [D]
 *   INFO: task kworker/u128:0:12 is blocked on a mutex likely owned by
 *         task (udev-worker):365
 *   Kernel panic - not syncing: hung_task: blocked tasks
 *
 * tb_domain_add() takes tb->lock, then tb_ctl_start() opens event processing:
 * icm_handle_event() queues icm_handle_notification onto tb->wq, and that work
 * takes tb->lock. The failure path then called tb_ctl_stop() -- which
 * schedules AND FLUSHES ring work -- with tb->lock still held. Making a
 * tb->lock-taking work item runnable and then waiting on it while holding
 * tb->lock is an AB-BA nothing can break. tb_domain_remove() had the same
 * shape, which is why unload hung too, and tb_stop()/icm_stop() both ran their
 * only cancels under the same lock.
 *
 * This models the ORDER as observable state rather than trying to reproduce a
 * real deadlock: a wait-for-work while holding a lock that the work takes is
 * recorded as a violation. Lever TB_DOMAIN_TEARDOWN_UNLOCKED=0 models the
 * pre-2.46 driver and makes every case below fail.
 * ---------------------------------------------------------------------------
 */
#ifndef TB_DOMAIN_TEARDOWN_UNLOCKED
#define TB_DOMAIN_TEARDOWN_UNLOCKED 1
#endif

struct tb_lockorder_model {
	bool		lock_held;
	bool		ctl_running;
	/* a tb->lock-taking work item is queued on tb->wq */
	bool		notif_queued;
	bool		notif_ran;
	bool		wq_destroyed;
	/* armed delayed_work that flush_workqueue() does not wait for */
	bool		timer_armed;
	bool		hotplug_active;

	unsigned int	deadlocks;	/* waited on lock-taking work under lock */
	unsigned int	uaf;		/* work ran after the domain went away */
	unsigned int	queue_after_destroy;
};

static void lo_lock(struct tb_lockorder_model *m) { m->lock_held = true; }
static void lo_unlock(struct tb_lockorder_model *m) { m->lock_held = false; }

/* Control channel live: the peer/ICM can queue a tb->lock-taking work item. */
static void lo_ctl_event(struct tb_lockorder_model *m)
{
	if (m->ctl_running)
		m->notif_queued = true;
}

/* tb_ctl_stop() -> tb_ring_stop() -> flush_work(): a WAIT. */
static void lo_ctl_stop(struct tb_lockorder_model *m)
{
	if (m->lock_held && m->notif_queued)
		m->deadlocks++;
	m->ctl_running = false;
}

/* tb_handle_event() re-arming reconcile_work while hotplug is still active. */
static void lo_maybe_rearm(struct tb_lockorder_model *m)
{
	if (m->hotplug_active && m->ctl_running)
		m->timer_armed = true;
}

static void lo_flush_wq(struct tb_lockorder_model *m)
{
	/* Runs queued work; does NOT disarm an armed delayed_work timer. */
	if (m->notif_queued) {
		m->notif_queued = false;
		m->notif_ran = true;
	}
}

/* ->deinit, called with the lock dropped: the only place a sync cancel fits. */
static void lo_deinit(struct tb_lockorder_model *m)
{
	if (!TB_DOMAIN_TEARDOWN_UNLOCKED)
		return;		/* pre-2.46: nothing sync-cancelled these at all */
	if (m->lock_held && m->timer_armed) {
		m->deadlocks++;
		return;
	}
	m->timer_armed = false;
}

static void lo_destroy_wq(struct tb_lockorder_model *m)
{
	m->wq_destroyed = true;
	if (m->timer_armed) {
		/* __queue_work() on a destroyed wq, with tb already freed. */
		m->queue_after_destroy++;
		m->uaf++;
	}
	if (m->notif_queued)
		m->uaf++;
}

/* tb_domain_add() up to and including its failure cleanup. */
static void lo_domain_add_fails(struct tb_lockorder_model *m)
{
	memset(m, 0, sizeof(*m));
	lo_lock(m);
	m->ctl_running = true;		/* tb_ctl_start() */
	m->hotplug_active = true;
	lo_ctl_event(m);		/* driver_ready window: ICM notification */
	/* driver_ready() fails -> err_ctl_stop */
	if (TB_DOMAIN_TEARDOWN_UNLOCKED) {
		lo_unlock(m);
		lo_ctl_stop(m);
		lo_flush_wq(m);
	} else {
		lo_ctl_stop(m);
		lo_unlock(m);
	}
	m->hotplug_active = false;
	lo_deinit(m);
	lo_destroy_wq(m);
}

/* tb_domain_remove(): cm_ops->stop under the lock, ctl stop outside it. */
static void lo_domain_remove(struct tb_lockorder_model *m, bool peer_active)
{
	lo_lock(m);
	/* cm_ops->stop(): close the re-arm gate FIRST, then non-sync cancel. */
	if (TB_DOMAIN_TEARDOWN_UNLOCKED) {
		m->hotplug_active = false;
		lo_maybe_rearm(m);	/* gate closed: cannot re-arm */
	} else {
		lo_maybe_rearm(m);	/* gate still open: re-arms behind us */
		m->hotplug_active = false;
	}
	if (peer_active)
		lo_ctl_event(m);

	if (TB_DOMAIN_TEARDOWN_UNLOCKED) {
		lo_unlock(m);
		lo_ctl_stop(m);
	} else {
		lo_ctl_stop(m);
		lo_unlock(m);
	}
	lo_flush_wq(m);
	lo_deinit(m);			/* lock dropped: sync cancel is safe */
	lo_destroy_wq(m);
}

/*
 * The exact appmana-008 panic: probe failure cleanup with an ICM notification
 * in flight.
 */
static void tb_test_domain_add_failure_no_deadlock(struct kunit *test)
{
	struct tb_lockorder_model m;

	lo_domain_add_fails(&m);

	KUNIT_EXPECT_EQ(test, 0u, m.deadlocks);
	/* The in-flight notification must have been drained, not abandoned. */
	KUNIT_EXPECT_TRUE(test, m.notif_ran);
	KUNIT_EXPECT_EQ(test, 0u, m.uaf);
	KUNIT_EXPECT_FALSE(test, m.lock_held);
}

/* The same cleanup reached through module unload, peer still talking. */
static void tb_test_domain_remove_no_deadlock(struct kunit *test)
{
	struct tb_lockorder_model m;

	memset(&m, 0, sizeof(m));
	m.ctl_running = true;
	m.hotplug_active = true;
	lo_domain_remove(&m, true);

	KUNIT_EXPECT_EQ(test, 0u, m.deadlocks);
	KUNIT_EXPECT_EQ(test, 0u, m.uaf);
	KUNIT_EXPECT_EQ(test, 0u, m.queue_after_destroy);
	KUNIT_EXPECT_FALSE(test, m.lock_held);
}

/*
 * An armed delayed_work timer survives flush_workqueue(). Closing the re-arm
 * gate before the cancels, and sync-cancelling from ->deinit with the lock
 * dropped, is what stops it firing after destroy_workqueue() and kfree(tb).
 */
static void tb_test_domain_remove_no_work_after_destroy(struct kunit *test)
{
	struct tb_lockorder_model m;

	memset(&m, 0, sizeof(m));
	m.ctl_running = true;
	m.hotplug_active = true;
	lo_domain_remove(&m, false);

	KUNIT_EXPECT_FALSE(test, m.timer_armed);
	KUNIT_EXPECT_EQ(test, 0u, m.queue_after_destroy);
}

/*
 * The v2.44 wedged-ICM takeover routes a wedged board through the
 * tb_domain_add() failure cleanup TWICE (ICM attempt, then software CM), so it
 * multiplies exposure to this path rather than avoiding it. Both passes must
 * be clean.
 */
static void tb_test_icm_takeover_teardown_no_deadlock(struct kunit *test)
{
	struct tb_lockorder_model m;
	unsigned int deadlocks = 0, uaf = 0;
	int i;

	for (i = 0; i < 2; i++) {
		lo_domain_add_fails(&m);
		deadlocks += m.deadlocks;
		uaf += m.uaf;
	}

	KUNIT_EXPECT_EQ(test, 0u, deadlocks);
	KUNIT_EXPECT_EQ(test, 0u, uaf);
}

/*
 * The takeover fallback must NOT trigger for a healthy responding ICM
 * (that one really does answer ring 0 itself) nor rewrite the plain
 * no-firmware software path.
 */
static void tb_test_icm_healthy_keeps_firmware_cm(struct kunit *test)
{
	struct cm_probe_model_result r;
	struct icm_fw_model fw;

	/* Healthy responsive ICM: firmware CM, no wedge diagnosis. */
	icm_fw_cold_boot(&fw);
	r = cm_model_probe(&fw, false, false);
	KUNIT_EXPECT_TRUE(test, r.ok);
	KUNIT_EXPECT_FALSE(test, r.software);
	KUNIT_EXPECT_FALSE(test, r.wedged_diag);
	KUNIT_EXPECT_EQ(test, fw.warm_restarts, 0u);

	/* No firmware at all (cold boot on 009): plain software CM. */
	icm_fw_cold_boot(&fw);
	fw.icm_en = false;
	r = cm_model_probe(&fw, false, false);
	KUNIT_EXPECT_TRUE(test, r.ok);
	KUNIT_EXPECT_TRUE(test, r.software);
	KUNIT_EXPECT_FALSE(test, r.wedged_diag);
}

/*
 * Reproduces the 2026-07-09/10 appmana-019<->008 stranding: the survivor gets
 * NO unplug event when its neighbour's link drops (008's kernel log is silent
 * across 019's reboot while the lane adapter reads UNPLUGGED), and a plug
 * event can be lost the same way. tb_handle_hotplug() is edge-triggered only,
 * so the software topology diverges from hardware forever: the stale XDomain
 * (and its usb4_rdma ib_device) stays registered against a dead link -- NCCL
 * hangs -- and a trained link stays un-enumerated. cm_reconcile() is lockstep
 * with tb.c's reconciliation; without it in the driver these go red.
 */
static void tb_test_cm_reconcile_lost_unplug(struct kunit *test)
{
	struct cm_host h;

	memset(&h, 0, sizeof(h));
	h.port[0].link_up = true;
	h.port[1].link_up = true;
	cm_scan_port(&h, 0);
	cm_scan_port(&h, 1);
	cm_arm_hotplug(&h);
	KUNIT_EXPECT_TRUE(test, h.port[1].xdomain);

	/* 019 reboots; 008's unplug event never arrives (observed live). */
	cm_link_down_lost(&h, 1);
	KUNIT_EXPECT_TRUE(test, h.port[1].xdomain);	/* stale, NCCL hangs */

	/* Reconciliation converges software topology to the lane state. */
	cm_reconcile(&h);
	KUNIT_EXPECT_FALSE(test, h.port[1].xdomain);

	/* The healthy port is untouched. */
	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);
}

static void tb_test_cm_reconcile_lost_plug(struct kunit *test)
{
	struct cm_host h;

	memset(&h, 0, sizeof(h));
	h.port[0].link_up = true;
	cm_scan_port(&h, 0);
	cm_arm_hotplug(&h);

	/* Peer's link trains but the plug event is lost/absorbed. */
	cm_link_up_lost(&h, 1);
	KUNIT_EXPECT_FALSE(test, h.port[1].xdomain);	/* missed by both paths */

	cm_reconcile(&h);
	KUNIT_EXPECT_TRUE(test, h.port[1].xdomain);	/* synthesized plug */

	/* Reconciliation while disarmed (init/suspend) must do nothing. */
	memset(&h, 0, sizeof(h));
	cm_link_up_lost(&h, 0);
	cm_reconcile(&h);
	KUNIT_EXPECT_FALSE(test, h.port[0].xdomain);
}

/*
 * Reproduces the 2026-08-20 appmana-001 live-tunnel teardown: under
 * force_sw_cm the software CM enumerated the Razer TB4 dock and activated
 * its PCIe tunnel; sixteen seconds later (one reconcile poll after the
 * resident ICM's stray "event 0xa") the root port's lane state read
 * UNPLUGGED while the dock still answered config space -- the driver's own
 * warning said "router present but lane state 7" and synthesized the
 * unplug anyway, tearing down the live tunnel and kicking the PHY of a
 * live link. The resident ICM chewed on that real edge, root-switch
 * config reads started timing out, and the domain (and the desktop behind
 * its sysfs) hung.
 *
 * Contract: one lane-state sample is not evidence of an unplug. The
 * reconcile may synthesize an unplug only when the enumerated child/peer
 * router also fails a bounded config-space probe. A dead peer (the
 * appmana-008/019 stranding this reconcile exists for) fails the probe
 * and is still torn down; a live link with a perturbed state register is
 * left alone.
 */
static void tb_test_cm_reconcile_perturbed_lane_keeps_live_link(struct kunit *test)
{
	struct cm_host h;

	memset(&h, 0, sizeof(h));
	h.port[0].link_up = true;
	cm_scan_port(&h, 0);
	cm_arm_hotplug(&h);
	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);

	/*
	 * Resident ICM / CLx perturbs the lane-state register; the link and
	 * its tunnel are alive and the router answers the probe.
	 */
	h.port[0].state_perturbed = true;
	KUNIT_EXPECT_TRUE(test, cm_lane_sample_unplugged(&h, 0));
	KUNIT_EXPECT_TRUE(test, cm_peer_probe(&h, 0));

	cm_reconcile(&h);
	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);	/* live link kept */

	/* The perturbation clears; nothing may have changed. */
	h.port[0].state_perturbed = false;
	cm_reconcile(&h);
	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);

	/* A REAL unplug still converges: sample unplugged AND probe dead. */
	cm_link_down_lost(&h, 0);
	KUNIT_EXPECT_FALSE(test, cm_peer_probe(&h, 0));
	cm_reconcile(&h);
	KUNIT_EXPECT_FALSE(test, h.port[0].xdomain);
}

/*
 * Reproduces the 2026-08-22 appmana-001 enclosure freeze: under force_sw_cm
 * (resident Titan Ridge ICM) an ASMedia 246x NVMe enclosure was surprise-
 * removed; pciehp tore the tunneled PCIe subtree down cleanly, the
 * reconcile correctly synthesized the lost unplug ("router present but
 * lane state 7") and the teardown completed -- and then the post-teardown
 * PHY kick (tb_port_kick_detection) bounced the root port's lanes. Three
 * seconds later the machine froze silently (journal stops mid-line, no
 * hung-task output, mouse alive, all disk I/O wedged): the resident ICM
 * consumed the host-generated lane edge and wedged the NHI in the known
 * MMIO-stall class. Same firmware failure as the 2026-08-20 live-link
 * kick, now proven for a genuinely empty port too.
 *
 * Contract: a synthesized unplug must still converge the topology on every
 * host, but the detection re-arm kick is issued ONLY where no resident
 * firmware coexists (chain segments, where the 019<->008 latch-off it
 * exists for was observed). Under a resident ICM the kick must never be
 * issued: the enumerated child is the reconcile's to tear down, the lane
 * edge is the firmware's to own.
 */
static void tb_test_cm_reconcile_no_kick_under_resident_icm(struct kunit *test)
{
	struct cm_host h;

	/* Resident-ICM host (force_sw_cm) with an enumerated child. */
	memset(&h, 0, sizeof(h));
	h.resident_icm = true;
	h.port[0].link_up = true;
	cm_scan_port(&h, 0);
	cm_arm_hotplug(&h);
	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);

	/* Real surprise removal; the hot event is lost. */
	cm_link_down_lost(&h, 0);
	KUNIT_EXPECT_FALSE(test, cm_peer_probe(&h, 0));
	cm_reconcile(&h);

	/* The unplug is still synthesized... */
	KUNIT_EXPECT_FALSE(test, h.port[0].xdomain);
	/* ...but no lane edge is generated: the resident ICM stays sane. */
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
	KUNIT_EXPECT_FALSE(test, h.port[0].detection_rearmed);

	/* Chain host (no resident firmware): the 019<->008 re-arm stays. */
	memset(&h, 0, sizeof(h));
	h.port[1].link_up = true;
	cm_scan_port(&h, 1);
	cm_arm_hotplug(&h);
	cm_link_down_lost(&h, 1);
	cm_reconcile(&h);
	KUNIT_EXPECT_FALSE(test, h.port[1].xdomain);
	KUNIT_EXPECT_TRUE(test, h.port[1].detection_rearmed);
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
}

static void cm_run_reconcile(struct cm_host *h, int passes)
{
	while (passes-- > 0)
		cm_reconcile(h);
}

/*
 * The live appmana-001 port 0:4 failure (2026-08-22): an ASM246x NVMe
 * enclosure's link half-trains and parks at CONNECTING ("failed to reach
 * state TB_PORT_UP. Ignoring port...", LANE_ADP_CS_1 state 1). CONNECTING
 * is neither present nor gone, so without a retrain the reconcile ignores
 * the port FOREVER -- and with the blind kick correctly gated out under the
 * resident ICM (05d1e4c) nothing else ever gives the lane a fresh training
 * edge. The same disk enumerated cleanly earlier the same morning: only a
 * new edge is missing.
 *
 * Contract: an enclosure attached to a resident-ICM host must eventually
 * enumerate via the bounded plug-directed retrain, and the retrain must
 * never wedge the firmware (it waits out the ICM's transition window).
 */
static void tb_test_cm_plug_retrain_half_trained_resident_icm(struct kunit *test)
{
	struct cm_host h;

	memset(&h, 0, sizeof(h));
	h.resident_icm = true;
	cm_arm_hotplug(&h);

	cm_enclosure_attach_marginal(&h, 0, /*dead=*/false);
	cm_run_reconcile(&h, 12);

	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
	KUNIT_EXPECT_EQ(test, h.port[0].lane_edges, 1);

	/* The same recovery must work on a chain host (no firmware). */
	memset(&h, 0, sizeof(h));
	cm_arm_hotplug(&h);
	cm_enclosure_attach_marginal(&h, 1, /*dead=*/false);
	cm_run_reconcile(&h, 12);
	KUNIT_EXPECT_TRUE(test, h.port[1].xdomain);
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
}

/*
 * Detection latched off under a resident ICM: the lane samples UNPLUGGED
 * forever despite the powered enclosure on the cable, so the lane itself
 * carries NO plug signal -- but the resident firmware still watches the
 * port and emits its ICM-protocol notification (the live "unexpected event
 * 0xa"). That notification is the connect request that directs the
 * retrain. On a chain host no such notification exists and an UNPLUGGED
 * lane must never be bounced by the reconcile (the blind start-time kick
 * owns that case); this is what keeps the state-7 retrain plug-DIRECTED
 * rather than a periodic blind bounce.
 */
static void tb_test_cm_plug_retrain_latched_port_resident_icm(struct kunit *test)
{
	struct cm_host h;

	memset(&h, 0, sizeof(h));
	h.resident_icm = true;
	cm_arm_hotplug(&h);

	/* Idle for a while first: no hint, no edges on the empty port. */
	cm_run_reconcile(&h, 10);
	KUNIT_EXPECT_EQ(test, h.port[0].lane_edges, 0);

	cm_enclosure_attach_latched(&h, 0);
	cm_run_reconcile(&h, 12);

	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
	KUNIT_EXPECT_EQ(test, h.port[0].lane_edges, 1);

	/* Chain host: no notification -> the reconcile never bounces a
	 * bare UNPLUGGED lane, no matter how long it sits. */
	memset(&h, 0, sizeof(h));
	cm_arm_hotplug(&h);
	cm_enclosure_attach_latched(&h, 0);
	cm_run_reconcile(&h, 40);
	KUNIT_EXPECT_EQ(test, h.port[0].lane_edges, 0);
	KUNIT_EXPECT_FALSE(test, h.port[0].xdomain);
}

/*
 * Electrically dead (or hopelessly marginal) hardware: every fresh edge
 * ends parked at CONNECTING again. The retrain must be BOUNDED -- a fixed
 * number of spaced attempts per episode, then leave the port alone -- and
 * must never wedge the resident firmware while doing so (each host edge
 * makes the firmware busy again; the attempt spacing must outlast that).
 */
static void tb_test_cm_plug_retrain_bounded_dead_hardware(struct kunit *test)
{
	struct cm_host h;

	memset(&h, 0, sizeof(h));
	h.resident_icm = true;
	cm_arm_hotplug(&h);

	cm_enclosure_attach_marginal(&h, 0, /*dead=*/true);
	cm_run_reconcile(&h, 100);

	KUNIT_EXPECT_FALSE(test, h.port[0].xdomain);
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
	KUNIT_EXPECT_EQ(test, h.port[0].lane_edges, CM_RETRAIN_MAX_ATTEMPTS);
}

/*
 * The 05d1e4c regression guard, restated for the retrain: a REAL surprise
 * unplug of an enumerated enclosure under the resident ICM raises the
 * firmware's own notification AND leaves the firmware mid-transition. The
 * reconcile must synthesize the unplug, and the retrain must sit out both
 * the teardown cooldown and the firmware's busy window before it touches
 * the (now empty) port -- an early edge is exactly the 2026-08-22 silent
 * freeze. The single late edge it does issue is the harmless re-arm that
 * readies the latched-off port for the NEXT plug.
 */
static void tb_test_cm_plug_retrain_post_unplug_cooldown(struct kunit *test)
{
	struct cm_host h;

	memset(&h, 0, sizeof(h));
	h.resident_icm = true;
	h.port[0].link_up = true;
	cm_scan_port(&h, 0);
	cm_arm_hotplug(&h);
	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);

	cm_link_down_lost(&h, 0);

	/* Inside the firmware's transition window: teardown only, no edge. */
	cm_run_reconcile(&h, 3);
	KUNIT_EXPECT_FALSE(test, h.port[0].xdomain);
	KUNIT_EXPECT_EQ(test, h.port[0].lane_edges, 0);
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);

	/* After the quiet window: at most one directed re-arm, no wedge. */
	cm_run_reconcile(&h, 30);
	KUNIT_EXPECT_FALSE(test, h.port[0].xdomain);
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
	KUNIT_EXPECT_LE(test, h.port[0].lane_edges, 1);
}

/*
 * The ACTUAL live appmana-001 topology decoded 2026-08-22: the two
 * "ports" 0:3/0:4 are the two LANES of one connector (dual pair (3,4),
 * primaries iterate; "0:4 failed to reach state TB_PORT_UP" is
 * tb_switch_lane_bonding_enable() waiting on the dual_link_port). The
 * KIOXIA enclosure's router (0-3) enumerates on lane 0, but lane 1 parks
 * at CONNECTING, bonding fails once at enumeration and is NEVER retried,
 * and the degraded x1 link's PCIe payload stays dead ("Slot(4): No
 * link"). Contract: the reconcile retrains the half-trained SECONDARY
 * lane only (the primary, carrying the router, is never bounced -- a
 * full-connector bounce of a live link is the 2026-08-20 ICM wedge), and
 * once the secondary lane is up it re-runs lane bonding, which upstream
 * only ever attempts at enumeration.
 */
static void tb_test_cm_lane1_bonding_recovery_resident_icm(struct kunit *test)
{
	struct cm_host h;

	memset(&h, 0, sizeof(h));
	h.resident_icm = true;
	cm_arm_hotplug(&h);

	cm_enclosure_attach_bonding_stuck(&h, 0, /*dead=*/false);
	cm_run_reconcile(&h, 20);

	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);
	KUNIT_EXPECT_TRUE(test, h.port[0].bonded);
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
	KUNIT_EXPECT_EQ(test, h.port[0].lane1_edges, 1);
	/* The primary lane was never bounced. */
	KUNIT_EXPECT_EQ(test, h.port[0].lane_edges, 0);

	/*
	 * Secondary lane already up but the link runs x1 (bonding raced or
	 * failed at enumeration): late bonding needs no lane edge at all.
	 */
	memset(&h, 0, sizeof(h));
	h.resident_icm = true;
	h.port[1].link_up = true;
	h.port[1].lane1_sample = CM_SAMPLE_UP;
	cm_scan_port(&h, 1);
	cm_arm_hotplug(&h);
	cm_run_reconcile(&h, 12);
	KUNIT_EXPECT_TRUE(test, h.port[1].bonded);
	KUNIT_EXPECT_EQ(test, h.port[1].lane1_edges, 0);
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
}

/*
 * A secondary lane that can never train: bounded lane-directed attempts
 * (and, since the connector-replug escalation, bounded full-connector
 * episodes -- see the replug tests), then the link is left alone at x1
 * -- degraded but working, clean logs, no wedge. Exactly the "prove
 * driver-side behavior correct on dead hardware" contract.
 */
static void tb_test_cm_lane1_retrain_bounded_dead_lane(struct kunit *test)
{
	struct cm_host h;

	memset(&h, 0, sizeof(h));
	h.resident_icm = true;
	cm_arm_hotplug(&h);

	cm_enclosure_attach_bonding_stuck(&h, 0, /*dead=*/true);
	cm_run_reconcile(&h, 100);

	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);	/* x1 still works */
	KUNIT_EXPECT_FALSE(test, h.port[0].bonded);
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
	KUNIT_EXPECT_EQ(test, h.port[0].lane1_edges, CM_RETRAIN_MAX_ATTEMPTS);
	KUNIT_EXPECT_EQ(test, h.port[0].lane_edges, 0);
}

/*
 * The live 2026-08-23 appmana-001 recovery: the ASM246x enclosure's
 * secondary lane never wakes on single-lane edges (host lane 1 parked
 * CONNECTING, device lane 1 UNPLUGGED -- its receiver arms only during
 * the connector handshake), but a full-connector software replug (both
 * lanes disabled together, held past the firmware's busy window, enabled
 * together, then a fresh enumeration) trains the link to bonded x2.
 * Contract: once the bounded single-lane attempts are exhausted the
 * reconcile escalates to a bounded connector replug -- tearing the
 * software topology down FIRST (a bounce of an enumerated primary is the
 * 2026-08-20 wedge), holding both lanes down together, re-enabling
 * together, and re-enumerating through the normal synthesized-plug path,
 * where bonding-at-enumeration finds both lanes up.
 */
static void tb_test_cm_connector_replug_recovers_full_train_only_lane(struct kunit *test)
{
	struct cm_host h;

	memset(&h, 0, sizeof(h));
	h.resident_icm = true;
	cm_arm_hotplug(&h);

	cm_enclosure_attach_bonding_stuck_full_train(&h, 0, /*dead=*/false);
	cm_run_reconcile(&h, 100);

	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);
	KUNIT_EXPECT_TRUE(test, h.port[0].bonded);
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
	/* Single-lane attempts exhaust first, then ONE connector replug. */
	KUNIT_EXPECT_EQ(test, h.port[0].lane1_edges, CM_RETRAIN_MAX_ATTEMPTS);
	KUNIT_EXPECT_EQ(test, h.port[0].connector_edges, 1);
	/* No single-lane glitch edge ever touched the primary. */
	KUNIT_EXPECT_EQ(test, h.port[0].lane_edges, 0);
}

/*
 * A secondary lane that not even a full-connector train recovers: the
 * replug episodes are bounded, and -- critically -- each failed episode
 * still brings the ROUTER back (the primary lane is not dead, so the
 * fresh enumeration lands at x1). The port must never be left torn down
 * or with its lanes held.
 */
static void tb_test_cm_connector_replug_bounded_when_lane1_dead(struct kunit *test)
{
	struct cm_host h;

	memset(&h, 0, sizeof(h));
	h.resident_icm = true;
	cm_arm_hotplug(&h);

	cm_enclosure_attach_bonding_stuck_full_train(&h, 0, /*dead=*/true);
	cm_run_reconcile(&h, CM_REPLUG_IDLE_RESET_PASSES - 50);

	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);	/* x1 still works */
	KUNIT_EXPECT_FALSE(test, h.port[0].bonded);
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
	KUNIT_EXPECT_FALSE(test, h.port[0].lanes_held);
	KUNIT_EXPECT_EQ(test, h.port[0].replug_phase, CM_REPLUG_IDLE);
	KUNIT_EXPECT_EQ(test, h.port[0].connector_edges, CM_REPLUG_MAX_ATTEMPTS);
}

/*
 * A firmware notification lands while the lanes are held down (another
 * physical event on the domain): the re-enable edge must NOT be issued
 * into the firmware's busy window -- the hold extends until the
 * notification has aged, and the episode still completes. Enabling on
 * the raw hold expiry regardless is the 2026-08-22 wedge recipe (an edge
 * 3 s after a real unplug).
 */
static void tb_test_cm_connector_replug_defers_to_fresh_icm_event(struct kunit *test)
{
	struct cm_host h;
	int guard = 0;

	memset(&h, 0, sizeof(h));
	h.resident_icm = true;
	cm_arm_hotplug(&h);

	cm_enclosure_attach_bonding_stuck_full_train(&h, 0, /*dead=*/false);
	while (h.port[0].replug_phase != CM_REPLUG_HELD && guard++ < 60)
		cm_reconcile(&h);
	KUNIT_ASSERT_EQ(test, h.port[0].replug_phase, CM_REPLUG_HELD);

	cm_icm_notices_edge(&h, 0);
	cm_run_reconcile(&h, 100);

	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
	KUNIT_EXPECT_TRUE(test, h.port[0].bonded);
	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);
}

/*
 * A single-lane cable (no secondary lane modelled) enumerates x1 and is
 * left completely alone: the connector replug keys on a secondary lane
 * parked at CONNECTING -- hardware provably present on lane 1 -- never on
 * a lane that simply is not there.
 */
static void tb_test_cm_connector_replug_leaves_single_lane_cable(struct kunit *test)
{
	struct cm_host h;

	memset(&h, 0, sizeof(h));
	h.resident_icm = true;
	h.port[0].link_up = true;	/* lane1_sample stays 0 */
	cm_arm_hotplug(&h);

	cm_run_reconcile(&h, 200);

	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);
	KUNIT_EXPECT_FALSE(test, h.icm_wedged);
	KUNIT_EXPECT_EQ(test, h.port[0].connector_edges, 0);
	KUNIT_EXPECT_EQ(test, h.port[0].lane1_edges, 0);
	KUNIT_EXPECT_EQ(test, h.port[0].lane_edges, 0);
}

/*
 * Reproduces the 2026-07-09 appmana-008 port-1 loop: an XDomain whose cached
 * remote UUID is corrupt (66518780-00e3-212c-ffff-ffffffffffff, an all-ones
 * tail from a half-trained-link read) fails every property read -- the healthy
 * peer ignores mismatched dst_uuid -- and the PROPERTIES state re-queued
 * itself for 87 minutes without ever re-verifying the identity. The model is
 * lockstep with tb_xdomain_state_work(): while the driver re-queues PROPERTIES
 * blindly, ident_tick() must too, and the recovery expectation goes red.
 */
static void tb_test_xdomain_stale_identity_recovery(struct kunit *test)
{
	struct ident_peer peer = { .true_uuid = 0x66518780u, .answers = true };
	struct ident_xd xd = {
		.cached_uuid = 0xffffffffu,
		.cached_properties = true,
	};

	/* Corrupt cached identity against a healthy peer: must recover via
	 * UUID re-verification + CM replacement of the unplugged XDomain. */
	KUNIT_EXPECT_TRUE(test, ident_run(&xd, &peer, 32, /*cm_reconciles=*/true));

	/*
	 * Co-reset guard (f876653 behaviour must be preserved): a peer that is
	 * merely still BOOTING (answers nothing) must not be stranded by a
	 * terminal state; once it boots with the SAME identity the existing
	 * XDomain enumerates without CM replacement.
	 */
	memset(&xd, 0, sizeof(xd));
	xd.cached_uuid = 0x66518780u;
	peer.answers = false;
	KUNIT_EXPECT_FALSE(test, ident_run(&xd, &peer, 8, /*cm_reconciles=*/false));
	KUNIT_EXPECT_FALSE(test, xd.unplugged);
	peer.answers = true;
	KUNIT_EXPECT_TRUE(test, ident_run(&xd, &peer, 8, /*cm_reconciles=*/false));

	/*
	 * A peer that rebooted into a NEW identity while our read loop was
	 * failing: same corrupt-cache shape, recovers only through replacement.
	 */
	memset(&xd, 0, sizeof(xd));
	xd.cached_uuid = 0x11111111u;
	xd.cached_properties = true;
	peer.true_uuid = 0x22222222u;
	KUNIT_EXPECT_TRUE(test, ident_run(&xd, &peer, 32, /*cm_reconciles=*/true));
}

static void tb_test_xdomain_silent_stale_identity_recovery(struct kunit *test)
{
	struct ident_peer peer = {
		.true_uuid = 0x5bdf8780u,
		.answers = false,
	};
	struct ident_xd xd = {
		.cached_uuid = 0xffffffffu,
		.cached_properties = true,
	};

	/*
	 * Live 025<->023 failure: the old, already-enumerated XDomain had a
	 * half-read UUID and the rebooted peer answered neither property nor UUID
	 * requests. A mismatch response never arrives, so UUID revalidation alone
	 * cannot condemn it. After both bounded read budgets fail, the stale
	 * object must be replaced despite the physical lane remaining up.
	 */
	ident_tick(&xd, &peer); /* exhausted PROPERTIES -> UUID */
	KUNIT_EXPECT_FALSE(test, xd.unplugged);
	ident_tick(&xd, &peer); /* exhausted UUID -> condemn stale object */
	KUNIT_EXPECT_TRUE(test, xd.unplugged);

	/* Replacement is not terminal: the fresh object recovers when peer boots. */
	ident_cm_replace(&xd, &peer);
	KUNIT_EXPECT_FALSE(test, xd.unplugged);
	KUNIT_EXPECT_FALSE(test, xd.enumerated);
	peer.answers = true;
	KUNIT_EXPECT_TRUE(test, ident_run(&xd, &peer, 8,
					 /*cm_reconciles=*/true));
}

/*
 * Live 2026-08-24, appmana-008 <-> appmana-019 (Maple Ridge AM5 chain nodes):
 * 008 booted while 019 was POWERED OFF. The port facing 019 enumerated an
 * XDomain (0-3) whose remote UUID was the all-ones-tail placeholder a link with
 * no powered peer behind it reads back (385a8780-0004-402c-ffff-ffffffffffff).
 * That placeholder was latched as the XDomain's final identity, so:
 *
 *   - every property read failed (XDP requests carry dst_uuid), and
 *   - when 019 finally powered on and answered with its REAL UUID,
 *     tb_xdomain_get_uuid() read the difference as "another domain connected",
 *     set is_unplugged and drove the XDomain terminal.
 *
 * Under a resident ICM no tb.c reconcile pass runs to replace the condemned
 * object, so 0-3 never registered, no tbframe service ever bound, and 019 sent
 * unanswered HELLOs until 008 was rebooted with 019 already up.
 *
 * A chain neighbour powering on later is normal operation. Lockstep with
 * tb_xdomain_get_uuid(): while the driver treats a placeholder as an identity,
 * ident_tick() must too, and these expectations go red.
 */
static void tb_test_xdomain_late_peer_powered_on_after_boot(struct kunit *test)
{
	struct ident_peer peer = { .true_uuid = 0x385a8780u, .answers = false };
	struct ident_xd xd = {
		.cached_uuid = IDENT_UUID_PLACEHOLDER,
		.cached_properties = false,	/* first boot: never enumerated */
	};

	/*
	 * Neighbour powered off at boot: the handshake keeps re-verifying and
	 * must not condemn itself (the f876653 co-reset contract).
	 */
	KUNIT_EXPECT_FALSE(test, ident_run(&xd, &peer, 16,
					   /*cm_reconciles=*/false));
	KUNIT_EXPECT_FALSE(test, xd.unplugged);

	/*
	 * Neighbour powers on. The XDomain must adopt the real identity and
	 * enumerate WITHOUT a connection-manager replacement: a Maple Ridge
	 * chain node runs a resident ICM and never executes the tb.c reconcile
	 * that would replace a condemned object.
	 */
	peer.answers = true;
	KUNIT_EXPECT_TRUE(test, ident_run(&xd, &peer, 8,
					  /*cm_reconciles=*/false));
	KUNIT_EXPECT_EQ(test, xd.cached_uuid, peer.true_uuid);
	KUNIT_EXPECT_FALSE(test, xd.unplugged);
}

/*
 * The placeholder is not a peer identity, so it can never be the identity a
 * mismatch is judged against: the first REAL answer replaces it. A genuine
 * cached UUID keeps the existing condemn-and-replace behaviour, which is what
 * distinguishes this from the 2026-07-09 stale-identity case above.
 */
static void tb_test_xdomain_placeholder_uuid_never_final_identity(struct kunit *test)
{
	struct ident_peer peer = { .true_uuid = 0x66518780u, .answers = true };
	struct ident_xd xd = { .cached_uuid = IDENT_UUID_PLACEHOLDER };

	ident_tick(&xd, &peer);		/* PROPERTIES: dst_uuid mismatch */
	KUNIT_EXPECT_EQ(test, xd.state, IDENT_STATE_UUID);

	ident_tick(&xd, &peer);		/* UUID: adopt, do not condemn */
	KUNIT_EXPECT_FALSE(test, xd.unplugged);
	KUNIT_EXPECT_EQ(test, xd.cached_uuid, peer.true_uuid);

	/* The port is not poisoned: the adopted identity reads properties. */
	ident_tick(&xd, &peer);
	KUNIT_EXPECT_TRUE(test, xd.enumerated);

	/* A real identity that genuinely changed is still condemned. */
	memset(&xd, 0, sizeof(xd));
	xd.cached_uuid = 0x11111111u;
	xd.uuid_verified = true;
	xd.state = IDENT_STATE_UUID;
	ident_tick(&xd, &peer);
	KUNIT_EXPECT_TRUE(test, xd.unplugged);
}

/*
 * The responsive-peer path must not pay for any of the above: a peer whose
 * identity we already hold enumerates on the first PROPERTIES tick, with no
 * UUID detour and no re-verification churn.
 */
static void tb_test_xdomain_responsive_peer_registers_promptly(struct kunit *test)
{
	struct ident_peer peer = { .true_uuid = 0x385a8780u, .answers = true };
	struct ident_xd xd = {
		.cached_uuid = 0x385a8780u,
		.uuid_verified = true,
	};

	ident_tick(&xd, &peer);
	KUNIT_EXPECT_TRUE(test, xd.enumerated);
	KUNIT_EXPECT_EQ(test, xd.state, IDENT_STATE_PROPERTIES);
	KUNIT_EXPECT_FALSE(test, xd.unplugged);
}

static void tb_test_xdomain_event_uuid_requires_peer_verification(struct kunit *test)
{
	struct ident_peer peer = { .true_uuid = 0x12345678u, .answers = true };
	struct ident_xd xd = { .cached_uuid = peer.true_uuid };

	/*
	 * A firmware topology UUID is an address claim, not authentication.
	 * Firmware-managed controllers own UUID discovery, so prove the claim
	 * with the first route-local properties response instead of issuing a
	 * host UUID request the resident ICM consumes locally.
	 */
	KUNIT_EXPECT_FALSE(test, tb_test_xdomain_initial_needs_uuid(true));
	ident_tick(&xd, &peer);
	KUNIT_EXPECT_TRUE(test, xd.uuid_verified);
	KUNIT_EXPECT_TRUE(test, xd.enumerated);
}

static void tb_test_xdomain_properties_response_proves_both_identities(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, tb_test_xdomain_properties_identity(true, true));
	KUNIT_EXPECT_FALSE(test, tb_test_xdomain_properties_identity(false, true));
	KUNIT_EXPECT_FALSE(test, tb_test_xdomain_properties_identity(true, false));
	KUNIT_EXPECT_FALSE(test, tb_test_xdomain_properties_identity(false, false));
}

static void tb_test_xdomain_unverified_uuid_cannot_alias_route(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, tb_test_xdomain_uuid_globally_matchable(false, true));
	KUNIT_EXPECT_TRUE(test, tb_test_xdomain_uuid_globally_matchable(true, true));
	KUNIT_EXPECT_FALSE(test, tb_test_xdomain_uuid_globally_matchable(true, false));
}

/*
 * The other half of the 008 strand: the properties-changed announcement gave up
 * after XDOMAIN_RETRIES and was never queued again, so 008 stopped announcing
 * its property directory ~10 s into a boot whose neighbour was powered off.
 * The announce must outlive an arbitrarily long absence, and must do so without
 * a control packet per second or a log line per attempt.
 */
static void tb_test_xdomain_announce_survives_absent_peer(struct kunit *test)
{
	struct announce_state a = {};

	announce_arm(&a);

	/* Neighbour powered off for an hour. */
	announce_run(&a, /*peer_present=*/false, 3600u * 1000u);
	KUNIT_EXPECT_TRUE(test, a.armed);
	KUNIT_EXPECT_LT(test, a.attempts, 100u);
	KUNIT_EXPECT_LE(test, a.errs, 8u);

	/* Neighbour powers on: the next scheduled attempt delivers. */
	announce_run(&a, /*peer_present=*/true, 5u * 60u * 1000u);
	KUNIT_EXPECT_TRUE(test, a.delivered);
}

/*
 * A neighbour that NEVER powers on. Nothing may accumulate: the spacing must
 * saturate at the ceiling rather than grow the attempt rate, the diagnostics
 * must stop entirely, and the XDomain must sit in a well-defined
 * not-registered, not-condemned state -- no latch that would block the
 * negotiation if the neighbour were powered on afterwards.
 */
static void tb_test_xdomain_peer_never_answers_leaves_no_latch(struct kunit *test)
{
	struct ident_peer peer = { .true_uuid = 0x385a8780u, .answers = false };
	struct ident_xd xd = { .cached_uuid = IDENT_UUID_PLACEHOLDER };
	struct announce_state a = {};
	unsigned int day_ms = 24u * 3600u * 1000u;

	announce_arm(&a);
	announce_run(&a, /*peer_present=*/false, day_ms);

	/* Still trying, at the ceiling, in silence. */
	KUNIT_EXPECT_TRUE(test, a.armed);
	KUNIT_EXPECT_FALSE(test, a.delivered);
	KUNIT_EXPECT_LE(test, a.errs, TB_XDOMAIN_ANNOUNCE_MAX_SHIFT + 1u);
	KUNIT_EXPECT_EQ(test, tb_xdomain_announce_delay_ms(a.failures),
			(unsigned int)TB_XDOMAIN_ANNOUNCE_BASE_MS <<
				TB_XDOMAIN_ANNOUNCE_MAX_SHIFT);
	/* A day of absence costs one packet per ceiling period, no more. */
	KUNIT_EXPECT_LE(test, a.attempts,
			day_ms / (TB_XDOMAIN_ANNOUNCE_BASE_MS <<
				  TB_XDOMAIN_ANNOUNCE_MAX_SHIFT) + 8u);

	/* The XDomain is unregistered but not condemned and not terminal. */
	KUNIT_EXPECT_FALSE(test, ident_run(&xd, &peer, 512,
					   /*cm_reconciles=*/false));
	KUNIT_EXPECT_FALSE(test, xd.enumerated);
	KUNIT_EXPECT_FALSE(test, xd.unplugged);

	/* And none of that absence latched anything: the peer arriving works. */
	peer.answers = true;
	KUNIT_EXPECT_TRUE(test, ident_run(&xd, &peer, 8,
					  /*cm_reconciles=*/false));
	announce_run(&a, /*peer_present=*/true, 5u * 60u * 1000u);
	KUNIT_EXPECT_TRUE(test, a.delivered);
}

/*
 * The field case as it actually happened: the neighbour was powered on hours
 * after the local host booted. Both halves must still converge.
 */
static void tb_test_xdomain_peer_answers_after_many_attempts(struct kunit *test)
{
	struct ident_peer peer = { .true_uuid = 0x385a8780u, .answers = false };
	struct ident_xd xd = { .cached_uuid = IDENT_UUID_PLACEHOLDER };
	struct announce_state a = {};

	announce_arm(&a);
	announce_run(&a, /*peer_present=*/false, 6u * 3600u * 1000u);
	ident_run(&xd, &peer, 2048, /*cm_reconciles=*/false);
	KUNIT_EXPECT_FALSE(test, xd.enumerated);

	peer.answers = true;
	KUNIT_EXPECT_TRUE(test, ident_run(&xd, &peer, 8,
					  /*cm_reconciles=*/false));
	KUNIT_EXPECT_EQ(test, xd.cached_uuid, peer.true_uuid);

	announce_run(&a, /*peer_present=*/true, 5u * 60u * 1000u);
	KUNIT_EXPECT_TRUE(test, a.delivered);
}

/*
 * A link/peer-present event arriving mid-backoff must restore the fast cadence
 * rather than leave the announcement parked at the 64 s ceiling: the peer that
 * just appeared should not wait a minute to hear from us.
 */
static void tb_test_xdomain_announce_rearms_mid_backoff(struct kunit *test)
{
	struct announce_state a = {};
	unsigned int before;

	announce_arm(&a);
	announce_run(&a, /*peer_present=*/false, 30u * 60u * 1000u);
	KUNIT_EXPECT_EQ(test, tb_xdomain_announce_delay_ms(a.failures),
			(unsigned int)TB_XDOMAIN_ANNOUNCE_BASE_MS <<
				TB_XDOMAIN_ANNOUNCE_MAX_SHIFT);

	/* tb_xdomain_queue_properties_changed() on the peer-present event. */
	announce_arm(&a);
	KUNIT_EXPECT_EQ(test, a.failures, 0u);

	before = a.now_ms;
	announce_tick(&a, /*peer_present=*/true);
	KUNIT_EXPECT_TRUE(test, a.delivered);
	KUNIT_EXPECT_EQ(test, a.now_ms - before,
			(unsigned int)TB_XDOMAIN_ANNOUNCE_BASE_MS);
}

/*
 * An announcement that never gives up re-queues its own delayed work, so it
 * must still stop DETERMINISTICALLY: __stop_handshake() publishes the stop
 * marker before cancel_delayed_work_sync(), and an attempt already inside its
 * bounded wire wait re-reads that marker instead of re-arming past the cancel.
 * Otherwise a delayed work outlives suspend or module unload.
 */
static void tb_test_xdomain_announce_stops_deterministically(struct kunit *test)
{
	struct announce_state a = {};
	unsigned int attempts;

	announce_arm(&a);
	announce_run(&a, /*peer_present=*/false, 60u * 1000u);
	KUNIT_EXPECT_TRUE(test, a.armed);

	/* Stop lands while an attempt is in flight: the stop wins. */
	announce_tick_stopped_midflight(&a);
	KUNIT_EXPECT_FALSE(test, a.armed);

	/* Nothing runs afterwards, however long the domain lives. */
	attempts = a.attempts;
	announce_run(&a, /*peer_present=*/false, 24u * 3600u * 1000u);
	KUNIT_EXPECT_EQ(test, a.attempts, attempts);
	KUNIT_EXPECT_FALSE(test, a.armed);

	/* Resume re-arms it (tb_xdomain_resume -> start_handshake). */
	announce_arm(&a);
	KUNIT_EXPECT_FALSE(test, a.stopped);
	announce_tick(&a, /*peer_present=*/true);
	KUNIT_EXPECT_TRUE(test, a.delivered);
}

/*
 * The placeholder predicate, against the identities actually read off the
 * fleet on 2026-08-25. Only an ALL-ones UUID is the absence of a peer: with no
 * powered router behind the link every config dword reads back all-ones, so
 * all 16 bytes are 0xff.
 *
 * The low half alone is NOT a placeholder signal on this hardware. Maple Ridge
 * controllers genuinely report an all-ones low half and distinguish themselves
 * in the high half. Testing the low half classified every one of them as "no
 * peer", which folded a successful UUID read into -ENODATA and left the
 * XDomain looping in state UUID forever -- appmana-019 against appmana-027,
 * while 027's announcements were arriving normally the whole time.
 */
static void tb_test_xdomain_placeholder_uuid_predicate(struct kunit *test)
{
	/* No powered router behind the link: every dword reads all-ones. */
	static const u8 absent_peer[16] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	};
	/* appmana-019's OWN root switch: 385a8780-0004-402c-ffff-ffffffffffff */
	static const u8 real_019[16] = {
		0x38, 0x5a, 0x87, 0x80, 0x00, 0x04, 0x40, 0x2c,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	};
	/* appmana-027: 5bdf8780-004d-b184-ffff-ffffffffffff */
	static const u8 real_027[16] = {
		0x5b, 0xdf, 0x87, 0x80, 0x00, 0x4d, 0xb1, 0x84,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	};
	/* appmana-008: 385a8780-0025-7a2c-ffff-ffffffffffff */
	static const u8 real_008[16] = {
		0x38, 0x5a, 0x87, 0x80, 0x00, 0x25, 0x7a, 0x2c,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	};
	/* appmana-004 (Titan Ridge, no all-ones tail at all). */
	static const u8 real_004[16] = {
		0xcc, 0x03, 0x00, 0x00, 0x00, 0x70, 0x7c, 0x0e,
		0x83, 0xa2, 0xd8, 0x22, 0x06, 0xa4, 0x89, 0x24,
	};
	/* One byte short of all-ones: still an answer, not an absence. */
	static const u8 nearly[16] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
	};

	KUNIT_EXPECT_TRUE(test, tb_xdomain_uuid_is_placeholder(absent_peer));

	/* Every one of these is a live peer that must be reachable. */
	KUNIT_EXPECT_FALSE(test, tb_xdomain_uuid_is_placeholder(real_019));
	KUNIT_EXPECT_FALSE(test, tb_xdomain_uuid_is_placeholder(real_027));
	KUNIT_EXPECT_FALSE(test, tb_xdomain_uuid_is_placeholder(real_008));
	KUNIT_EXPECT_FALSE(test, tb_xdomain_uuid_is_placeholder(real_004));
	KUNIT_EXPECT_FALSE(test, tb_xdomain_uuid_is_placeholder(nearly));

	/* Distinct boards must not collapse to the same "identity". */
	KUNIT_EXPECT_NE(test, 0, memcmp(real_019, real_027, 16));
	KUNIT_EXPECT_NE(test, 0, memcmp(real_019, real_008, 16));
}

/*
 * NHI ring callbacks can consume an entire completed descriptor batch. The
 * 32-QP USB4 RDMA reproducer observed ring_work monopolizing a bound worker
 * for >10 ms before the first transport loss. Keep that potentially long
 * completion path off the CPU-bound system workqueue.
 */
static void tb_test_ring_work_uses_unbound_queue(struct kunit *test)
{
	KUNIT_EXPECT_PTR_EQ(test, tb_test_ring_workqueue(),
			    system_unbound_wq);
}

static void tb_test_ring_descriptor_is_one_complete_word(struct kunit *test)
{
	/*
	 * length[11:0], eof[15:12], sof[19:16], flags[31:20]. This literal
	 * catches both field placement and accidental omission of any field;
	 * production publishes this exact word with one WRITE_ONCE().
	 */
	KUNIT_EXPECT_EQ(test,
			tb_test_ring_descriptor_word(0xabc, 3, 1, 0xc),
			(u32)0x00c13abc);
}

/*
 * Maple Ridge host-to-host links on the live TB4 chain identify as Gen 4 but
 * initially train at 20 Gb/s x1. The XDomain handshake must not treat the
 * generation alone as proof that both lanes are already bonded.
 */
static void tb_test_xdomain_gen4_single_lane_negotiates_bonding(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
		tb_test_xdomain_should_negotiate_bonding(true, 4,
							 TB_LINK_WIDTH_SINGLE));
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_should_negotiate_bonding(true, 4,
							 TB_LINK_WIDTH_DUAL));
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_should_negotiate_bonding(false, 4,
							 TB_LINK_WIDTH_SINGLE));
}

/* ICM supplies the remote UUID, but that must only skip the UUID query. */
static void tb_test_xdomain_icm_still_initializes_link(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, tb_test_xdomain_should_initialize_link(true));
	KUNIT_EXPECT_TRUE(test, tb_test_xdomain_should_initialize_link(false));
}

static void tb_test_xdomain_icm_single_lane_enters_bonding_state(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
		tb_test_xdomain_initial_state_needs_link_status(false, true));
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_initial_state_needs_link_status(false, false));
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_initial_state_needs_link_status(true, true));
}

/*
 * Maple Ridge ICM peers reject the optional XDomain link-state protocol even
 * though both physical lane adapters are present. Before services are
 * published (and therefore before DMA tunnels exist), that precise result
 * may select the symmetric direct-bonding fallback only after the matching
 * peer confirms readiness. Never change width after service publication:
 * doing so under thunderbolt-net reproduced controller config-space timeouts
 * and an asymmetric disconnect on appmana-023/025.
 */
static void tb_test_xdomain_icm_unsupported_status_uses_early_direct_bonding(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
		tb_test_xdomain_should_fallback_to_direct_bonding(true, false,
								    -EOPNOTSUPP, true));
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_should_fallback_to_direct_bonding(false, false,
								     -EOPNOTSUPP, true));
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_should_fallback_to_direct_bonding(true, true,
								     -EOPNOTSUPP, true));
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_should_fallback_to_direct_bonding(true, false,
								     -ETIMEDOUT, true));
}

/*
 * ICM can announce a host-to-host edge on only one controller.  Arming lane
 * bonding from that single notification changed Maple Ridge hardware before
 * the other endpoint was ready and stranded the otherwise usable x1 link.
 */
static void tb_test_xdomain_one_sided_icm_announcement_never_arms_bonding(struct kunit *test)
{
	KUNIT_EXPECT_FALSE_MSG(test,
		tb_test_xdomain_should_fallback_to_direct_bonding(true, false,
								     -EOPNOTSUPP, false),
		"direct lane writes require confirmation from the matching peer");
}

static void tb_test_xdomain_two_port_bonding_does_not_serialize_peers(struct kunit *test)
{
	struct bond_model_link link;

	bond_model_appmana_023_025(&link);
	bond_model_run(&link,
		tb_test_xdomain_direct_bonding_blocks_controller());

	KUNIT_EXPECT_TRUE(test, bond_model_pair_bonded(&link));
	KUNIT_EXPECT_FALSE(test, link.destabilized);
}

static void tb_test_xdomain_never_bonds_after_services_publish(struct kunit *test)
{
	struct bond_model_link link;

	bond_model_appmana_023_025(&link);
	link.a.port[1].services_published = true;
	bond_model_run(&link, false);

	KUNIT_EXPECT_FALSE(test, bond_model_pair_bonded(&link));
	KUNIT_EXPECT_TRUE(test, link.destabilized);
}

/*
 * A failed early bond is not a link-down event. ICM may own lane-1 cleanup,
 * and an older peer may simply not support bonding. Disabling adapters here
 * can destroy the still-valid x1 XDomain and strand a hot-reloaded controller
 * until a physical edge or cold boot.
 */
static void tb_test_xdomain_bond_abort_preserves_single_lane(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_direct_bonding_abort_disables_lane());
}

/*
 * Boot skew is the fleet's x1 fixed point: the two ends' target-DUAL
 * windows never overlap during initial negotiation (sequential reboots),
 * every failure is terminal, and no one ever retries. Re-arm from
 * ENUMERATED must recover the pair - and because it can succeed AFTER
 * services published, the success path must re-announce the property dirs
 * so DMA paths are reborn with bonded credits (never unregister/re-register
 * live directories - the tb_reannounce_property_dirs() contract).
 */
static void tb_test_xdomain_bonding_rearm_recovers_boot_skew(struct kunit *test)
{
	struct bond_model_link link;

	bond_model_appmana_023_025(&link);
	/* The synchronous cold-boot scheduler: windows never overlap. */
	bond_model_run(&link, true);
	KUNIT_ASSERT_FALSE(test, bond_model_pair_bonded(&link));

	/* Services meanwhile came up on the x1 link. */
	link.a.port[1].services_published = true;
	link.b.port[0].services_published = true;

	bond_model_rearm(&link, 5);

	KUNIT_EXPECT_TRUE(test, bond_model_pair_bonded(&link));
	KUNIT_EXPECT_TRUE(test, link.a.port[1].services_republished);
	KUNIT_EXPECT_TRUE(test, link.b.port[0].services_republished);
	KUNIT_EXPECT_FALSE(test, link.destabilized);
}

/* Re-arm is bounded and an incapable peer never bonds or destabilizes. */
static void tb_test_xdomain_bonding_rearm_bounded_and_safe(struct kunit *test)
{
	struct bond_model_link link;

	bond_model_appmana_023_025(&link);
	bond_model_run(&link, true);

	/* Budget zero: stays x1, quietly. */
	bond_model_rearm(&link, 0);
	KUNIT_EXPECT_FALSE(test, bond_model_pair_bonded(&link));
	KUNIT_EXPECT_FALSE(test, link.destabilized);

	/* Incapable peer: retries exhaust without bonding or destabilizing. */
	link.b.port[0].capable = false;
	bond_model_rearm(&link, 5);
	KUNIT_EXPECT_FALSE(test, bond_model_pair_bonded(&link));
	KUNIT_EXPECT_FALSE(test, link.destabilized);

	/* The b5f07da constraint holds for the re-arm path too. */
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_bonding_rearm_touches_lanes_on_failure());
}

/*
 * The re-arm predicates in xdomain.c: an ENUMERATED, capable, unbonded
 * XDomain with budget left re-arms; a bonded or exhausted one does not.
 * The passive high side accepts a link-state-change request while
 * ENUMERATED (capable, unbonded), so the low side's re-arm window never
 * needs to overlap a 10 s park - which is what made boot skew terminal.
 */
static void tb_test_xdomain_bonding_rearm_predicates(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
		tb_test_xdomain_bonding_rearm_allowed(true, false, 0));
	KUNIT_EXPECT_TRUE(test,
		tb_test_xdomain_bonding_rearm_allowed(true, false, 4));
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_bonding_rearm_allowed(true, false, 5));
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_bonding_rearm_allowed(true, true, 0));
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_bonding_rearm_allowed(false, false, 0));

	/* Parked high side keeps accepting, as today. */
	KUNIT_EXPECT_TRUE(test,
		tb_test_xdomain_accepts_link_state_change(true, false,
							  true, false));
	/* ENUMERATED + capable + unbonded now accepts (the passive side). */
	KUNIT_EXPECT_TRUE(test,
		tb_test_xdomain_accepts_link_state_change(false, true,
							  true, false));
	/* Bonded or incapable ENUMERATED peers still refuse. */
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_accepts_link_state_change(false, true,
							  true, true));
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_accepts_link_state_change(false, true,
							  false, false));
	/* Any other state still refuses. */
	KUNIT_EXPECT_FALSE(test,
		tb_test_xdomain_accepts_link_state_change(false, false,
							  true, false));
}

/*
 * ICM owns physical-lane cleanup.  On appmana-025, module removal received an
 * ICM XDomain-disconnected notification after the NHI had stopped answering.
 * Link exit tried tb_port_lane_bonding_disable(), blocked in a config read,
 * held the XDomain lock against ring_work, and stranded rmmod thunderbolt.
 */
static void tb_test_xdomain_icm_teardown_never_touches_lane_hardware(struct kunit *test)
{
	KUNIT_EXPECT_FALSE_MSG(test,
		tb_test_xdomain_link_exit_touches_lane_hardware(true, 3),
		"firmware-CM teardown must not issue synchronous lane register I/O after an XDomain disconnect");
}

/*
 * XDomain protocol handler ABI and reload-race regressions (appmana-025
 * kdump 202608031305). A tbframe.ko rebuilt against the stock
 * <linux/thunderbolt.h> registered a handler laid out uuid/callback/data/
 * list while this core read the extended layout: the walk in
 * tb_xdomain_handle_request() fetched ->callback_xd from the offset the
 * module had written ->data into and jumped into the module's .bss
 * (NX-execute panic in ring_work, RIP == tbframe_global+0x0, data arg 0).
 * The extended member must therefore live BEHIND the stock members, and
 * dispatch must never read it for a registrant that set ->callback (whose
 * storage may end at ->list). Independently,
 * tb_unregister_protocol_handler() used to return while a dispatch walk
 * could still be inside the handler's callback, so a module reload could
 * free code the walk was executing.
 */

static const uuid_t tb_test_handler_uuid =
	UUID_INIT(0x3b9d1c60, 0x8f24, 0x4b11, 0xa5, 0x40,
		  0x0e, 0x0f, 0x27, 0x1c, 0x9f, 0x11);

struct handler_test_ctx {
	struct completion entered;
	struct completion release;
	struct completion dispatch_done;
	struct completion unreg_done;
	struct tb_protocol_handler *unreg_handler;
	struct tb *tb;
	struct tb_xdp_header pkt;
	atomic_t blocking_calls;
	atomic_t stock_calls;
	atomic_t xd_calls;
	atomic_t wrong_calls;
};

static struct tb *tb_test_handler_domain(struct kunit *test)
{
	struct tb *tb;

	tb = kunit_kzalloc(test, sizeof(*tb), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tb);
	mutex_init(&tb->lock);
	tb->root_switch = alloc_host(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tb->root_switch);
	return tb;
}

static void tb_test_handler_fill_pkt(struct tb_xdp_header *hdr)
{
	memset(hdr, 0, sizeof(*hdr));
	hdr->xd_hdr.length_sn = sizeof(*hdr) / 4 - sizeof(hdr->xd_hdr) / 4;
	uuid_copy(&hdr->uuid, &tb_test_handler_uuid);
}

/*
 * The extended source-aware member may only be appended after the stock
 * members: a registrant compiled against the stock header stores
 * uuid/callback/data/list at these exact offsets and its storage ends at
 * ->list. Reordering (the original mid-struct insertion of ->callback_xd)
 * re-introduces the NX-execute panic.
 */
static void tb_test_xdomain_handler_abi_stock_offsets(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, offsetof(struct tb_protocol_handler, uuid),
			0 * sizeof(void *));
	KUNIT_EXPECT_EQ(test, offsetof(struct tb_protocol_handler, callback),
			1 * sizeof(void *));
	KUNIT_EXPECT_EQ(test, offsetof(struct tb_protocol_handler, data),
			2 * sizeof(void *));
	KUNIT_EXPECT_EQ(test, offsetof(struct tb_protocol_handler, list),
			3 * sizeof(void *));
	KUNIT_EXPECT_GE(test, offsetof(struct tb_protocol_handler, callback_xd),
			offsetof(struct tb_protocol_handler, list) +
			sizeof(struct list_head));
}

static int tb_test_handler_stock_cb(const void *buf, size_t size, void *data)
{
	struct handler_test_ctx *ctx = data;

	atomic_inc(&ctx->stock_calls);
	return 1;
}

static int tb_test_handler_poison_xd_cb(struct tb_xdomain *xd, const void *buf,
					size_t size, void *data)
{
	struct handler_test_ctx *ctx = data;

	atomic_inc(&ctx->wrong_calls);
	return 1;
}

/*
 * A registrant that set ->callback may be stock-built: its storage ends at
 * ->list, so ->callback_xd would read adjacent garbage. Dispatch must call
 * ->callback and never even look at ->callback_xd (poisoned here to stand
 * in for that garbage).
 */
static void tb_test_xdomain_handler_dispatch_source_blind_registrant(struct kunit *test)
{
	struct tb_protocol_handler *handler;
	struct handler_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);
	ctx->tb = tb_test_handler_domain(test);
	tb_test_handler_fill_pkt(&ctx->pkt);

	handler = kunit_kzalloc(test, sizeof(*handler), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, handler);
	handler->uuid = &tb_test_handler_uuid;
	handler->callback = tb_test_handler_stock_cb;
	handler->callback_xd = tb_test_handler_poison_xd_cb;
	handler->data = ctx;
	KUNIT_ASSERT_EQ(test, tb_register_protocol_handler(handler), 0);

	KUNIT_EXPECT_TRUE(test,
		tb_xdomain_handle_request(ctx->tb, TB_CFG_PKG_XDOMAIN_REQ,
					  &ctx->pkt, sizeof(ctx->pkt)));

	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->stock_calls), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->wrong_calls), 0);

	tb_unregister_protocol_handler(handler);
}

static int tb_test_handler_blocking_cb(const void *buf, size_t size, void *data)
{
	struct handler_test_ctx *ctx = data;

	atomic_inc(&ctx->blocking_calls);
	complete(&ctx->entered);
	wait_for_completion(&ctx->release);
	return 1;
}

static int tb_test_handler_new_instance_xd_cb(struct tb_xdomain *xd,
					      const void *buf, size_t size,
					      void *data)
{
	struct handler_test_ctx *ctx = data;

	atomic_inc(&ctx->xd_calls);
	return 1;
}

static int tb_test_handler_dispatch_thread(void *arg)
{
	struct handler_test_ctx *ctx = arg;

	tb_xdomain_handle_request(ctx->tb, TB_CFG_PKG_XDOMAIN_REQ, &ctx->pkt,
				  sizeof(ctx->pkt));
	complete(&ctx->dispatch_done);
	return 0;
}

static int tb_test_handler_unreg_thread(void *arg)
{
	struct handler_test_ctx *ctx = arg;

	tb_unregister_protocol_handler(ctx->unreg_handler);
	complete(&ctx->unreg_done);
	return 0;
}

/*
 * The module reload race: the peer's control packet is being dispatched
 * into the old instance's callback while that instance unregisters.
 * tb_unregister_protocol_handler() must not return until the in-flight
 * callback has finished, and a re-registered new instance (same UUID,
 * fresh data) must then receive subsequent packets.
 */
static void tb_test_xdomain_handler_unregister_waits_for_dispatch(struct kunit *test)
{
	struct tb_protocol_handler *old_inst, *new_inst;
	struct handler_test_ctx *ctx;
	struct task_struct *task;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);
	init_completion(&ctx->entered);
	init_completion(&ctx->release);
	init_completion(&ctx->dispatch_done);
	init_completion(&ctx->unreg_done);
	ctx->tb = tb_test_handler_domain(test);
	tb_test_handler_fill_pkt(&ctx->pkt);

	old_inst = kunit_kzalloc(test, sizeof(*old_inst), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_inst);
	old_inst->uuid = &tb_test_handler_uuid;
	old_inst->callback = tb_test_handler_blocking_cb;
	old_inst->data = ctx;
	KUNIT_ASSERT_EQ(test, tb_register_protocol_handler(old_inst), 0);
	ctx->unreg_handler = old_inst;

	task = kthread_run(tb_test_handler_dispatch_thread, ctx,
			   "tb-test-dispatch");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, task);
	KUNIT_ASSERT_NE(test,
			wait_for_completion_timeout(&ctx->entered, 5 * HZ),
			0UL);

	task = kthread_run(tb_test_handler_unreg_thread, ctx, "tb-test-unreg");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, task);

	/*
	 * The callback is still blocked, so unregistration must not have
	 * returned. The pre-fix code returned immediately here, letting the
	 * module free the callback it was executing.
	 */
	KUNIT_EXPECT_EQ(test,
			wait_for_completion_timeout(&ctx->unreg_done,
						    msecs_to_jiffies(200)),
			0UL);

	complete(&ctx->release);
	KUNIT_EXPECT_NE(test,
			wait_for_completion_timeout(&ctx->unreg_done, 5 * HZ),
			0UL);
	KUNIT_EXPECT_NE(test,
			wait_for_completion_timeout(&ctx->dispatch_done, 5 * HZ),
			0UL);

	/* Reload: a new instance takes over the same UUID. */
	new_inst = kunit_kzalloc(test, sizeof(*new_inst), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_inst);
	new_inst->uuid = &tb_test_handler_uuid;
	new_inst->callback_xd = tb_test_handler_new_instance_xd_cb;
	new_inst->data = ctx;
	KUNIT_ASSERT_EQ(test, tb_register_protocol_handler(new_inst), 0);

	KUNIT_EXPECT_TRUE(test,
		tb_xdomain_handle_request(ctx->tb, TB_CFG_PKG_XDOMAIN_REQ,
					  &ctx->pkt, sizeof(ctx->pkt)));
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->xd_calls), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->blocking_calls), 1);

	tb_unregister_protocol_handler(new_inst);
}

/*
 * With two active XDomains on one controller (023: one port to 025, one to
 * an unresponsive 022), a TB_CFG_PKG_ERROR generated on the DEAD port's
 * route must not complete a pending XDP request addressed to the LIVE
 * peer. It did: tb_xdomain_match accepted any error packet uncondition-
 * ally, so the sibling port's error traffic poisoned the 023<->025
 * lane-bonding negotiation (link-status failed in ~2 ms with the peer
 * never receiving the request; observed 2026-08-18 on the canaries).
 * Same-route errors must still complete the request (that is how a
 * genuinely dead peer fails fast).
 */
static void tb_test_xdomain_error_match_is_route_checked(struct kunit *test)
{
	/* Error from the request's own route: completes it. */
	KUNIT_EXPECT_TRUE(test, tb_test_xdomain_error_pkg_matches(0x1, 0x1));
	/* Error from a different route (the sibling port's dead peer):
	 * must NOT complete it.
	 */
	KUNIT_EXPECT_FALSE(test, tb_test_xdomain_error_pkg_matches(0x1, 0x3));
	KUNIT_EXPECT_FALSE(test, tb_test_xdomain_error_pkg_matches(0x3, 0x1));
}

/*
 * A domain whose root switch failed to initialise still has a live ring 0,
 * so an inbound XDomain request can reach the lookup with tb->root_switch
 * still NULL. Field crash (appmana-023, 2026-08-24, stack 2.46):
 *   thunderbolt 0-0: failed to initialize port 1
 *   BUG: kernel NULL pointer dereference at switch_find_xdomain+0x11
 *   tb_xdomain_handle_request <- tb_domain_event_cb <- tb_ctl_rx_callback
 * The lookup must answer "no such XDomain" instead of dereferencing.
 */
static void tb_test_xdomain_lookup_without_root_switch(struct kunit *test)
{
	struct tb *tb;
	uuid_t uuid;

	tb = kunit_kzalloc(test, sizeof(*tb), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tb);
	tb->root_switch = NULL;
	uuid_gen(&uuid);

	KUNIT_EXPECT_PTR_EQ(test, tb_xdomain_find_by_uuid(tb, &uuid), NULL);
	KUNIT_EXPECT_PTR_EQ(test, tb_xdomain_find_by_link_depth(tb, 1, 1), NULL);
	/*
	 * The route lookup is the one the appmana-023 oops actually came
	 * through: tb_xdomain_handle_request -> tb_xdomain_find_by_route_locked
	 * -> tb_xdomain_find_by_route. Cover the reported path, not just its
	 * siblings.
	 */
	KUNIT_EXPECT_PTR_EQ(test, tb_xdomain_find_by_route(tb, 0x301), NULL);
}

/*
 * A firmware image having a reset vector does not establish a supported live
 * recovery contract. The reference driver uses the ARC bits only for startup
 * and never attempts this reset after an ICM timeout, so every family must
 * fail closed unless a documented and independently tested sequence exists.
 */
static void tb_test_icm_warm_restart_is_unsupported(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test,
			   tb_icm_warm_restart_supported(PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_4C_NHI));
	KUNIT_EXPECT_FALSE(test,
			   tb_icm_warm_restart_supported(PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_2C_NHI));

	/* Titan/Alpine Ridge: mask-ROM-only re-auth, warm restart is terminal. */
	KUNIT_EXPECT_FALSE(test,
		tb_icm_warm_restart_supported(PCI_DEVICE_ID_INTEL_TITAN_RIDGE_4C_NHI));
	KUNIT_EXPECT_FALSE(test,
		tb_icm_warm_restart_supported(PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_C_4C_NHI));

	/* Anything unexamined stays refused: the default must be the safe one. */
	KUNIT_EXPECT_FALSE(test, tb_icm_warm_restart_supported(0x0000));
	KUNIT_EXPECT_FALSE(test, tb_icm_warm_restart_supported(0xffff));
}

/* A trained lane whose peer router does not answer must remain retryable. */
static void tb_test_cm_plug_synth_scan_fails_keeps_retrying(struct kunit *test)
{
	struct cm_host h;
	int i;

	memset(&h, 0, sizeof(h));
	cm_arm_hotplug(&h);
	cm_peer_attach_router_mute(&h, 0);

	for (i = 0; i < 200; i++)
		cm_reconcile(&h);

	/* Nothing can enumerate behind a mute router. */
	KUNIT_EXPECT_FALSE(test, h.port[0].xdomain);
	/*
	 * But the port must not be abandoned after one attempt: the peer can
	 * start answering at any time (a neighbour finishing its boot), and
	 * only a retry discovers that.
	 */
	KUNIT_EXPECT_GT(test, h.port[0].plug_synth_failed, 1);
	/* Bounded, not a hot loop: one attempt per quiet window at most. */
	KUNIT_EXPECT_LE(test, h.port[0].plug_synth_failed,
			200 / CM_RETRAIN_QUIET_PASSES + 1);
}

/* A later response must let the bounded retry enumerate without a new edge. */
static void tb_test_cm_plug_synth_recovers_when_router_answers(struct kunit *test)
{
	struct cm_host h;
	int i;

	memset(&h, 0, sizeof(h));
	cm_arm_hotplug(&h);
	cm_peer_attach_router_mute(&h, 0);

	for (i = 0; i < 60; i++)
		cm_reconcile(&h);
	KUNIT_EXPECT_FALSE(test, h.port[0].xdomain);

	/* The neighbour finishes booting; its router answers config space. */
	h.port[0].router_mute = false;

	for (i = 0; i < 60; i++)
		cm_reconcile(&h);
	KUNIT_EXPECT_TRUE(test, h.port[0].xdomain);
}

static void tb_test_xdomain_response_match_follows_protocol_model(struct kunit *test)
{
	const u32 protocol = 0x584450;
	struct xdp_match_model_request request = {
		.route = 0x3,
		.type = UUID_REQUEST,
		.sequence = 2,
		.response_capacity = sizeof(struct tb_xdp_uuid_response),
		.protocol = protocol,
	};
	struct xdp_match_model_response response = {
		.route = 0x3,
		.type = UUID_RESPONSE,
		.sequence = 2,
		.size = sizeof(struct tb_xdp_uuid_response),
		.declared_size = sizeof(struct tb_xdp_uuid_response),
		.protocol = protocol,
	};
#define EXPECT_MATCH_MODEL() do { \
	bool expected = xdp_match_model_matches(&request, &response); \
	bool actual = tb_test_xdomain_response_pkg_matches( \
		request.route, request.type, request.sequence, \
		request.response_capacity, response.route, response.type, \
		response.sequence, response.size, response.declared_size, \
		response.protocol == request.protocol); \
	KUNIT_EXPECT_EQ(test, actual, expected); \
} while (0)

	/* Control: a complete response to the active request is accepted. */
	EXPECT_MATCH_MODEL();

	/* A late response from the previous sequence cannot complete this one. */
	response.sequence = 1;
	EXPECT_MATCH_MODEL();
	response.sequence = request.sequence;

	/* A response to another operation on the same route is not interchangeable. */
	response.type = PROPERTIES_CHANGED_RESPONSE;
	response.size = sizeof(struct tb_xdp_properties_changed_response);
	response.declared_size = response.size;
	EXPECT_MATCH_MODEL();
	response.type = UUID_RESPONSE;

	/* Lengths are bytes: reject both truncation and capacity overflow. */
	response.size = sizeof(struct tb_xdp_header);
	response.declared_size = response.size;
	EXPECT_MATCH_MODEL();
	response.size = request.response_capacity + sizeof(u32);
	response.declared_size = response.size;
	EXPECT_MATCH_MODEL();
	response.size = sizeof(struct tb_xdp_uuid_response);
	response.declared_size = response.size;

	/* The header and received frame must agree on the packet length. */
	response.declared_size -= sizeof(u32);
	EXPECT_MATCH_MODEL();
	response.declared_size = response.size;

	response.route = 0x1;
	EXPECT_MATCH_MODEL();
	response.route = request.route;
	response.protocol ^= 1;
	EXPECT_MATCH_MODEL();

	/* Properties replies are variable length and need not fill capacity. */
	request.type = PROPERTIES_REQUEST;
	request.response_capacity = sizeof(struct tb_xdp_properties_response) +
				    16 * sizeof(u32);
	response.type = PROPERTIES_RESPONSE;
	response.size = sizeof(struct tb_xdp_properties_response);
	response.declared_size = response.size;
	response.protocol = request.protocol;
	EXPECT_MATCH_MODEL();

	/* A protocol error is a valid completion for the active request. */
	response.type = ERROR_RESPONSE;
	response.size = sizeof(struct tb_xdp_error_response);
	response.declared_size = response.size;
	EXPECT_MATCH_MODEL();

#undef EXPECT_MATCH_MODEL
}

static void tb_test_xdomain_native_error_fails_request(struct kunit *test)
{
	/* Native control errors complete the request, but never as success. */
	KUNIT_EXPECT_EQ(test, tb_test_xdomain_native_error_result(), 1);
}

/*
 * Control-channel liveness (ctl.h). These gate whether tb_stop() is allowed to
 * push config space I/O at the controller during teardown -- see the
 * appmana-019 CIO hang recorded there.
 */
static void tb_test_ctl_liveness_threshold(struct kunit *test)
{
	/* A quiet channel and short stalls are not "dead". */
	KUNIT_EXPECT_FALSE(test, tb_ctl_timeouts_indicate_dead(0));
	KUNIT_EXPECT_FALSE(test, tb_ctl_timeouts_indicate_dead(1));
	KUNIT_EXPECT_FALSE(test, tb_ctl_timeouts_indicate_dead(2));

	/* Three in a row is the verdict, and it stays put above it. */
	KUNIT_EXPECT_TRUE(test, tb_ctl_timeouts_indicate_dead(3));
	KUNIT_EXPECT_TRUE(test, tb_ctl_timeouts_indicate_dead(4));
	KUNIT_EXPECT_TRUE(test, tb_ctl_timeouts_indicate_dead(9999));
}

static void tb_test_ctl_xdomain_tx_status_classification(struct kunit *test)
{
	struct icm_tr_pkg_xdomain_packet_response status = {
		.hdr = {
			.code = ICM_XDOMAIN_PACKET,
			.packet_id = 0,
			.total_packets = 1,
		},
	};
	struct ctl_rx_packet_model packet = {
		.eof = TB_CFG_PKG_ICM_RESP,
		.size = sizeof(status),
		.code = status.hdr.code,
		.flags = status.hdr.flags,
		.packet_id = status.hdr.packet_id,
		.total_packets = status.hdr.total_packets,
	};
#define EXPECT_TX_STATUS() do { \
	bool expected = ctl_model_is_xdomain_tx_status(&packet); \
	bool actual = tb_ctl_is_xdomain_tx_status(packet.eof, &status, \
						     packet.size); \
	KUNIT_EXPECT_EQ(test, actual, expected); \
} while (0)

	/* Success and error completions are both local transmit status. */
	EXPECT_TX_STATUS();
	status.hdr.flags = ICM_FLAGS_ERROR;
	packet.flags = status.hdr.flags;
	EXPECT_TX_STATUS();
	status.hdr.flags = 0;
	packet.flags = 0;

	/* The peer's end-to-end response remains a separate packet. */
	packet.eof = TB_CFG_PKG_XDOMAIN_RESP;
	EXPECT_TX_STATUS();
	packet.eof = TB_CFG_PKG_ICM_RESP;

	status.hdr.code = ICM_DISCONNECT_XDOMAIN;
	packet.code = status.hdr.code;
	EXPECT_TX_STATUS();
	status.hdr.code = ICM_XDOMAIN_PACKET;
	packet.code = status.hdr.code;

	status.hdr.packet_id = 1;
	packet.packet_id = status.hdr.packet_id;
	EXPECT_TX_STATUS();
	status.hdr.packet_id = 0;
	packet.packet_id = 0;

	status.hdr.total_packets = 2;
	packet.total_packets = status.hdr.total_packets;
	EXPECT_TX_STATUS();
	status.hdr.total_packets = 1;
	packet.total_packets = 1;

	packet.size = sizeof(status) - sizeof(u32);
	EXPECT_TX_STATUS();
	packet.size = sizeof(status) + sizeof(u32);
	EXPECT_TX_STATUS();

#undef EXPECT_TX_STATUS
}

static void tb_test_ctl_split_state_records_local_acceptance(struct kunit *test)
{
	struct tb_cfg_request_state state = {
		.local = TB_CFG_LOCAL_WAITING,
		.peer = TB_CFG_PEER_WAITING,
	};
	enum tb_cfg_request_action action;

	action = tb_cfg_request_state_step(&state,
					   TB_CFG_REQUEST_EVENT_LOCAL_ACCEPTED);

	KUNIT_EXPECT_EQ(test, state.local, TB_CFG_LOCAL_ACCEPTED);
	KUNIT_EXPECT_EQ(test, state.peer, TB_CFG_PEER_WAITING);
	KUNIT_EXPECT_EQ(test, action, TB_CFG_REQUEST_ACTION_NONE);
}

static void tb_test_ctl_split_state_preserves_accept_on_peer_timeout(struct kunit *test)
{
	struct tb_cfg_request_state state = {
		.local = TB_CFG_LOCAL_WAITING,
		.peer = TB_CFG_PEER_WAITING,
	};
	enum tb_cfg_request_action action;

	action = tb_cfg_request_state_step(&state,
					   TB_CFG_REQUEST_EVENT_LOCAL_ACCEPTED);
	KUNIT_ASSERT_EQ(test, action, TB_CFG_REQUEST_ACTION_NONE);
	action = tb_cfg_request_state_step(&state,
					   TB_CFG_REQUEST_EVENT_PEER_TIMED_OUT);

	KUNIT_EXPECT_EQ(test, state.local, TB_CFG_LOCAL_ACCEPTED);
	KUNIT_EXPECT_EQ(test, state.peer, TB_CFG_PEER_TIMED_OUT);
	KUNIT_EXPECT_EQ(test, action, TB_CFG_REQUEST_ACTION_FAIL);
}

static void tb_test_ctl_peer_timeout_keeps_local_command_waiting(struct kunit *test)
{
	struct tb_cfg_request_state state = {
		.local = TB_CFG_LOCAL_WAITING,
		.peer = TB_CFG_PEER_WAITING,
	};
	enum tb_cfg_request_action action;

	action = tb_cfg_request_state_step(&state,
					   TB_CFG_REQUEST_EVENT_PEER_TIMED_OUT);

	/* The peer deadline cannot stand in for the independent local deadline. */
	KUNIT_EXPECT_EQ(test, state.local, TB_CFG_LOCAL_WAITING);
	KUNIT_EXPECT_EQ(test, state.peer, TB_CFG_PEER_TIMED_OUT);
	KUNIT_EXPECT_EQ(test, action, TB_CFG_REQUEST_ACTION_FAIL);
	KUNIT_EXPECT_TRUE(test, tb_cfg_local_slot_is_owned(state.local));
}

static void tb_test_ctl_local_status_slot_blocks_reuse(struct kunit *test)
{
	/* The first command can claim an empty completion slot. */
	KUNIT_EXPECT_TRUE(test, tb_cfg_local_slot_may_claim(false));
	/* An unsequenced completion makes reuse while occupied unsafe. */
	KUNIT_EXPECT_FALSE(test, tb_cfg_local_slot_may_claim(true));
	KUNIT_EXPECT_TRUE(test,
			  tb_cfg_local_slot_is_owned(TB_CFG_LOCAL_WAITING));
	KUNIT_EXPECT_FALSE(test,
			   tb_cfg_local_slot_is_owned(TB_CFG_LOCAL_ACCEPTED));
	KUNIT_EXPECT_FALSE(test,
			   tb_cfg_local_slot_is_owned(TB_CFG_LOCAL_FAILED));
	KUNIT_EXPECT_FALSE(test,
			   tb_cfg_local_slot_is_owned(TB_CFG_LOCAL_TIMED_OUT));
}

static void tb_test_ctl_split_state_terminal_transitions(struct kunit *test)
{
	struct tb_cfg_request_state state = {
		.local = TB_CFG_LOCAL_WAITING,
		.peer = TB_CFG_PEER_WAITING,
	};
	enum tb_cfg_request_action action;

	action = tb_cfg_request_state_step(&state,
					   TB_CFG_REQUEST_EVENT_LOCAL_FAILED);
	KUNIT_EXPECT_EQ(test, state.local, TB_CFG_LOCAL_FAILED);
	KUNIT_EXPECT_EQ(test, state.peer, TB_CFG_PEER_CANCELED);
	KUNIT_EXPECT_EQ(test, action, TB_CFG_REQUEST_ACTION_FAIL);

	/* A late peer response cannot reverse the failed transaction. */
	action = tb_cfg_request_state_step(&state,
					   TB_CFG_REQUEST_EVENT_PEER_MATCHED);
	KUNIT_EXPECT_EQ(test, state.local, TB_CFG_LOCAL_FAILED);
	KUNIT_EXPECT_EQ(test, state.peer, TB_CFG_PEER_CANCELED);
	KUNIT_EXPECT_EQ(test, action, TB_CFG_REQUEST_ACTION_NONE);
}

static void tb_test_ctl_split_state_peer_response_is_independent(struct kunit *test)
{
	struct tb_cfg_request_state state = {
		.local = TB_CFG_LOCAL_WAITING,
		.peer = TB_CFG_PEER_WAITING,
	};
	enum tb_cfg_request_action action;

	action = tb_cfg_request_state_step(&state,
					   TB_CFG_REQUEST_EVENT_PEER_MATCHED);
	KUNIT_EXPECT_EQ(test, state.local, TB_CFG_LOCAL_WAITING);
	KUNIT_EXPECT_EQ(test, state.peer, TB_CFG_PEER_MATCHED);
	KUNIT_EXPECT_EQ(test, action, TB_CFG_REQUEST_ACTION_COMPLETE);

	/* The peer terminal state is first-wins. */
	action = tb_cfg_request_state_step(&state,
					   TB_CFG_REQUEST_EVENT_PEER_TIMED_OUT);
	KUNIT_EXPECT_EQ(test, state.peer, TB_CFG_PEER_MATCHED);
	KUNIT_EXPECT_EQ(test, action, TB_CFG_REQUEST_ACTION_NONE);
}

static void tb_test_ctl_xdomain_tx_status_route_correlation(struct kunit *test)
{
	enum tb_cfg_request_event event;

	event = tb_test_xdomain_intermediate_event(0x3, 0x3, false);
	KUNIT_EXPECT_EQ(test, event, TB_CFG_REQUEST_EVENT_LOCAL_ACCEPTED);
	event = tb_test_xdomain_intermediate_event(0x3, 0x3, true);
	KUNIT_EXPECT_EQ(test, event, TB_CFG_REQUEST_EVENT_LOCAL_FAILED);
	event = tb_test_xdomain_intermediate_event(0x3, 0x1, false);
	KUNIT_EXPECT_EQ(test, event, TB_CFG_REQUEST_EVENT_NONE);
	event = tb_test_xdomain_intermediate_event(0x1, 0x3, true);
	KUNIT_EXPECT_EQ(test, event, TB_CFG_REQUEST_EVENT_NONE);
}

static void tb_test_ctl_timeout_route_decode(struct kunit *test)
{
	struct tb_xdomain_header xdomain = {
		.route_hi = 0x12,
		.route_lo = 0x3456789a,
	};
	struct tb_cfg_header config = tb_cfg_make_header(0x123456789aULL);

	KUNIT_EXPECT_EQ(test,
			tb_cfg_request_route(TB_CFG_PKG_XDOMAIN_REQ, &xdomain,
					     sizeof(xdomain)),
			0x123456789aULL);
	KUNIT_EXPECT_EQ(test,
			tb_cfg_request_route(TB_CFG_PKG_XDOMAIN_RESP, &xdomain,
					     sizeof(xdomain)),
			0x123456789aULL);
	KUNIT_EXPECT_EQ(test,
			tb_cfg_request_route(TB_CFG_PKG_READ, &config,
					     sizeof(config)),
			0x123456789aULL);
	KUNIT_EXPECT_EQ(test,
			tb_cfg_request_route(TB_CFG_PKG_XDOMAIN_REQ, &xdomain,
					     sizeof(xdomain) - 1),
			0ULL);
}

static void tb_test_ctl_split_state_matches_independent_model(struct kunit *test)
{
	struct ctl_transaction_model model = {
		.submit = CTL_SUBMIT_MODEL_WAITING,
		.peer = CTL_PEER_MODEL_WAITING,
	};
	struct tb_cfg_request_state actual = {
		.local = TB_CFG_LOCAL_WAITING,
		.peer = TB_CFG_PEER_WAITING,
	};
	enum ctl_transaction_model_action model_action;
	enum tb_cfg_request_action actual_action;

	model_action = ctl_model_transaction_step(&model, CTL_TRANSACTION_MODEL_LOCAL_ACCEPTED);
	actual_action = tb_cfg_request_state_step(&actual, TB_CFG_REQUEST_EVENT_LOCAL_ACCEPTED);
	KUNIT_ASSERT_EQ(test, model.submit, CTL_SUBMIT_MODEL_ACCEPTED);
	KUNIT_ASSERT_EQ(test, model.peer, CTL_PEER_MODEL_WAITING);
	KUNIT_ASSERT_EQ(test, model_action, CTL_TRANSACTION_MODEL_NONE);
	KUNIT_ASSERT_EQ(test, actual.local, TB_CFG_LOCAL_ACCEPTED);
	KUNIT_ASSERT_EQ(test, actual.peer, TB_CFG_PEER_WAITING);
	KUNIT_ASSERT_EQ(test, actual_action, TB_CFG_REQUEST_ACTION_NONE);

	model_action = ctl_model_transaction_step(&model, CTL_TRANSACTION_MODEL_PEER_MATCHED);
	actual_action = tb_cfg_request_state_step(&actual, TB_CFG_REQUEST_EVENT_PEER_MATCHED);
	KUNIT_EXPECT_EQ(test, model.submit, CTL_SUBMIT_MODEL_ACCEPTED);
	KUNIT_EXPECT_EQ(test, model.peer, CTL_PEER_MODEL_MATCHED);
	KUNIT_EXPECT_EQ(test, model_action, CTL_TRANSACTION_MODEL_COMPLETE);
	KUNIT_EXPECT_EQ(test, actual.local, TB_CFG_LOCAL_ACCEPTED);
	KUNIT_EXPECT_EQ(test, actual.peer, TB_CFG_PEER_MATCHED);
	KUNIT_EXPECT_EQ(test, actual_action, TB_CFG_REQUEST_ACTION_COMPLETE);
}

static void tb_test_ctl_liveness_matched_reply_clears(struct kunit *test)
{
	int n = 0;
	int i;

	for (i = 0; i < 5; i++)
		n = tb_ctl_liveness_next(n, TB_CTL_EVENT_TIMEOUT);
	KUNIT_EXPECT_EQ(test, n, 5);
	KUNIT_EXPECT_TRUE(test, tb_ctl_timeouts_indicate_dead(n));

	/* One reply we actually asked for and the channel is live again. */
	n = tb_ctl_liveness_next(n, TB_CTL_EVENT_REPLY_MATCHED);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_FALSE(test, tb_ctl_timeouts_indicate_dead(n));
}

static void tb_test_ctl_liveness_unmatched_reply_does_not_clear(struct kunit *test)
{
	int n = 0;
	int i;

	for (i = 0; i < TB_CTL_DEAD_TIMEOUTS; i++)
		n = tb_ctl_liveness_next(n, TB_CTL_EVENT_TIMEOUT);
	KUNIT_EXPECT_TRUE(test, tb_ctl_timeouts_indicate_dead(n));

	/*
	 * Ring 0 still moves packets, but they pair with nothing. That is the
	 * degraded state, not recovery: the verdict must NOT be cleared, or
	 * teardown resumes writing into a controller that is already going.
	 */
	for (i = 0; i < 20; i++)
		n = tb_ctl_liveness_next(n, TB_CTL_EVENT_REPLY_UNMATCHED);
	KUNIT_EXPECT_EQ(test, n, TB_CTL_DEAD_TIMEOUTS);
	KUNIT_EXPECT_TRUE(test, tb_ctl_timeouts_indicate_dead(n));
}

static void tb_test_ctl_liveness_appmana_019_sequence(struct kunit *test)
{
	int n = 0;
	int i;

	/* 16:26 -- software CM healthy, matched replies flowing. */
	for (i = 0; i < 900; i++)
		n = tb_ctl_liveness_next(n, TB_CTL_EVENT_REPLY_MATCHED);
	KUNIT_EXPECT_FALSE(test, tb_ctl_timeouts_indicate_dead(n));

	/*
	 * 17:21..17:30 -- rx_matched frozen, rx_unmatched climbing, every
	 * request timing out. This is where the old code would still have run
	 * a full config space teardown on rmmod.
	 */
	for (i = 0; i < 112; i++) {
		n = tb_ctl_liveness_next(n, TB_CTL_EVENT_TIMEOUT);
		n = tb_ctl_liveness_next(n, TB_CTL_EVENT_REPLY_UNMATCHED);
	}
	KUNIT_EXPECT_TRUE(test, tb_ctl_timeouts_indicate_dead(n));
}

static void tb_test_icm_phy_reset_covers_single_lane_links(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, tb_icm_phy_port_needs_reset(false, false));
	KUNIT_EXPECT_TRUE(test, tb_icm_phy_port_needs_reset(true, false));
	KUNIT_EXPECT_TRUE(test, tb_icm_phy_port_needs_reset(false, true));
	KUNIT_EXPECT_TRUE(test, tb_icm_phy_port_needs_reset(true, true));
}

static struct kunit_case tb_test_cases[] = {
	KUNIT_CASE(tb_test_ring_descriptor_is_one_complete_word),
	KUNIT_CASE(tb_test_ring_work_uses_unbound_queue),
	KUNIT_CASE(tb_test_xdomain_gen4_single_lane_negotiates_bonding),
	KUNIT_CASE(tb_test_xdomain_icm_still_initializes_link),
	KUNIT_CASE(tb_test_xdomain_icm_single_lane_enters_bonding_state),
	KUNIT_CASE(tb_test_xdomain_icm_unsupported_status_uses_early_direct_bonding),
	KUNIT_CASE(tb_test_xdomain_one_sided_icm_announcement_never_arms_bonding),
	KUNIT_CASE(tb_test_xdomain_two_port_bonding_does_not_serialize_peers),
	KUNIT_CASE(tb_test_xdomain_never_bonds_after_services_publish),
	KUNIT_CASE(tb_test_xdomain_bond_abort_preserves_single_lane),
	KUNIT_CASE(tb_test_xdomain_bonding_rearm_recovers_boot_skew),
	KUNIT_CASE(tb_test_xdomain_bonding_rearm_bounded_and_safe),
	KUNIT_CASE(tb_test_xdomain_bonding_rearm_predicates),
	KUNIT_CASE(tb_test_xdomain_error_match_is_route_checked),
	KUNIT_CASE(tb_test_xdomain_icm_teardown_never_touches_lane_hardware),
	KUNIT_CASE(tb_test_xdomain_handler_abi_stock_offsets),
	KUNIT_CASE(tb_test_xdomain_handler_dispatch_source_blind_registrant),
	KUNIT_CASE(tb_test_xdomain_handler_unregister_waits_for_dispatch),
	KUNIT_CASE(tb_test_xdomain_properties_stale),
	KUNIT_CASE(tb_test_xdomain_reboot_stranding),
	KUNIT_CASE(tb_test_xdomain_coreset_strand),
	KUNIT_CASE(tb_test_xdomain_late_second_link),
	KUNIT_CASE(tb_test_xdomain_stale_identity_recovery),
	KUNIT_CASE(tb_test_xdomain_silent_stale_identity_recovery),
	KUNIT_CASE(tb_test_xdomain_late_peer_powered_on_after_boot),
	KUNIT_CASE(tb_test_xdomain_placeholder_uuid_never_final_identity),
	KUNIT_CASE(tb_test_xdomain_responsive_peer_registers_promptly),
	KUNIT_CASE(tb_test_xdomain_event_uuid_requires_peer_verification),
	KUNIT_CASE(tb_test_xdomain_properties_response_proves_both_identities),
	KUNIT_CASE(tb_test_xdomain_unverified_uuid_cannot_alias_route),
	KUNIT_CASE(tb_test_xdomain_announce_survives_absent_peer),
	KUNIT_CASE(tb_test_xdomain_peer_never_answers_leaves_no_latch),
	KUNIT_CASE(tb_test_xdomain_peer_answers_after_many_attempts),
	KUNIT_CASE(tb_test_xdomain_announce_rearms_mid_backoff),
	KUNIT_CASE(tb_test_xdomain_announce_stops_deterministically),
	KUNIT_CASE(tb_test_xdomain_placeholder_uuid_predicate),
	KUNIT_CASE(tb_test_xdomain_lookup_without_root_switch),
	KUNIT_CASE(tb_test_cm_reconcile_lost_unplug),
	KUNIT_CASE(tb_test_cm_reconcile_lost_plug),
	KUNIT_CASE(tb_test_cm_reconcile_perturbed_lane_keeps_live_link),
	KUNIT_CASE(tb_test_cm_reconcile_no_kick_under_resident_icm),
	KUNIT_CASE(tb_test_cm_plug_retrain_half_trained_resident_icm),
	KUNIT_CASE(tb_test_cm_plug_retrain_latched_port_resident_icm),
	KUNIT_CASE(tb_test_cm_plug_retrain_bounded_dead_hardware),
	KUNIT_CASE(tb_test_cm_plug_retrain_post_unplug_cooldown),
	KUNIT_CASE(tb_test_cm_lane1_bonding_recovery_resident_icm),
	KUNIT_CASE(tb_test_cm_lane1_retrain_bounded_dead_lane),
	KUNIT_CASE(tb_test_cm_connector_replug_recovers_full_train_only_lane),
	KUNIT_CASE(tb_test_cm_connector_replug_bounded_when_lane1_dead),
	KUNIT_CASE(tb_test_cm_connector_replug_defers_to_fresh_icm_event),
	KUNIT_CASE(tb_test_cm_connector_replug_leaves_single_lane_cable),
	KUNIT_CASE(tb_test_icm_warm_restart_loses_auth),
	KUNIT_CASE(tb_test_icm_wedged_running),
	KUNIT_CASE(tb_test_cm_select_forced_software),
	KUNIT_CASE(tb_test_cm_forced_takeover_unlocks_config),
	KUNIT_CASE(tb_test_icm_partial_wedge_refuses_software_takeover),
	KUNIT_CASE(tb_test_icm_startup_proofs_are_separate),
	KUNIT_CASE(tb_test_icm_root_config_probe_preserves_sequence_retries),
	KUNIT_CASE(tb_test_icm_root_recovery_is_separate_and_bounded),
	KUNIT_CASE(tb_test_nhi_recovery_is_exact_and_one_shot),
	KUNIT_CASE(tb_test_nhi_recovery_command_failure_is_terminal),
	KUNIT_CASE(tb_test_root_power_cycle_timeout_requires_consumption),
	KUNIT_CASE(tb_test_cfg_tx_consumption_is_independent_and_monotonic),
	KUNIT_CASE(tb_test_tx_index_progress_is_not_descriptor_completion),
	KUNIT_CASE(tb_test_nhi_startup_recovery_requires_power_cycle_dispatch),
	KUNIT_CASE(tb_test_maple_root_power_cycle_encoding),
	KUNIT_CASE(tb_test_pcie2cio_completion_states),
	KUNIT_CASE(tb_test_nhi_recovery_success_completes_episode),
	KUNIT_CASE(tb_test_nhi_recovery_is_limited_to_proven_hardware),
	KUNIT_CASE(tb_test_nhi_recovery_does_not_claim_reclaim_safety),
	KUNIT_CASE(tb_test_nhi_runtime_recovery_is_a_separate_machine),
	KUNIT_CASE(tb_test_nhi_runtime_recovery_is_bounded),
	KUNIT_CASE(tb_test_nhi_runtime_recovery_requires_data_proof),
	KUNIT_CASE(tb_test_nhi_runtime_recovery_failures_are_terminal),
	KUNIT_CASE(tb_test_nhi_dma_misc_policy_is_mutually_exclusive),
	KUNIT_CASE(tb_test_maple_ridge_disables_interrupt_clear_on_read),
	KUNIT_CASE(tb_test_nhi_tx_stall_requires_hardware_consumer_proof),
	KUNIT_CASE(tb_test_icm_warm_restart_is_unsupported),
	KUNIT_CASE(tb_test_domain_add_failure_no_deadlock),
	KUNIT_CASE(tb_test_domain_remove_no_deadlock),
	KUNIT_CASE(tb_test_domain_remove_no_work_after_destroy),
	KUNIT_CASE(tb_test_icm_takeover_teardown_no_deadlock),
	KUNIT_CASE(tb_test_icm_healthy_keeps_firmware_cm),
	KUNIT_CASE(tb_test_path_basic),
	KUNIT_CASE(tb_test_path_not_connected_walk),
	KUNIT_CASE(tb_test_path_single_hop_walk),
	KUNIT_CASE(tb_test_path_daisy_chain_walk),
	KUNIT_CASE(tb_test_path_simple_tree_walk),
	KUNIT_CASE(tb_test_path_complex_tree_walk),
	KUNIT_CASE(tb_test_path_max_length_walk),
	KUNIT_CASE(tb_test_path_not_connected),
	KUNIT_CASE(tb_test_path_not_bonded_lane0),
	KUNIT_CASE(tb_test_path_not_bonded_lane1),
	KUNIT_CASE(tb_test_path_not_bonded_lane1_chain),
	KUNIT_CASE(tb_test_path_not_bonded_lane1_chain_reverse),
	KUNIT_CASE(tb_test_path_mixed_chain),
	KUNIT_CASE(tb_test_path_mixed_chain_reverse),
	KUNIT_CASE(tb_test_tunnel_pcie),
	KUNIT_CASE(tb_test_tunnel_dp),
	KUNIT_CASE(tb_test_tunnel_dp_chain),
	KUNIT_CASE(tb_test_tunnel_dp_tree),
	KUNIT_CASE(tb_test_tunnel_dp_max_length),
	KUNIT_CASE(tb_test_tunnel_3dp),
	KUNIT_CASE(tb_test_tunnel_port_on_path),
	KUNIT_CASE(tb_test_tunnel_usb3),
	KUNIT_CASE(tb_test_tunnel_dma),
	KUNIT_CASE(tb_test_tunnel_dma_rx),
	KUNIT_CASE(tb_test_tunnel_dma_tx),
	KUNIT_CASE(tb_test_tunnel_dma_chain),
	KUNIT_CASE(tb_test_tunnel_dma_match),
	KUNIT_CASE(tb_test_credit_alloc_legacy_not_bonded),
	KUNIT_CASE(tb_test_credit_alloc_legacy_bonded),
	KUNIT_CASE(tb_test_credit_alloc_pcie),
	KUNIT_CASE(tb_test_credit_alloc_without_dp),
	KUNIT_CASE(tb_test_credit_alloc_dp),
	KUNIT_CASE(tb_test_credit_alloc_usb3),
	KUNIT_CASE(tb_test_credit_alloc_dma),
	KUNIT_CASE(tb_test_credit_alloc_dma_multiple),
	KUNIT_CASE(tb_test_credit_alloc_all),
	KUNIT_CASE(tb_test_property_parse),
	KUNIT_CASE(tb_test_property_format),
	KUNIT_CASE(tb_test_property_copy),
	KUNIT_CASE(tb_test_cm_plug_synth_scan_fails_keeps_retrying),
	KUNIT_CASE(tb_test_cm_plug_synth_recovers_when_router_answers),
	KUNIT_CASE(tb_test_xdomain_response_match_follows_protocol_model),
	KUNIT_CASE(tb_test_xdomain_native_error_fails_request),
	KUNIT_CASE(tb_test_ctl_liveness_threshold),
	KUNIT_CASE(tb_test_ctl_xdomain_tx_status_classification),
	KUNIT_CASE(tb_test_ctl_split_state_records_local_acceptance),
	KUNIT_CASE(tb_test_ctl_split_state_preserves_accept_on_peer_timeout),
	KUNIT_CASE(tb_test_ctl_peer_timeout_keeps_local_command_waiting),
	KUNIT_CASE(tb_test_ctl_local_status_slot_blocks_reuse),
	KUNIT_CASE(tb_test_ctl_split_state_terminal_transitions),
	KUNIT_CASE(tb_test_ctl_split_state_peer_response_is_independent),
	KUNIT_CASE(tb_test_ctl_xdomain_tx_status_route_correlation),
	KUNIT_CASE(tb_test_ctl_timeout_route_decode),
	KUNIT_CASE(tb_test_ctl_split_state_matches_independent_model),
	KUNIT_CASE(tb_test_ctl_liveness_matched_reply_clears),
	KUNIT_CASE(tb_test_ctl_liveness_unmatched_reply_does_not_clear),
	KUNIT_CASE(tb_test_ctl_liveness_appmana_019_sequence),
	KUNIT_CASE(tb_test_icm_phy_reset_covers_single_lane_links),
	{ }
};

static struct kunit_suite tb_test_suite = {
	.name = "thunderbolt",
	.test_cases = tb_test_cases,
};

kunit_test_suite(tb_test_suite);

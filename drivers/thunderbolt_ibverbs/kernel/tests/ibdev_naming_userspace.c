// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror of kernel/tests/ibdev_naming_test.c (KUnit).
 *
 * Drives tbv_ibdev_rail_name_index() -- the CALLER that reads fields off a
 * struct tbv_rail -- because the defect was there: it chose which rail-key
 * field to key the ib_device name on. Two mock rails model appmana-022's
 * neighbours (021 and 023): identical domain / native_lane / local_adapter,
 * differing ONLY in key.route (downstream port 0x3 vs 0x1). The contract is
 * "two neighbours -> two names"; this test is red while the caller keys on
 * local_adapter (both 0 -> collide -> the 2nd ib_register_device() is -ENFILE)
 * and green once it keys on the route. The test body never changes between the
 * two -- only the code under test does.
 *
 * Keep in lockstep with the KUnit. The fleet kernels lack CONFIG_KUNIT.
 *
 * build+run: cc -O2 -Wall -o /tmp/naming_test ibdev_naming_userspace.c && /tmp/naming_test
 * (define BUGGY_CALLER to compile the pre-fix local_adapter caller and watch it fail)
 */
#include <stdio.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint64_t u64;

#define TBV_NATIVE_MAX_LANES   4
#define TBV_NAME_MAX_PORTS     64
#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef ERANGE
#define ERANGE 34
#endif

/* --- minimal struct shapes (only the fields the caller touches) --- */
enum tbv_backend_type { TBV_BACKEND_APPLE, TBV_BACKEND_NATIVE };
struct tb { int index; };
struct tb_xdomain { struct tb *tb; };
struct tbv_rail_key { u64 route; u32 local_adapter; };
struct tbv_peer { enum tbv_backend_type backend; struct tb_xdomain *xd; };
struct tbv_rail { struct tbv_peer *peer; struct tbv_rail_key key; u32 native_lane; };

/* --- MIRROR of tbv_ibdev_name_index() in kernel/ibdev.c (verbatim) --- */
static int tbv_ibdev_name_index(int domain_idx, u32 route_port,
				u32 native_lane, int apple,
				unsigned int max_lanes)
{
	unsigned int slot;

	if (domain_idx < 0)
		return -ENODEV;
	if (route_port >= TBV_NAME_MAX_PORTS)
		return -ERANGE;
	if (apple) {
		slot = max_lanes;
	} else {
		if (native_lane >= max_lanes)
			return -ERANGE;
		slot = native_lane;
	}
	return ((unsigned int)domain_idx * TBV_NAME_MAX_PORTS + route_port)
		* (max_lanes + 1) + slot;
}

/* --- MIRROR of tbv_ibdev_rail_name_index() in kernel/ibdev.c --- */
static int tbv_ibdev_rail_name_index(const struct tbv_rail *rail)
{
	u32 disc;

	if (!rail || !rail->peer || !rail->peer->xd || !rail->peer->xd->tb)
		return -ENODEV;
#ifdef BUGGY_CALLER
	disc = rail->key.local_adapter;            /* the bug: constant 0 */
#else
	disc = (u32)(rail->key.route & 0xff);      /* the fix: downstream port */
#endif
	return tbv_ibdev_name_index(rail->peer->xd->tb->index, disc,
				    rail->native_lane,
				    rail->peer->backend != TBV_BACKEND_NATIVE,
				    TBV_NATIVE_MAX_LANES);
}

static int failures;
#define CHECK(c, m) do { \
	printf("  [%s] %s\n", (c) ? "ok" : "FAIL", m); \
	if (!(c)) failures++; \
} while (0)

int main(void)
{
	struct tb dom = { .index = 0 };
	struct tb_xdomain xd = { .tb = &dom };
	struct tbv_peer p021 = { .backend = TBV_BACKEND_NATIVE, .xd = &xd };
	struct tbv_peer p023 = { .backend = TBV_BACKEND_NATIVE, .xd = &xd };
	/* identical domain/lane/local_adapter; only route differs */
	struct tbv_rail to_021 = { .peer = &p021, .key = { .route = 0x3, .local_adapter = 0 }, .native_lane = 0 };
	struct tbv_rail to_023 = { .peer = &p023, .key = { .route = 0x1, .local_adapter = 0 }, .native_lane = 0 };
	int a = tbv_ibdev_rail_name_index(&to_021);
	int b = tbv_ibdev_rail_name_index(&to_023);

	printf("tbv_ibdev_rail_name_index on the real appmana-022 rails:\n");
	printf("  rail->021 (route 0x3) -> usb4_rdma%d\n", a);
	printf("  rail->023 (route 0x1) -> usb4_rdma%d\n", b);
	CHECK(a >= 0 && b >= 0, "both rails name successfully");
	CHECK(a != b, "two neighbours on one node get DIFFERENT usb4_rdma names");

	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}

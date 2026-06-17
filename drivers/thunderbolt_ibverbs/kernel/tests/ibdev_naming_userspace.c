// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror of kernel/tests/ibdev_naming_test.c (KUnit).
 *
 * Models the REAL per-rail naming inputs seen on a mid-chain node, captured
 * live on appmana-022 (cabled to neighbours 021 and 023). Both native rails
 * report the SAME (domain 0, native_lane 0, local_adapter 0); the ONLY field
 * that differs between them is the XDomain ROUTE (downstream port): 0x3 vs 0x1.
 * Observed dmesg:
 *   registered native ib_device usb4_rdma0 ... route=0x3
 *   failed to register per-rail ib_device usb4_rdma0: -23 ... route=0x1
 *
 * tbv_ibdev_name_index() keys the name on (domain, route downstream-port, lane).
 * The two rails share domain/lane/local_adapter and differ only in route, so
 * keying on the route's low byte (the downstream port) gives them distinct
 * names. This mirror feeds the function the REAL inputs and asserts the
 * contract (two rails -> two names).
 *
 * build+run: cc -O2 -Wall -o /tmp/naming_test ibdev_naming_userspace.c && /tmp/naming_test
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

/*
 * A per-rail naming input exactly as tbv_ibdev_rail_name_index() reads it off a
 * struct tbv_rail: the tb domain index, the rail key's route (whose low byte is
 * the downstream port), and the native lane. The two rails below are the
 * literal values from appmana-022.
 */
struct rail_id { int domain; u32 local_adapter; u64 route; u32 native_lane; int apple; };

/* what the driver computes: the route's low byte is the downstream port */
static int name_index_of(const struct rail_id *r)
{
	return tbv_ibdev_name_index(r->domain, (u32)(r->route & 0xff),
				    r->native_lane, r->apple,
				    TBV_NATIVE_MAX_LANES);
}

static int failures;
#define CHECK(c, m) do { \
	printf("  [%s] %s\n", (c) ? "ok" : "FAIL", m); \
	if (!(c)) failures++; \
} while (0)

int main(void)
{
	/* appmana-022's two native rails: differ ONLY in route */
	struct rail_id to_021 = { .domain = 0, .local_adapter = 0, .route = 0x3, .native_lane = 0 };
	struct rail_id to_023 = { .domain = 0, .local_adapter = 0, .route = 0x1, .native_lane = 0 };
	int a, b;

	printf("tbv_ibdev_name_index multi-rail naming (real appmana-022 rails):\n");

	a = name_index_of(&to_021);
	b = name_index_of(&to_023);
	printf("  rail->021 (route 0x3) -> usb4_rdma%d\n", a);
	printf("  rail->023 (route 0x1) -> usb4_rdma%d\n", b);

	CHECK(a >= 0 && b >= 0, "both rails name successfully");
	/* THE BUG: same (domain, local_adapter, lane), different route -> the
	 * names MUST differ, or the second ib_register_device() is -ENFILE. */
	CHECK(a != b, "two neighbours on one node get DIFFERENT usb4_rdma names");

	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}

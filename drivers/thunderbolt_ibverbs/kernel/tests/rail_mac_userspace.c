// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror of kernel/tests/rail_mac_test.c (KUnit). Fleet kernels lack
 * CONFIG_KUNIT; this duplicates the pure function verbatim and runs the same
 * cases with `cc`. Change tbv_rail_netdev_mac() in native_control.c, this
 * mirror, and the kunit suite together.
 *
 * build+run: cc -O2 -Wall -o /tmp/rail_mac_test rail_mac_userspace.c && /tmp/rail_mac_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint64_t u64;

/* --- MIRROR of tbv_rail_netdev_mac() in kernel/native_control.c --- */
static void tbv_rail_netdev_mac(u64 node_guid, u8 mac[6])
{
	mac[0] = 0x02;
	mac[1] = (u8)(node_guid >> 32);
	mac[2] = (u8)(node_guid >> 24);
	mac[3] = (u8)(node_guid >> 16);
	mac[4] = (u8)(node_guid >> 8);
	mac[5] = (u8)node_guid;
}

#define GUID(peer, rail) (0x0200544256524253ULL + ((u64)(peer) << 24) + (rail))

static int failures;
#define CHECK(cond, msg) do { \
	if (cond) printf("  [ok] %s\n", (msg)); \
	else { printf("  [FAIL] %s\n", (msg)); failures++; } \
} while (0)

int main(void)
{
	u8 p1r0[6], p2r0[6], p1r1[6], again[6];

	tbv_rail_netdev_mac(GUID(1, 0), p1r0);
	tbv_rail_netdev_mac(GUID(2, 0), p2r0);
	tbv_rail_netdev_mac(GUID(1, 1), p1r1);
	tbv_rail_netdev_mac(GUID(1, 0), again);

	CHECK(p1r0[0] == 0x02, "mac[0] == 0x02 (locally administered)");
	CHECK((p1r0[0] & 0x01) == 0, "mac is unicast (multicast bit clear)");
	CHECK(memcmp(p1r0, again, 6) == 0, "deterministic: same guid -> same mac");
	CHECK(memcmp(p1r0, p2r0, 6) != 0, "distinct: peer1 != peer2");
	CHECK(memcmp(p1r0, p1r1, 6) != 0, "distinct: rail0 != rail1");

	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}

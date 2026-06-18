// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror of kernel/tests/link_gid_test.c (KUnit).
 *
 * The fleet's Ubuntu generic kernels do not set CONFIG_KUNIT, so the kunit suite
 * cannot run on the nodes. This harness duplicates the PURE function under test
 * verbatim and runs the same cases, so the contract can be verified on any host
 * with `cc`. If you change tbv_link_gid_prefix() in kernel/native_control.c,
 * update BOTH this mirror and the kunit suite.
 *
 * Given the Thunderbolt unique_id UUIDs of the two hosts on a link, derive the
 * GID /64 subnet prefix such that:
 *   - symmetric: both ends (which see (local,remote) in opposite order) derive
 *     the SAME /64, so they land on one subnet -> match by GID subnet;
 *   - distinct per link: a node's two cabled links get two different /64s, so a
 *     dest GID resolves to exactly one rail;
 *   - ULA: fd00::/8 (RFC 4193), so it never collides with global/LAN prefixes.
 * The interface-id (low 8 bytes of the GID) is the per-rail node_guid, set
 * elsewhere; this helper only owns the /64.
 *
 * build+run: cc -O2 -Wall -o /tmp/link_gid_test link_gid_userspace.c && /tmp/link_gid_test
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t u8;
typedef uint64_t u64;

/* --- MIRROR of tbv_link_gid_prefix() in kernel/native_control.c --- */
/*
 * FNV-1a 64-bit: a tiny, portable, deterministic hash so the kernel and this
 * userspace mirror compute byte-identical results (jhash would force us to
 * duplicate the kernel's jhash internals here).
 */
static u64 tbv_link_gid_fnv1a(const u8 *data, size_t len)
{
	u64 h = 0xcbf29ce484222325ULL;
	size_t i;

	for (i = 0; i < len; i++) {
		h ^= data[i];
		h *= 0x00000100000001b3ULL;
	}
	return h;
}

static void tbv_link_gid_prefix(const u8 a[16], const u8 b[16], u8 prefix[8])
{
	const u8 *lo = a, *hi = b;
	u8 buf[32];
	u64 h;

	/* symmetric: order the pair so both ends hash the same bytes */
	if (memcmp(a, b, 16) > 0) {
		lo = b;
		hi = a;
	}
	memcpy(buf, lo, 16);
	memcpy(buf + 16, hi, 16);
	h = tbv_link_gid_fnv1a(buf, sizeof(buf));

	/* RFC 4193 ULA /64: 0xfd | 40-bit global-id (per-link hash) | 16-bit subnet=0 */
	memset(prefix, 0, 8);
	prefix[0] = 0xfd;
	prefix[1] = (u8)(h >> 32);
	prefix[2] = (u8)(h >> 24);
	prefix[3] = (u8)(h >> 16);
	prefix[4] = (u8)(h >> 8);
	prefix[5] = (u8)h;
	/* prefix[6..7] = subnet id 0 */
}

/* --- test harness --- */
static int failures;
#define CHECK(cond, msg) do { \
	if (cond) { printf("  [ok] %s\n", (msg)); } \
	else { printf("  [FAIL] %s\n", (msg)); failures++; } \
} while (0)

/* distinct 16-byte UUIDs standing in for TB unique_ids */
static const u8 UA[16] = { 0xc1,0x01,0,0,0,0x80,0x84,0x1e,0x03,0xb7,0xb9,0x1c,0xd8,0x23,0x20,0x23 };
static const u8 UB[16] = { 0x5a,0x11,0x22,0xb7,0x77,0x15,0,0,0,0,0,0,0,0,0,0x01 };
static const u8 UC[16] = { 0x5a,0x11,0x22,0xb7,0x75,0xb1,0,0,0,0,0,0,0,0,0,0x02 };

int main(void)
{
	u8 ab[8], ba[8], ac[8], bc[8];

	tbv_link_gid_prefix(UA, UB, ab);
	tbv_link_gid_prefix(UB, UA, ba);
	tbv_link_gid_prefix(UA, UC, ac);
	tbv_link_gid_prefix(UB, UC, bc);

	/* symmetric: both ends of the A<->B link derive the same /64 */
	CHECK(memcmp(ab, ba, 8) == 0, "symmetric: prefix(A,B) == prefix(B,A)");

	/* ULA marker */
	CHECK(ab[0] == 0xfd, "A<->B prefix is ULA (fd00::/8)");
	CHECK(ac[0] == 0xfd, "A<->C prefix is ULA (fd00::/8)");

	/* distinct per link: node A's two links (to B and to C) differ */
	CHECK(memcmp(ab, ac, 8) != 0, "distinct: A<->B != A<->C (per-link subnets)");
	/* and three distinct links are mutually distinct */
	CHECK(memcmp(ab, bc, 8) != 0, "distinct: A<->B != B<->C");
	CHECK(memcmp(ac, bc, 8) != 0, "distinct: A<->C != B<->C");

	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror of kernel/tests/ibdev_naming_test.c (KUnit).
 *
 * The fleet's Ubuntu generic kernels do not set CONFIG_KUNIT, so the kunit
 * suite cannot run on the nodes. This harness duplicates the PURE function
 * under test verbatim and runs the exact same cases, so the contract can be
 * verified on any host with `cc`. If you change tbv_ibdev_name_index() in
 * kernel/ibdev.c, update BOTH this mirror and the kunit suite.
 *
 * Contract (the bug it pins): a host cabled to two neighbours reaches both
 * through the SAME tb domain (one controller -> one tb->index) on the SAME
 * native lane 0, but over DIFFERENT downstream adapters
 * (rail->key.local_adapter). The per-rail ib_device name index MUST therefore
 * differ between them, or the second ib_register_device("usb4_rdmaN") returns
 * -ENFILE (duplicate name) and the second neighbour's rail never comes up
 * (observed on appmana-018: rail facing 002 and rail facing 027 both -> idx 0).
 *
 * build+run: cc -O2 -Wall -o /tmp/naming_test ibdev_naming_userspace.c && /tmp/naming_test
 */
#include <stdio.h>
#include <stdint.h>

typedef uint32_t u32;

#define TBV_NATIVE_MAX_LANES   4
#define TBV_NAME_MAX_ADAPTERS  64
#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef ERANGE
#define ERANGE 34
#endif

/* --- MIRROR of tbv_ibdev_name_index() in kernel/ibdev.c --- */
static int tbv_ibdev_name_index(int domain_idx, u32 local_adapter,
				u32 native_lane, int apple,
				unsigned int max_lanes)
{
	unsigned int slot;

	if (domain_idx < 0)
		return -ENODEV;
	if (local_adapter >= TBV_NAME_MAX_ADAPTERS)
		return -ERANGE;
	if (apple) {
		slot = max_lanes;
	} else {
		if (native_lane >= max_lanes)
			return -ERANGE;
		slot = native_lane;
	}
	return ((unsigned int)domain_idx * TBV_NAME_MAX_ADAPTERS + local_adapter)
		* (max_lanes + 1) + slot;
}

static int failures;
#define CHECK(c, m) do { \
	printf("  [%s] %s\n", (c) ? "ok" : "FAIL", m); \
	if (!(c)) failures++; \
} while (0)

#define L TBV_NATIVE_MAX_LANES

int main(void)
{
	int a, b;

	printf("tbv_ibdev_name_index multi-rail naming:\n");

	/* THE BUG: two neighbours, same domain + lane, different local adapter
	 * -> distinct names, never a duplicate (-ENFILE). */
	a = tbv_ibdev_name_index(0, 1, 0, 0, L);  /* rail facing peer on adapter 1 */
	b = tbv_ibdev_name_index(0, 3, 0, 0, L);  /* rail facing peer on adapter 3 */
	CHECK(a >= 0 && b >= 0, "both adapters name successfully");
	CHECK(a != b, "two peers on one domain+lane get DIFFERENT names");

	/* determinism: a given physical rail always maps to the same index */
	CHECK(tbv_ibdev_name_index(0, 1, 0, 0, L) == a, "same rail -> same index");

	/* bonded lanes to one peer (same adapter) still disambiguate by lane */
	CHECK(tbv_ibdev_name_index(0, 1, 0, 0, L) !=
	      tbv_ibdev_name_index(0, 1, 1, 0, L), "lanes 0 and 1 differ");

	/* apple rail takes the per-(domain,adapter) slot above the native lanes */
	CHECK(tbv_ibdev_name_index(0, 1, 0, 1, L) !=
	      tbv_ibdev_name_index(0, 1, 0, 0, L), "apple slot != native lane 0");
	CHECK(tbv_ibdev_name_index(0, 2, 0, 1, L) !=
	      tbv_ibdev_name_index(0, 1, 0, 1, L), "apple slots differ by adapter");

	/* distinct domains never collide either */
	CHECK(tbv_ibdev_name_index(1, 1, 0, 0, L) !=
	      tbv_ibdev_name_index(0, 1, 0, 0, L), "different domains differ");

	/* error handling */
	CHECK(tbv_ibdev_name_index(-1, 0, 0, 0, L) == -ENODEV, "negative domain -> -ENODEV");
	CHECK(tbv_ibdev_name_index(0, 0, L, 0, L) == -ERANGE, "native lane >= max -> -ERANGE");
	CHECK(tbv_ibdev_name_index(0, TBV_NAME_MAX_ADAPTERS, 0, 0, L) == -ERANGE,
	      "adapter out of range -> -ERANGE");

	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}

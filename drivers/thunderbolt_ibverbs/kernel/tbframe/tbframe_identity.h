/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Per-link ULA EUI-64 for GID derivation (spec §8), the legacy per-rail
 * identity math. One place for it so the KUnit self-loop model asserts
 * against exactly what the service layer ships:
 * a 24-bit host hash folded from the host router UUID (stable across
 * boots, unique per host) plus a 16-bit link hash over the remote UUID
 * and route, formatted as the modified EUI-64 of the synthetic
 * locally-administered MAC 02:H1:H2:H3:L1:L2.
 *
 * The route term in the link hash is what keeps an intra-domain loop
 * (both ports of one host cabled together, local UUID == remote UUID)
 * sound: the two ends of the cable share every UUID but never a route,
 * so each link still derives a distinct EUI-64.
 */
#ifndef TBFRAME_IDENTITY_H
#define TBFRAME_IDENTITY_H

#include <linux/types.h>

static inline u32 tbframe_host_identity_hash(const u8 uuid[16])
{
	u32 h = 0;
	int i;

	if (!uuid)
		return 0x544246; /* "TBF" */
	for (i = 0; i < 16; i++)
		h = h * 31 + uuid[i];
	h &= 0xffffffu;
	return h ? h : 0x544246;
}

static inline u16 tbframe_link_identity_hash(const u8 remote_uuid[16],
					     u64 route)
{
	u32 h = tbframe_host_identity_hash(remote_uuid);
	u16 folded;

	h ^= lower_32_bits(route);
	h ^= upper_32_bits(route);
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	folded = (u16)(h ^ (h >> 16));
	return folded ? folded : 1;
}

static inline u64 tbframe_identity_eui64(const u8 local_uuid[16],
					 const u8 remote_uuid[16], u64 route)
{
	u32 host = tbframe_host_identity_hash(local_uuid);
	u16 link = tbframe_link_identity_hash(remote_uuid, route);
	u8 mac[6] = {
		0x02, host >> 16, host >> 8, host, link >> 8, link,
	};

	/* RFC 4291 modified EUI-64: mac[0..2] ^ U/L, ff:fe, mac[3..5]. */
	return ((u64)(mac[0] ^ 0x02) << 56) | ((u64)mac[1] << 48) |
	       ((u64)mac[2] << 40) | (0xffULL << 32) | (0xfeULL << 24) |
	       ((u64)mac[3] << 16) | ((u64)mac[4] << 8) | (u64)mac[5];
}

#endif /* TBFRAME_IDENTITY_H */

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared fake-link helpers for the tbrxe KUnit suites: drive the tbframe
 * client upcalls directly (there is no tbframe module in the KUnit kernel).
 *
 * One shared local identity: the FIRST link_up publishes the ib_device and
 * pins the self GID for the whole KUnit kernel, so every suite must
 * advertise the same local_gid_eui64.
 */
#ifndef TBRXE_TEST_LINK_H
#define TBRXE_TEST_LINK_H

#include "../tbrxe_frame.h"

/* The identity this side "advertised in its own HELLO" (what a peer would
 * derive our GID from) and the peer's advertised identity.
 */
#define TBRXE_TEST_LOCAL_EUI64	0x005cdcfffe4360e9ull
#define TBRXE_TEST_PEER_EUI64	0x00a1b2fffec3d4e5ull

static inline void tbrxe_test_link_up(void *fake_link)
{
	struct tbframe_link_info info = {
		.gid_eui64	= TBRXE_TEST_PEER_EUI64,
		.local_gid_eui64 = TBRXE_TEST_LOCAL_EUI64,
		.rx_ring_entries = 2048,
		.data_window	= 1984,
		.max_payload	= TBFRAME_MAX_FRAME,
		.width		= 1,
		.speed		= 20,
	};

	tbrxe_frame_client_ops()->link_up(NULL, fake_link, &info);
}

static inline void tbrxe_test_link_down(void *fake_link)
{
	tbrxe_frame_client_ops()->link_down(NULL, fake_link,
					    TBFRAME_DOWN_CLOSED);
}

#endif /* TBRXE_TEST_LINK_H */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TBNET_TEST_H_
#define _TBNET_TEST_H_

bool tbnet_test_session_needs_teardown(bool handshake_complete,
				       bool session_active,
				       bool resources_owned);

#endif /* _TBNET_TEST_H_ */

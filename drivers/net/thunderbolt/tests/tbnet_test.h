/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TBNET_TEST_H_
#define _TBNET_TEST_H_

bool tbnet_test_session_needs_teardown(bool handshake_complete,
				       bool session_active,
				       bool resources_owned);

struct tbnet_test_handoff_result {
	int handoff_ret;
	unsigned int quarantine_requests;
	unsigned int quarantined;
	unsigned int buffers_freed;
	unsigned int rings_freed;
	unsigned int hopids_released;
	bool ring_pointers_cleared;
	bool leaf_ownership_cleared;
	int transmit_path;
	int transmit_ring;
	int receive_path;
	int receive_ring;
};

void tbnet_test_terminal_handoff(int quarantine_ret,
				 struct tbnet_test_handoff_result *result);

struct tbnet_test_disconnect_result {
	unsigned int teardown_calls;
	unsigned int retry_calls;
	unsigned int login_calls;
};

void tbnet_test_recovery_disconnect(int teardown_ret,
				    struct tbnet_test_disconnect_result *result);

#endif /* _TBNET_TEST_H_ */

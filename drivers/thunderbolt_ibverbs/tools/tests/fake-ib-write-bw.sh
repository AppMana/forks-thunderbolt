#!/usr/bin/env bash
set -eu

printf '%s\n' "$$" >> "${FAKE_IB_PID_LOG:?}"
printf 'fake ib_write_bw pid=%s args=%s\n' "$$" "$*"

if [[ "${FAKE_SSH_MODE:-pass}" == hang ]]; then
	sleep 300
elif [[ "$*" == *192.0.2.10* ]]; then
	sleep 0.2
else
	sleep 3
fi

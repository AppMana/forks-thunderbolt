#!/usr/bin/env bash
set -eu

args=("$@")
argc="${#args[@]}"
target="${args[argc - 2]}"
remote="${args[argc - 1]}"
host="${target#*@}"
host="${host%%.*}"

record() {
	printf '%s\n' "$1" >> "${FAKE_SSH_LOG:?}"
}

case "$remote" in
*"command -v ib_write_bw"*)
	record "preflight:$target"
	;;
*"ip -o -4 addr show scope global"*)
	record "address:$host"
	printf '192.0.2.10\n'
	;;
*"for d in /sys/class/infiniband/usb4_rdma"*)
	record "pick-device:$host"
	printf 'usb4_rdma0\n'
	;;
*"/gid_attrs/ndevs/0"*)
	record "device-peer:$host"
	if [[ "$host" == srv ]]; then
		printf 'tbr-cli\n'
	else
		printf 'tbr-srv\n'
	fi
	;;
*"@@@HOST@@@"*)
	record "snapshot:$host"
	printf '@@@HOST@@@\nhostname=%s\n@@@SUMMARY@@@\nverbs_qps: 1\n' "$host"
	;;
*"nohup setsid"*)
	record "start:$host"
	bash -c "$remote"
	;;
*"deadline="*)
	record "wait:$host"
	bash -c "$remote"
	;;
*"## dmesg"*)
	record "failure-evidence:$host"
	printf '## dmesg\nmock\n## blocked processes\nmock\n'
	;;
*"cat '/tmp/tbv-hang-repro-"*)
	record "read-log:$host"
	bash -c "$remote"
	;;
*"rm -f '/tmp/tbv-hang-repro-"*)
	record "cleanup:$host"
	bash -c "$remote"
	;;
*)
	printf 'unexpected fake ssh command for %s: %s\n' "$target" "$remote" >&2
	exit 2
	;;
esac

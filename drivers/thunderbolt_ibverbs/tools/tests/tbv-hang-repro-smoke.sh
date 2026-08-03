#!/usr/bin/env bash
set -euo pipefail

repo_root="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
repro="$repo_root/tools/tbv-hang-repro.sh"
fake_ssh="$repo_root/tools/tests/fake-ssh.sh"
fake_ib="$repo_root/tools/tests/fake-ib-write-bw.sh"
scratch="$(mktemp -d -t tbv-hang-repro-test.XXXXXX)"
cleanup() {
	if [[ -r "$scratch/ib-pids" ]]; then
		while read -r pid; do
			kill "$pid" 2>/dev/null || true
		done < "$scratch/ib-pids"
	fi
	rm -rf "$scratch"
}
trap cleanup EXIT

mkdir -p "$scratch/bin"
ln -s "$fake_ssh" "$scratch/bin/ssh"
ln -s "$fake_ib" "$scratch/bin/ib_write_bw"
export PATH="$scratch/bin:$PATH"
export FAKE_IB_PID_LOG="$scratch/ib-pids"
export SSH_USER=operator
export DOMAIN=example.test
export TBV_ROUNDS=1
export TBV_ROUND_TIMEOUT=2

export FAKE_SSH_MODE=pass
export FAKE_SSH_LOG="$scratch/pass-events"
"$repro" operator@srv cli "$scratch/pass" >"$scratch/pass-stdout"
grep -q '^preflight:operator@srv[.]example[.]test$' "$FAKE_SSH_LOG"
grep -q '^preflight:operator@cli[.]example[.]test$' "$FAKE_SSH_LOG"
grep -q '^== no hang in 1 rounds' "$scratch/pass-stdout"
grep -q '^### round=1 result=0 ' "$scratch/pass/client.txt"
grep -q '^### round=1 result=0 ' "$scratch/pass/server.txt"

export FAKE_SSH_MODE=hang
export FAKE_SSH_LOG="$scratch/hang-events"
set +e
"$repro" operator@srv cli "$scratch/hang" >"$scratch/hang-stdout"
rc=$?
set -e
[[ "$rc" -eq 3 ]]
grep -q '^== FAILURE reproduced at round 1' "$scratch/hang-stdout"
grep -q 'tag=hang-round1' "$scratch/hang/counters-server.txt"
grep -q 'tag=hang-round1' "$scratch/hang/counters-client.txt"

failure_snapshot_line="$(awk -F: '$1 == "snapshot" && $2 == "srv" { n++; if (n == 2) { print NR; exit } }' "$FAKE_SSH_LOG")"
last_cleanup_line="$(awk -F: '$1 == "cleanup" && $2 == "srv" { n=NR } END { print n }' "$FAKE_SSH_LOG")"
[[ -n "$failure_snapshot_line" && -n "$last_cleanup_line" ]]
[[ "$failure_snapshot_line" -lt "$last_cleanup_line" ]]

while read -r pid; do
	if kill -0 "$pid" 2>/dev/null; then
		printf 'reproducer left fake perftest pid %s alive\n' "$pid" >&2
		exit 1
	fi
done < "$FAKE_IB_PID_LOG"

if grep -q 'pkill' "$repro"; then
	printf 'reproducer must not kill perftest processes it did not launch\n' >&2
	exit 1
fi

printf 'tbv-hang-repro smoke tests passed\n'

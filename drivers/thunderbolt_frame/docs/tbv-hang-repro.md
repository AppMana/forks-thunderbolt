# Packaged hang reproducer

`tbv-hang-repro` is a bounded traffic-and-capture harness for a directly
connected Thunderbolt/USB4 RDMA peer pair. It is shipped by the
`thunderbolt-ibverbs-tools` APT package and is also runnable from
`tools/tbv-hang-repro.sh` in a source checkout.

It approximates an NCCL NET/IB data path with large bidirectional
`IBV_WR_RDMA_WRITE` traffic, multiple QPs, and a deep transmit window. It does
not claim to reproduce every NCCL failure. Its purpose is to turn a stall into
a small, repeatable workload with:

- a hard per-round deadline;
- peer-checked `usb4_rdma` HCA selection;
- a manifest containing hosts, devices, workload shape, and bootstrap address;
- before, live failure-time, and after debugfs snapshots from both peers;
- client and server `ib_write_bw` output;
- bounded dmesg and blocked-process evidence.

The command exits zero when all rounds pass, one on preflight/capture failure,
and three when a workload round fails or reaches its deadline.

The defaults are the bounded failure shape currently used for regression
capture: 32 QPs per direction, 200 one-MiB writes per QP, transmit depth 128,
three rounds, and a 90-second deadline per round. Every value is overridable
through the variables listed by `tbv-hang-repro --help`.

At the deadline, the client and server remain alive while the harness captures
debugfs, dmesg, and process wait channels. Cleanup happens afterward and is
limited to the PID/start-time identities launched for that run, so it cannot
kill an unrelated perftest process whose PID was reused.

## Install

```bash
sudo apt-get install thunderbolt-ibverbs-tools
tbv-hang-repro --help
```

The orchestrating host needs Bash, OpenSSH, and GNU coreutils. Remote peers
need passwordless SSH and sudo for capture, coreutils, util-linux, `iproute2`,
`perftest`, mounted debugfs, and the matching loaded
`thunderbolt_frame_rxe` engine (override with `TBV_MODULE`).

## Other packaged diagnostics

The package also contains:

- `tbv-nhi-ring-regs`, a read-only NHI BAR register dumper;
- `tbv-trace-to-kunit`, which summarizes a verbs trace or emits a KUnit replay
  skeleton;
- `tbv-verb-trace.bt`, `tbv-send-timeline.bt`,
  `tbv-ring-progress.bt`, and `tbv-post-trace.bt` under
  `/usr/share/thunderbolt-ibverbs/bpftrace/`.

The older topology-specific benchmarks, Nix module loader, reset helper, and
live reload scripts remain source-only. They contain deployment assumptions or
mutating recovery behavior and are intentionally not installed as ordinary
operator commands.

## Example

```bash
TBV_SERVER_ADDR=192.0.2.10 \
tbv-hang-repro rdma-a.example.net rdma-b.example.net ./captures/qps32
```

By default the tool selects the first non-Thunderbolt global IPv4 address on
the server for perftest bootstrap and derives `tbr-*` peer names from the SSH
hostnames. Use the documented `TBV_SERVER_ADDR`, HCA, or peer-name overrides
when the management DNS name differs from the driver's peer identity.

The harness never unloads a module, resets an NHI, reboots a host, changes a
module parameter, or kills an `ib_write_bw` process it did not start. Recovery
and destructive experiments remain outside its scope.

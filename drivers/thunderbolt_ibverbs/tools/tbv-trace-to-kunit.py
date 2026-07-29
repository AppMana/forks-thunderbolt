#!/usr/bin/env python3
"""Turn a captured tbv-verb-trace.bt run into a KUnit replay skeleton.

The point of this tool is that the next hang should cost an hour, not a day. It
does the mechanical half of the conversion: reading the capture, reconstructing
each QP's state machine and WR stream, finding where forward progress stopped,
and emitting a harness declaration plus a suite file wired to the repo's
existing pattern. It does NOT invent the assertion -- a human reads the summary,
decides what contract the driver broke, and fills in the marked body.

Workflow:
  1. sudo bpftrace tools/tbv-verb-trace.bt -o capture.txt      (on both ends)
     alongside tools/tbv-hang-repro.sh, which snapshots debugfs at the freeze.
  2. tools/tbv-trace-to-kunit.py capture.txt --summary
     -> the QP transitions, the WR histogram, the completion statuses and the
        last WRs that never completed.
  3. tools/tbv-trace-to-kunit.py capture.txt --emit <name> [--peers peers.txt]
     -> kernel/tests/<name>_test.c and the tbv.h declaration to paste, with the
        captured facts already written into the comments as provenance.
  4. Implement tbv_test_<name>() next to the code it drives (path.c for path
     state, ibdev.c for QP/WR state; the structs are file-private), add the
     object to kernel/Makefile, and run tools/run-kunit.sh.

Usage:
  tbv-trace-to-kunit.py CAPTURE [--summary] [--emit NAME] [--peers FILE]
                                [--outdir DIR]
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
import sys

# enum ib_wr_opcode, include/rdma/ib_verbs.h. The driver only implements a
# subset; anything else showing up in a capture is itself a finding.
WR_OPCODE = {
    0: "RDMA_WRITE",
    1: "RDMA_WRITE_WITH_IMM",
    2: "SEND",
    3: "SEND_WITH_IMM",
    4: "RDMA_READ",
    5: "ATOMIC_CMP_AND_SWP",
    6: "ATOMIC_FETCH_AND_ADD",
    8: "LOCAL_INV",
    9: "SEND_WITH_INV",
}

QP_STATE = {0: "RESET", 1: "INIT", 2: "RTR", 3: "RTS", 4: "SQD", 5: "SQE", 6: "ERR"}

# The driver reports negative errnos through the send done/complete callbacks.
STATUS = {
    0: "ok",
    -110: "ETIMEDOUT (surfaces as IBV_WC_RETRY_EXC_ERR)",
    -125: "ECANCELED (flush)",
    -104: "ECONNRESET",
    -12: "ENOMEM",
    -107: "ENOTCONN",
}

LINE = re.compile(
    r"^(?P<seq>\d+)\s+(?P<ts>\d+)\s+(?P<cpu>\d+)\s+(?P<task>\S+)\s+(?P<event>\w+)\s*(?P<kv>.*)$"
)


def parse(path: pathlib.Path) -> list[dict]:
    events = []
    for line in path.read_text(errors="replace").splitlines():
        m = LINE.match(line)
        if not m:
            continue
        ev = m.groupdict()
        fields = {}
        for tok in ev.pop("kv").split():
            if "=" not in tok:
                continue
            k, _, v = tok.partition("=")
            try:
                fields[k] = int(v, 0)
            except ValueError:
                fields[k] = v
        ev["seq"] = int(ev["seq"])
        ev["ts"] = int(ev["ts"])
        ev["fields"] = fields
        events.append(ev)
    events.sort(key=lambda e: e["seq"])
    return events


def summarise(events: list[dict]) -> dict:
    """Reconstruct what the workload did and where it stopped making progress."""
    qp_transitions = collections.defaultdict(list)
    wr_ops = collections.Counter()
    wr_sizes = collections.Counter()
    complete_status = collections.Counter()
    txdone_status = collections.Counter()
    errors = []

    for ev in events:
        f = ev["fields"]
        name = ev["event"]
        if name == "modify_qp":
            qp_transitions[f.get("qpn")].append(f)
        elif name == "post_send_one":
            op = f.get("op")
            wr_ops[WR_OPCODE.get(op, f"op{op}")] += 1
            wr_sizes[f.get("len0", 0)] += 1
        elif name == "send_complete":
            complete_status[f.get("status", 0)] += 1
        elif name == "send_tx_done":
            txdone_status[f.get("status", 0)] += 1
        elif name in ("post_send_ret", "post_send_one_ret", "reserve_data_fail"):
            errors.append(ev)

    # Forward progress: the gap between the last post and the last completion.
    # A capture that ends with posts outstanding and no completions after them
    # is the freeze, and its trailing events are what the replay must recreate.
    last_post = max((e["seq"] for e in events if e["event"] == "post_send_one"), default=0)
    last_done = max(
        (e["seq"] for e in events if e["event"] in ("send_complete", "send_tx_done")),
        default=0,
    )

    return {
        "events": len(events),
        "qp_transitions": qp_transitions,
        "wr_ops": wr_ops,
        "wr_sizes": wr_sizes,
        "complete_status": complete_status,
        "txdone_status": txdone_status,
        "errors": errors,
        "last_post": last_post,
        "last_done": last_done,
        "stalled": last_post > last_done,
        "span_ms": (events[-1]["ts"] - events[0]["ts"]) / 1e6 if events else 0.0,
    }


def print_summary(s: dict, out=sys.stdout) -> None:
    p = lambda *a: print(*a, file=out)
    p(f"events={s['events']} span={s['span_ms']:.1f}ms")

    p("\n== QP state machine (first QP, verbatim attrs) ==")
    for qpn, transitions in list(s["qp_transitions"].items())[:1]:
        for t in transitions:
            state = QP_STATE.get(t.get("state"), t.get("state"))
            attrs = " ".join(
                f"{k}={v}"
                for k, v in t.items()
                if k not in ("qpn", "state", "mask") and v not in (0, None)
            )
            p(f"  qpn={qpn} mask=0x{t.get('mask', 0):x} -> {state} {attrs}")

    p("\n== work requests ==")
    for op, n in s["wr_ops"].most_common():
        p(f"  {op:24s} {n}")
    p("  sizes: " + ", ".join(f"{sz}B x{n}" for sz, n in s["wr_sizes"].most_common(6)))

    p("\n== completions ==")
    for st, n in s["complete_status"].most_common():
        p(f"  send_complete  status={st:<5} {n:8d}  {STATUS.get(st, '')}")
    for st, n in s["txdone_status"].most_common():
        p(f"  send_tx_done   status={st:<5} {n:8d}  {STATUS.get(st, '')}")

    if s["errors"]:
        p(f"\n== {len(s['errors'])} rejected/failed calls (first 10) ==")
        for e in s["errors"][:10]:
            p(f"  seq={e['seq']} {e['event']} {e['fields']}")

    p("\n== forward progress ==")
    if s["stalled"]:
        p(
            f"  STALL: last post at seq={s['last_post']} is after the last "
            f"completion at seq={s['last_done']}."
        )
        p("  The WRs posted after that completion never finished. That is the replay.")
    else:
        p(
            f"  every post was followed by a completion (last post seq={s['last_post']}, "
            f"last completion seq={s['last_done']})."
        )
        p("  If the workload still hung, the stall is below the verb layer --")
        p("  read the debugfs peers/summary snapshot for path credits and tx_poll.")


SUITE_TEMPLATE = '''// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: replay of a captured {name} stall.
 *
 * Generated from a tbv-verb-trace.bt capture by tools/tbv-trace-to-kunit.py.
 * Raw capture and the distilled sequence live in
 * drivers/thunderbolt_ibverbs/traces/{name}/.
 *
 * Captured facts this replay is built on:
{provenance}
 *
 * Built into the module on CONFIG_KUNIT; run with tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include "../tbv.h"

/*
 * TODO(author): assert the contract the capture shows the driver breaking.
 * Keep at least one case that pins the scenario itself down -- a replay whose
 * setup silently stops reproducing the captured state passes for the wrong
 * reason, which is how a hand-modelled test went green against code that could
 * not work.
 */
static void tbv_{name}_makes_forward_progress(struct kunit *test)
{{
	u32 done_calls = 0;
	int status = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_{name}(&done_calls, &status),
			0);

	KUNIT_EXPECT_GT_MSG(test, done_calls, 0U,
			    "the captured WR never completed, so its tx_pending never drains");
}}

static struct kunit_case tbv_{name}_cases[] = {{
	KUNIT_CASE(tbv_{name}_makes_forward_progress),
	{{}}
}};

static struct kunit_suite tbv_{name}_suite = {{
	.name = "thunderbolt_ibverbs_{name}",
	.test_cases = tbv_{name}_cases,
}};

kunit_test_suite(tbv_{name}_suite);
'''


def emit(name: str, s: dict, outdir: pathlib.Path) -> None:
    lines = []
    for qpn, transitions in list(s["qp_transitions"].items())[:1]:
        for t in transitions:
            state = QP_STATE.get(t.get("state"), t.get("state"))
            lines.append(f" *   modify_qp qpn={qpn} mask=0x{t.get('mask', 0):x} -> {state}")
    for op, n in s["wr_ops"].most_common(4):
        lines.append(f" *   {n} x {op}")
    lines.append(
        " *   sizes " + ", ".join(f"{sz}B" for sz, _ in s["wr_sizes"].most_common(4))
    )
    for st, n in s["complete_status"].most_common(4):
        lines.append(f" *   send_complete status={st} x{n}  {STATUS.get(st, '')}")
    if s["stalled"]:
        lines.append(
            f" *   STALL: post at seq={s['last_post']} never completed "
            f"(last completion seq={s['last_done']})"
        )

    outdir.mkdir(parents=True, exist_ok=True)
    suite = outdir / f"{name}_test.c"
    suite.write_text(
        SUITE_TEMPLATE.format(name=name, provenance="\n".join(lines))
    )

    print(f"wrote {suite}")
    print("\nAdd to kernel/Makefile:")
    print(f"thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/{name}_test.o")
    print("\nAdd to the CONFIG_KUNIT block in kernel/tbv.h:")
    print(f"int tbv_test_{name}(u32 *done_calls_out, int *status_out);")
    print(
        "\nThen implement tbv_test_%s() next to the code it drives "
        "(path.c for path/ring/credit state, ibdev.c for QP and WR state)." % name
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", type=pathlib.Path)
    ap.add_argument("--summary", action="store_true", help="print the distilled sequence")
    ap.add_argument("--emit", metavar="NAME", help="emit a KUnit suite skeleton")
    ap.add_argument("--outdir", type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parent.parent / "kernel/tests")
    args = ap.parse_args()

    events = parse(args.capture)
    if not events:
        print(f"no tbv-verb-trace records in {args.capture}", file=sys.stderr)
        return 1

    s = summarise(events)
    if args.summary or not args.emit:
        print_summary(s)
    if args.emit:
        emit(args.emit, s, args.outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())

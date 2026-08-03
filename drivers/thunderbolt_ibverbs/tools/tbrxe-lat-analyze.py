#!/usr/bin/env python3
"""Per-stage latency budget from two tbv-lat-timeline.bt traces.

Reads the client-side and server-side event logs captured during an
ib_send_lat run (single QP, one WR in flight) and attributes microseconds
to named stages. Events per node (ns timestamps, monotonic per node):

  POST  rxe_post_send        (syscall ctx)
  SEND  rxe_sender           (rxe_wq worker: requester+completer)
  RECV  rxe_receiver         (rxe_wq worker: responder)
  XMIT  tbframe_xmit op psn  (engine task ctx)
  ENQ   __tb_ring_enqueue    (doorbell)
  IRQ   ring_msix ring=      (hardirq)
  WORK  ring_work work=      (ring wq worker)
  CLIRX tbrxe_client_rx op psn (ring wq worker)
  TXC   tbframe_core_tx_complete
  CQ    rxe_cq_post

Stage decomposition per node, matched around each data-packet XMIT
(op 0x04 = RC SEND_ONLY / 0x00..0x05 request class) and each CLIRX:

  post_to_sender   POST -> next SEND        (rxe_wq wakeup)
  sender_to_xmit   SEND -> XMIT(data)       (packet build)
  xmit_to_enq      XMIT -> ENQ              (ring_tx + doorbell write)
  enq_to_txirq     ENQ -> IRQ(tx ring)      (local TX completion irq =
                                             direct interrupt moderation
                                             measurement)
  rxirq_to_work    IRQ(rx ring) -> WORK     (ring wq wakeup)
  work_to_clirx    WORK -> CLIRX            (descriptor drain + copy)
  clirx_to_recv    CLIRX(send) -> RECV      (rxe_wq wakeup, responder)
  clirx_to_send    CLIRX(ack) -> SEND       (rxe_wq wakeup, completer)
  recv_to_ackxmit  RECV -> XMIT(ack)        (responder ack build)
  recv_to_cq       RECV -> CQ               (recv CQE post)
  cq_to_post       CQ -> POST               (userspace poll + post turnaround)

Cross-node residual: perftest RTT/2 minus the in-node sums = wire +
RX-IRQ moderation delay (unsynchronized clocks prevent direct measure;
enq_to_txirq bounds the moderation term independently).

Usage: lat-analyze.py client.txt server.txt [--csv]
"""
import re
import sys
from collections import defaultdict


def parse(path):
    evs = []
    rx = re.compile(
        r"^(\d+) (\w+) cpu=(\d+)(?: op=([0-9a-f]+) psn=(\d+))?"
        r"(?: ring=([0-9a-f]+))?(?: work=([0-9a-f]+))?")
    with open(path) as fh:
        for line in fh:
            m = rx.match(line.strip())
            if not m:
                continue
            ns, ev, cpu, op, psn, ring, work = m.groups()
            evs.append({
                "t": int(ns), "ev": ev, "cpu": int(cpu),
                "op": int(op, 16) if op else None,
                "psn": int(psn) if psn else None,
                "ring": ring, "work": work,
            })
    evs.sort(key=lambda e: e["t"])
    return evs


REQ_OPS = set(range(0x00, 0x0C))          # RC request class
ACK_OPS = {0x11}                           # RC ACKNOWLEDGE


def ring_work_map(evs):
    """work ptr = ring ptr + 0x50 observed; map by pairing IRQ->WORK."""
    pairs = defaultdict(lambda: defaultdict(int))
    last_irq = None
    for e in evs:
        if e["ev"] == "IRQ":
            last_irq = e
        elif e["ev"] == "WORK" and last_irq and \
                e["t"] - last_irq["t"] < 1_000_000:
            pairs[e["work"]][last_irq["ring"]] += 1
    return {w: max(r, key=r.get) for w, r in pairs.items()}


def classify_rings(evs):
    """The ring ENQ'd just after a data XMIT is the TX data ring; the ring
    whose IRQ precedes a CLIRX is the RX data ring."""
    tx_ring = defaultdict(int)
    rx_ring = defaultdict(int)
    last = None
    last_irq = None
    for e in evs:
        if e["ev"] == "XMIT":
            last = ("XMIT", e["t"])
        elif e["ev"] == "ENQ":
            if last and last[0] == "XMIT" and e["t"] - last[1] < 100_000:
                tx_ring[e["ring"]] += 1
            last = None
        elif e["ev"] == "IRQ":
            last_irq = e
        elif e["ev"] == "CLIRX" and last_irq and \
                e["t"] - last_irq["t"] < 2_000_000:
            rx_ring[last_irq["ring"]] += 1
    tx = max(tx_ring, key=tx_ring.get) if tx_ring else None
    rx = max(rx_ring, key=rx_ring.get) if rx_ring else None
    return tx, rx


def deltas(evs):
    tx, rx = classify_rings(evs)
    d = defaultdict(list)
    migrations = defaultdict(int)
    prev = {}
    pend = {}

    def note(stage, t0, t1, c0=None, c1=None):
        d[stage].append(t1 - t0)
        if c0 is not None and c1 is not None and c0 != c1:
            migrations[stage] += 1

    for e in evs:
        ev = e["ev"]
        if ev == "POST":
            pend["POST"] = e
        elif ev == "SEND":
            p = pend.pop("POST", None)
            if p and e["t"] - p["t"] < 1_000_000:
                note("post_to_sender", p["t"], e["t"], p["cpu"], e["cpu"])
            a = pend.pop("CLIRX_ACK", None)
            if a and e["t"] - a["t"] < 1_000_000:
                note("clirx_to_sender", a["t"], e["t"], a["cpu"], e["cpu"])
            pend["SEND"] = e
        elif ev == "XMIT":
            if e["op"] in REQ_OPS:
                s = pend.pop("SEND", None)
                if s and e["t"] - s["t"] < 1_000_000:
                    note("sender_to_xmit", s["t"], e["t"])
                pend["XMIT"] = e
            elif e["op"] in ACK_OPS:
                r = pend.pop("RECV", None)
                if r and e["t"] - r["t"] < 1_000_000:
                    note("recv_to_ackxmit", r["t"], e["t"])
                pend["XMIT"] = e
        elif ev == "ENQ":
            x = pend.pop("XMIT", None)
            if x and e["t"] - x["t"] < 100_000:
                note("xmit_to_enq", x["t"], e["t"])
            if e["ring"] == tx:
                pend["ENQ_TX"] = e
        elif ev == "IRQ":
            if e["ring"] == tx:
                q = pend.pop("ENQ_TX", None)
                if q and e["t"] - q["t"] < 10_000_000:
                    note("enq_to_txirq", q["t"], e["t"])
            if e["ring"] == rx:
                pend["IRQ_RX"] = e
        elif ev == "WORK":
            i = pend.pop("IRQ_RX", None)
            if i and e["t"] - i["t"] < 1_000_000:
                note("rxirq_to_work", i["t"], e["t"], i["cpu"], e["cpu"])
                pend["WORK_RX"] = e
        elif ev == "CLIRX":
            w = pend.pop("WORK_RX", None)
            if w and e["t"] - w["t"] < 1_000_000:
                note("work_to_clirx", w["t"], e["t"])
            if e["op"] in REQ_OPS:
                pend["CLIRX_REQ"] = e
            elif e["op"] in ACK_OPS:
                pend["CLIRX_ACK"] = e
        elif ev == "RECV":
            c = pend.pop("CLIRX_REQ", None)
            if c and e["t"] - c["t"] < 1_000_000:
                note("clirx_to_receiver", c["t"], e["t"], c["cpu"], e["cpu"])
            pend["RECV"] = e
        elif ev == "CQ":
            r = pend.get("RECV")
            if r and e["t"] - r["t"] < 1_000_000:
                note("recv_to_cq", r["t"], e["t"])
            pend["CQ"] = e
        prev[ev] = e
    # userspace turnaround: CQ -> next POST
    lastcq = None
    for e in evs:
        if e["ev"] == "CQ":
            lastcq = e
        elif e["ev"] == "POST" and lastcq and \
                e["t"] - lastcq["t"] < 1_000_000:
            d["cq_to_post"].append(e["t"] - lastcq["t"])
            lastcq = None
    return d, migrations


def stats(v):
    v = sorted(v)
    n = len(v)
    if not n:
        return None
    return (n, v[0] / 1e3, v[n // 2] / 1e3, v[int(n * .99)] / 1e3,
            v[-1] / 1e3)


def main():
    for path in sys.argv[1:3]:
        evs = parse(path)
        d, mig = deltas(evs)
        print(f"\n=== {path} ({len(evs)} events) ===")
        print(f"{'stage':>18} {'n':>6} {'min':>8} {'med':>8} "
              f"{'p99':>8} {'max':>9}  (us)")
        order = ["post_to_sender", "sender_to_xmit", "xmit_to_enq",
                 "enq_to_txirq", "rxirq_to_work", "work_to_clirx",
                 "clirx_to_receiver", "recv_to_ackxmit", "recv_to_cq",
                 "clirx_to_sender", "cq_to_post"]
        for k in order + [k for k in d if k not in order]:
            s = stats(d.get(k, []))
            if not s:
                continue
            n, mn, md, p99, mx = s
            m = f"  mig={mig[k]}" if mig.get(k) else ""
            print(f"{k:>18} {n:>6} {mn:>8.2f} {md:>8.2f} "
                  f"{p99:>8.2f} {mx:>9.2f}{m}")


if __name__ == "__main__":
    main()

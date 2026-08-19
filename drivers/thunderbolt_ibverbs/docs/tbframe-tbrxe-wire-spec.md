# tbframe / tbrxe wire and contract specification

Status: draft for review. Companion to the approved restructuring plan
(rxe-derived IB engine over a lossless Thunderbolt frame service). This
document is normative for the new stack; where it conflicts with the legacy
`proto/native_data.h` protocol, that protocol is being retired.

## 1. Scope and layering

```
 +--------------------------------------------------------------+
 |  tbrxe: rxe-derived IB engine (BTH..ICRC, PSN reliability)   |
 +--------------------------------------------------------------+
 |  tbframe: session + lossless frame service on XDomain rings  |
 +--------------------------------------------------------------+
 |  thunderbolt core: XDomain discovery, lane bonding, NHI ring |
 +--------------------------------------------------------------+
```

- tbframe owns all hardware access (rings, HopIDs, tunnels, XDomain
  control-channel messaging). tbrxe never touches `tb_*` symbols.
- tbrxe owns everything from the first payload byte of a data frame onward.
  tbframe never parses IB headers.
- Lane bonding remains in the forked `drivers/thunderbolt/xdomain.c`
  (`b0f8ff4`), below this specification entirely.

## 2. Data frame format

One TB ring frame carries exactly one tbrxe packet. No fragmentation or
reassembly exists at the tbframe layer.

```
 offset  size   field
 0       12     BTH (per IBA / rxe_hdr.h, verbatim)
 12      var    extension headers per rxe_opcode[] table (RETH/AETH/...)
 var     var    payload
 var     0..3   pad (BTH PadCount)
 last    4      ICRC (see section 4)
```

- Maximum frame size: 4096 bytes (NHI 12-bit descriptor length).
- Frames are variable-length; the descriptor length is authoritative and
  must equal the IBA transport unit length (`paylen` in rxe terms).
- There is no tbframe-level data header. Demultiplexing needs are covered
  by (a) the ring pair being exclusively owned by one tbrxe session and
  (b) the PDF nibbles below.

### PDF (sof/eof) nibble assignment

The NHI provides 4-bit sof/eof protocol-defined fields per frame, filtered
by RX masks. On the wire a frame is segmented into transport packets; the
SOF PDF marks the first packet and the EOF PDF the last, and the receiving
NHI closes a frame on any packet whose PDF matches the RX ring's
`eof_mask`. The SOF marker therefore MUST be distinct from every EOF
(frame-type) value: with `sof == eof` (wire v1) the peer chopped every
multi-packet frame at the first transport-packet boundary, so nothing
above one packet (~252 bytes) was ever delivered. This is the same reason
tbnet splits FRAME_START/FRAME_END. Assignment for tbframe rings (wire
v2):

| sof | eof | meaning |
|-----|-----|---------|
| 0x6 | 0x4 | tbrxe data packet (BTH..ICRC) |
| 0x6 | 0x5 | tbframe keepalive/probe (payload: 8-byte session cookie) |

The frame type rides in the EOF PDF; single-packet frames carry only that
nibble, and the RX descriptor reports only the closing PDF (sof reads back
0). Values 0x1/0x2 are avoided (tbnet uses them); 0xF is avoided (control
channel convention). RX rings are started with `sof_mask = BIT(6)` and
`eof_mask = BIT(4)|BIT(5)`. Frames failing the mask are dropped by
hardware. A future frame type gets a new EOF nibble value, never a header
change.

## 3. Session establishment (control plane)

Control messages ride the XDomain control channel (`tb_xdomain_request/
response`), exactly like ThunderboltIP login — never the data rings.

State machine: reuse the shared `thunderbolt_negotiation.h` contract
verbatim (`tb_xdomain_handshake`: request_sent + peer_seen, supersede on
inbound HELLO while established, generation gate accepting backwards
generations, zombie predicate). Reference sequence for ring bring-up is
`tbnet_connected_work`: allocate rings, register in-HopID from the peer's
advertised transmit HopID, prime all RX buffers, start rings, and only then
`tb_xdomain_enable_paths`.

HELLO payload (new, replaces the legacy tbv HELLO):

| field | size | semantics |
|---|---|---|
| proto_version | u16 | this spec, starts at 1; mismatch = refuse session |
| transmit_hopid | u16 | HopID the sender transmits on |
| rx_ring_entries | u16 | power of two, 256..4096; basis of Mode A window |
| capabilities | u32 | bit 0: E2E supported; bit 1: keepalive; rest reserved |
| gid_eui64 | u64 | sender's per-link ULA EUI-64 (GID derivation) |
| session_cookie | u64 | random per boot; echoed in keepalives |

READY confirms both sides observed each other's HELLO and the paths are
enabled. Data may flow only after READY completes in both directions.
READY_ACK is withheld while a teardown of the current session is pending:
the ack certifies the paths it vouches for, and certifying entries that
are queued for removal lets the peer stream a full TX ring into a
half-torn-down path (the 2026-08-18 router egress wedge).

BYE (op 5) / BYE_ACK (op 6), additive: orderly-teardown quiesce, the
ThunderboltIP logout analog. A side about to tear its session down for a
reason the peer cannot know about (local close, dead-path verify) sends
BYE before touching any hop entry or ring. The receiver downs its session
(reason LOGOUT: admission closed, rings cancelled, automatic re-handshake
later) and acks only once it has left UP, so the ack certifies "no more
frames from this side". The sender's wait is bounded (3 x 300 ms) and a
peer that does not know BYE simply never consumes it -- degradation is
exactly the pre-BYE behavior. Rationale: a transmitter left streaming
into a peer's disabled or absent ingress hop wedges its OWN router
egress persistently (credit state, reset-only recovery); measured on the
023/025 canaries 2026-08-18.

### Link liveness

- Level-triggered verify: `tb_xdomain_paths_active()` read-back on a timer
  (default 5 s) while the session is up; failure -> `link_down(reason)` and
  re-handshake. Hardware hot-events are hints, never the mechanism.
- Optional keepalive frames (capability bit 1) carry the session cookie; a
  cookie mismatch is a supersede signal (peer rebooted within one verify
  interval).

## 4. ICRC

The 4-byte ICRC trailer is retained (it catches DMA/IOMMU and software
assembly corruption, which this fabric has demonstrated). Computation
follows `rxe_icrc.c` with one deviation: the IP/UDP pseudo-header is
removed entirely.

- seed: `crc32_le` state after 8 bytes of 0xFF with initial value
  0xffffffff (identical to rxe's masked-LRH seed 0xdebb20e3).
- input: BTH with the QPN-adjacent reserved byte masked to ones
  (`bth->qpn |= ~BTH_QPN_MASK`), then remaining extension headers verbatim,
  then payload + pad.
- trailer: inverted CRC, little-endian, last 4 bytes of the frame.

Rationale: nothing mutates in flight on a single-hop link; the masked
IP/UDP block existed only to tolerate router mutation. Both endpoints run
this fork; cross-implementation interop is a non-goal.

## 5. MTU and packetization

- Baseline rule: strict `IB_MTU_2048`. Worst-case transport unit =
  80 (header budget, `RXE_MAX_HDR_LENGTH`) + 2048 + pad + 4 <= 4096. All
  rxe packetization grammar applies unchanged (FIRST/MIDDLE exactly MTU,
  LAST in (0, MTU], ONLY <= MTU). Links whose frame payload budget cannot
  carry the deviated transport unit below stay on this rule.
- MTU-4096 deviation (in effect since wire v3; frames/sec was measured as
  the bandwidth ceiling on the 023/025 canaries): report `IB_MTU_4096` in
  verbs while fragmenting at `4096 - 80 - 4` aligned down to 4 = 4012-byte
  payload ceiling. The packetization grammar is unchanged in shape with
  the effective MTU being 4012 (FIRST/MIDDLE exactly 4012, pad-free by
  4-alignment); worst-case transport unit = 80 + 4012 + 4 = 4096.
  Wire-consistent because both ends derive the ceiling from this spec;
  documented as a fork deviation from IBA MTU enums. Mixed wire versions
  refuse the session at HELLO (a v2 responder would NAK every 4012-byte
  FIRST/MIDDLE as "not mtu").

## 6. Flow control

Correctness never depends on hardware E2E.

### Mode A — static window (default, universal)

- The sender bounds aggregate unacknowledged data frames per link to
  `min(local cap, peer rx_ring_entries)` from HELLO.
- Enforcement: per-QP inflight caps (rxe's existing
  `RXE_INFLIGHT_SKBS_PER_QP_HIGH` analog, plus the 128-PSN unacked window)
  summed over the link; the tbframe admission check refuses `xmit` beyond
  the link cap and invokes the `tx_released` upcall as completions drain.
- Invariant: every unacked data frame occupies at most one peer RX
  descriptor, and the peer reposts descriptors before its engine emits the
  ACK that releases the window. Therefore `window <= rx_ring_entries`
  implies the RX ring cannot overflow. No credit messages exist; there is
  nothing to lose or desynchronize.
- ACK/control frames are small and bounded by data received (one ACK per
  ack-req packet); they are admitted outside the data window but counted
  against a fixed reserve (ring is sized with a control reserve:
  `usable data window = rx_ring_entries - 64`).

### Mode B — hardware E2E (negotiated enhancement)

- Enabled only when both HELLOs advertise capability bit 0 and the link is
  Gen 4 (Maple Ridge). RX rings get `RING_FLAG_E2E` with the paired TX
  HopID per `tb_ring_alloc_rx` convention.
- Effect: the Mode A admission cap may be raised above `rx_ring_entries`
  (hardware backpressures). The PSN machinery is unchanged.
- Revocable per link at HELLO time (config knob) if the known Intel
  "passes saturated run, wedges newly created rings" behavior resurfaces.

### What the PSN layer still covers (either mode)

CRC-dropped frames, frames lost across `link_down -> link_up` transitions,
and duplicates from retransmission. Loss model contract: frames are never
silently lost while `link_state == UP`; any loss window is bracketed by a
link_down/link_up pair and session reset.

## 7. tbframe kernel API (contract for tbrxe)

Downcalls (tbrxe -> tbframe), mirroring `rxe_net.c`'s surface:

```c
struct tbframe_link;                     /* one peer, one ring pair */
int  tbframe_alloc_frame(struct tbframe_link *l, size_t paylen,
                         struct tbframe_frame **f);   /* headroom-free */
int  tbframe_xmit(struct tbframe_link *l, struct tbframe_frame *f);
                                         /* consumes f; -ENOSPC = window */
void tbframe_frame_free(struct tbframe_frame *f);
const char *tbframe_link_name(struct tbframe_link *l);
```

Upcalls (tbframe -> tbrxe), registered at link open:

```c
void (*rx)(void *ctx, struct tbframe_frame *f);   /* NAPI batch context */
void (*tx_released)(void *ctx);                   /* window reopened */
void (*link_up)(void *ctx, u16 mtu, u8 width, u8 speed);
void (*link_down)(void *ctx, enum tbframe_down_reason r);
```

Rules: no upcall is invoked with tbframe locks held; `rx` batches are
bounded; all downcalls are non-blocking and callable from the engine's
task context; every internal tbframe wait on hardware is
`readx_poll_timeout`-bounded and a timeout poisons the link, never blocks
the caller.

## 8. GID and addressing (normative; verified against the NCCL fork and
## ib_core, see nccl-routing-requirements analysis 2026-08-03)

The addressing model is the fleet's proven per-rail scheme, unchanged:

- **One ib_device per tbframe link**, published at that link's first
  `link_up`, unpublished on terminal link_down. NCCL's rail routing selects
  a *device* by GID /64 match (listen handle advertises every device's GID
  table; connector picks the device sharing a /64 with a peer GID), so
  per-link devices are the unit of routing. Naming stays `usb4_rdma*`
  compatible for the webhook's HCA patterns.
- **One GID-anchor netdev per ib_device**, bound with
  `ib_device_set_netdev` before registration: IFF_NOARP, xmit drops — it
  carries no data, ever. The kernel makes this mandatory: RoCE GID tables
  populate ONLY from a bound netdev's IPs (`add_roce_gid` rejects a NULL
  ndev), and RC `modify_qp(RTR)` runs a FIB lookup bound to the sgid's
  netdev. All GID table management is ib_core's; the driver implements no
  query_gid for RoCE and carries no identity of its own.
- **Addresses are provisioned from userspace by the existing Ansible-owned
  tooling** (`tbv-rdma-addr` convention): each cable gets a deterministic
  ULA /64 (sha256 of the sorted endpoint UUID pair), `::1`/`::2` at the
  ends, applied with `ip -6 addr replace`. That single address creates the
  GID entry, the connected route RTR resolves over, and the /64-match
  signal NCCL routes by. Ahead-of-time topology lives in addressing, where
  it is inspectable with `ip addr`/`ip route`.
- The engine needs no peer table and no GID lookup: each ib_device is
  bound to exactly one point-to-point link; the transport for a packet is
  the device's link. The HELLO `gid_eui64`/`local_gid_eui64` fields remain
  in the wire format for diagnostics but are not an addressing authority.
- No identity refresh, no RTR-time rebind: a GID is valid exactly as long
  as its netdev's address, which outlives session resets; supersede/zombie
  transitions bracket loss windows via link_down/link_up without touching
  addressing.

# NASDAQ Exchange Simulator in C++ (`~/nasdaq-sim-cpp`)

## Context

An expert-grade NASDAQ order book simulator in C++ — a **standalone project with no relation** to the Python simulator in the current directory. Lives at `~/nasdaq-sim-cpp` as its own git repo.

Agreed decisions:
- **Protocols**: a **documented subset** of OUCH 4.2 (order entry over SoupBinTCP/TCP) and ITCH 5.0 (market data over MoldUDP64/UDP multicast). No claim of full NASDAQ parity.
- **Strategy**: strict MVP vertical slice first — *OUCH order in → engine match → OUCH response → ITCH event → listener reconstructs book* — then polish (simulator agents, terminal UI, replay, benchmarks, hot-path optimization).
- **Correctness before latency**: the canonical book is simple and provably correct with heavy invariant checking; the optimized book comes later, validated by differential fuzz testing against the reference.
- **Toolchain**: C++20, CMake, minimal deps (STL + raw sockets/kqueue; GoogleTest via FetchContent; no Boost). macOS primary target; socket code behind a thin interface for easy epoll port.

## Non-goals (documented in README)

Auctions/crosses, NOII, MWCB, hidden/pegged orders, real routing (all orders are display, non-routed), MoldUDP64 retransmission recovery (port stubbed, documented), SoupBinTCP reconnect-with-replay, multi-venue behavior, production compliance/regulatory semantics.

## Architecture

Classic exchange topology: single-writer matching-engine thread, I/O at the edges.

```
 OUCH clients ──TCP──▶ Gateway ──queue──▶ Engine thread ──queue──▶ Feed ──UDP multicast──▶ ITCH listeners
 (flow gen /           (kqueue loop,      (matching,       │       (MoldUDP64
  marketsim)            SoupBinTCP +       single writer)  │        packetizer)
                        OUCH codec,                        ▼
                        risk checks) ◀──queue── OUCH responses (acks/fills/cancels)
```

Queues start as plain mutex-guarded MPSC/SPSC queues; lock-free rings are an optimization-phase swap behind the same interface.

### Matching engine
- Price-time priority, per-symbol book. Fixed-point prices (int64, ×10000), `uint64` ns timestamps, 8-char padded symbols.
- **Reference book (canonical)**: sorted containers (`std::map` per side), FIFO queue per level, `order_id → order` hash map. Simple, auditable, invariant-checked. This is the book the MVP ships with.
- **Optimized book (later phase)**: array-indexed price ladder, order pool, intrusive lists — same interface, proven equivalent by differential fuzzing (identical command stream → assert identical fills, events, and book state vs reference). Re-centering complexity is confined here and only attempted once the reference oracle exists; if it proves too fiddly, a wide static band is the fallback.
- Emits typed events via a listener interface.

### Invariants (checked in debug builds via `book.check_invariants()`, and continuously in tests)
1. Level aggregate qty == sum of resting order qtys at that level.
2. FIFO: execution order within a level matches insertion order.
3. Best bid < best ask after every operation (no crossed/locked book at rest).
4. Order map ↔ book consistency: every mapped order is in exactly one level list and vice versa.
5. Feed-side: listener's reconstructed book == engine book after every event (checked in integration tests); no Mold sequence gaps.

### Protocol subset
- **OUCH 4.2** in: Enter `O`, Cancel `X`, Replace `U`. Out: System Event `S`, Accepted `A`, Canceled `C`, Executed `E`, Replaced `U`, Rejected `J`.
- **SoupBinTCP**: framing, Login Request/Accepted/Rejected, Sequenced/Unsequenced Data, heartbeats both directions, Logout, End of Session.
- **ITCH 5.0**: System Event `S`, Stock Directory `R`, Trading Action `H`, Add Order `A`/`F`, Executed `E`, Executed w/ Price `C`, Cancel `X`, Delete `D`, Replace `U`, Trade `P`. Both encode and decode (decode used by listener/replay).
- **MoldUDP64**: downstream packets, heartbeats, end-of-session; gap *detection* on receive (recovery is a non-goal).

## Repo layout

```
~/nasdaq-sim-cpp/
├── CMakeLists.txt          # C++20, warnings-as-errors; Debug/Release/ASan/TSan presets
├── README.md               # architecture, build/run, subset + non-goals, measured numbers
├── docs/design.md          # this design, committed in phase 0
├── src/
│   ├── common/  types, endian rw, fixed-point price, nanoclock (+ later: pool, ring, histogram)
│   ├── book/    reference book now; optimized ladder book later, same interface
│   ├── ouch/  soup/  itch/  mold/     # message structs + codecs, packed, static_assert'd
│   ├── gateway/  feed/  engine/       # sockets, ITCH publishing, thread wiring
│   └── apps/    exchanged, flowgen→marketsim, itchview, itchreplay, bench
└── tests/       # GoogleTest unit + integration + differential fuzz
```

## Phases — MVP tier (each phase ends with its Definition of Done + a commit)

**0. Scaffold.** Repo init, CMake + GoogleTest, `common/` minimum (types, endian, fixed-point, clock), `docs/design.md` committed.
   *DoD: `ctest` runs green on endian/price round-trip tests.*

**1. Reference matching engine + invariants.** Full order lifecycle (add/cancel/replace/execute, partial fills, walk-the-book, replace loses time priority), `check_invariants()`, exhaustive unit tests + randomized invariant test (10k random ops, invariants hold after each).
   *DoD: all book tests green; randomized run clean under ASan; trivial orders/sec number printed by a micro-bench test (O(n²) canary, not a perf goal).*

**2. In-process demo.** Small `demo` app: scripted + random command stream → engine → printed fills and final book, native structs, no codecs.
   *DoD: running `demo` shows sane matching output; doubles as a living example.*

**3. OUCH 4.2 + SoupBinTCP codecs.** Packed structs (size/offset static_asserts), encode/decode, SoupBin session state machine tested purely (no sockets): login, sequenced data, heartbeat timeout, logout.
   *DoD: golden-byte round-trip tests green; state machine test covers login→data→timeout paths.*

**4. ITCH 5.0 + MoldUDP64 codecs.** Message structs encode/decode, mold packetizer (MTU-aware batching) + depacketizer with sequence/gap tracking.
   *DoD: golden-byte tests green; packetize→depacketize round-trip preserves message stream; injected gap is detected.*

**5. Gateway + engine wiring (TCP slice).** kqueue TCP server, SoupBin sessions, OUCH decode → basic risk checks (price/qty bounds, duplicate token) → engine thread → OUCH responses back out.
   *DoD: integration test: raw test client connects, logs in, enters two crossing orders, receives Accepted + Executed over the wire.*

**6. Feed + listener (multicast slice) → MVP complete.** Engine events → ITCH → mold → UDP multicast (loopback); `itchlisten` receives, reconstructs book, checks invariants + gap-free sequencing; ~100-line random flow generator (`flowgen`) drives the exchange.
   *DoD: run `exchanged` + `flowgen` + `itchlisten`: listener's reconstructed book matches engine's snapshot at end of session; zero gaps. This is the full vertical slice demo.*

## Phases — polish tier (after MVP works)

**7. Optimized book.** Ladder + order pool + intrusive lists behind the book interface; differential fuzz vs reference (1M+ random ops, identical outputs); re-centering or static-band fallback; keep reference book in tree as the test oracle.
   *DoD: fuzz equivalence green; micro-bench shows improvement; hot path shown allocation-free (counting allocator).*

**8. marketsim.** Grow flowgen into N agents (passive/aggressive/cancel mix, Poisson-ish arrivals) over real OUCH connections.
   *DoD: multi-client run produces a stable two-sided market visible on the feed.*

**9. itchview terminal UI.** Live ladder display on top of the listener's reconstruction.
   *DoD: watching marketsim through itchview shows a live book.*

**10. itchreplay.** Capture tee in feed → dump-to-text + paced replay onto multicast.
   *DoD: capture a session, replay it, listener reconstructs identical final book.*

**11. bench + queue optimization + README.** SPSC ring swap-in (TSan-verified), latency histograms (p50/p99/p99.9) for order→ack and order→ITCH-out, README with architecture diagram, measured numbers, subset/non-goals.
   *DoD: bench prints histogram tables; TSan clean; README complete.*

## Verification (overall)

- `ctest` green at every phase; ASan on unit suites, TSan on threaded integration tests.
- MVP acceptance = phase 6 DoD (end-to-end slice with book-equality check).
- Optimization acceptance = differential fuzz equivalence, never benchmarks alone.

## Notes

- All work happens in `~/nasdaq-sim-cpp` (fresh `git init`); nothing references the Python project in this directory.

# nasdaq-sim-cpp

A NASDAQ-style exchange simulator in C++20: price-time-priority matching
engine, OUCH 4.2 order entry over SoupBinTCP (TCP), and ITCH 5.0 market data
over MoldUDP64 (UDP multicast), with a multi-agent market simulator, a live
terminal book viewer, capture/replay tooling, and latency benchmarks.

Implements a **documented subset** of the NASDAQ protocols (see
[Protocol subset](#protocol-subset) and [Non-goals](#non-goals)) — this is a
simulator built for engineering fidelity, not a claim of full exchange parity.

```
 OUCH clients ──TCP──▶ Gateway ──queue──▶ Engine thread ──queue──▶ Feed ──UDP multicast──▶ ITCH listeners
 (flowgen /            (kqueue loop,      (matching,       │       (ITCH 5.0 encode,       (itchlisten,
  marketsim)            SoupBinTCP +       single writer,  │        MoldUDP64               itchview,
                        OUCH 4.2 codec,    FastBook)       │        packetizer,             capture-file
                        risk checks)                       ▼        capture tee)            reconstruction)
                                  ◀──queue── OUCH responses (Accepted / Executed / Canceled / Replaced / Rejected)
```

Single-writer engine thread; I/O at the edges; every stage communicates
through queues. Only the engine touches the books.

## The matching engine

Two implementations behind the same semantics:

- **Reference book** (`src/book/order_book.cpp`): `std::map` price levels,
  FIFO lists, invariant-checked (`check_invariants()` validates level
  aggregates, FIFO integrity, index consistency, and no crossed/locked book
  after every test operation). Simple, auditable — kept in tree permanently
  as the test oracle.
- **FastBook** (`src/book/fast_book.cpp`): contiguous price ladder indexed by
  tick offset (band centered on the first price, lazy best-price cursors),
  pooled orders with intrusive FIFO links, and an open-addressing id map
  with backward-shift deletion so a size-bounded steady state never
  rehashes. Out-of-band and off-tick prices degrade gracefully to ordered
  fallback maps. The in-band hot path performs **zero heap allocations**
  (verified by a counting `operator new` test).

**Equivalence is proven, not assumed**: a differential fuzz test drives both
books with the same 500k-op random command stream (adds, partial cancels,
replaces, off-tick and out-of-band prices) and asserts byte-identical event
streams and snapshots. The engine runs FastBook.

Semantics: limit orders, price-time priority, executions at the resting
price, OUCH-style cancel-to-quantity (reduction keeps time priority),
replace always loses time priority, and a replace that would cross is
emitted as delete + fresh order lifecycle (a replaced order must rest to be
an ITCH `U`).

## Protocol subset

| Layer | Implemented |
|---|---|
| OUCH 4.2 in | Enter Order `O`, Cancel `X`, Replace `U` |
| OUCH 4.2 out | System Event `S`, Accepted `A`, Executed `E`, Canceled `C`, Replaced `U`, Rejected `J` |
| SoupBinTCP | framing, Login Accept/Reject, Sequenced/Unsequenced Data, bidirectional heartbeats, 15s receive timeout, Logout, End of Session |
| ITCH 5.0 | System Event `S`, Stock Directory `R`, Trading Action `H`, Add Order `A`/`F`, Executed `E`/`C`, Cancel `X`, Delete `D`, Replace `U`, Trade `P` — encode **and** decode |
| MoldUDP64 | downstream packets (MTU-aware batching), heartbeats, end-of-session, receive-side gap detection |

All codecs are validated against golden byte layouts with spec field
offsets, plus round-trip tests.

## Non-goals

Auctions/crosses, NOII, MWCB, hidden/pegged orders, routing (all orders are
displayed, non-routed), MoldUDP64 retransmission recovery (gaps are
*detected*, not repaired), SoupBinTCP reconnect-with-replay, credential
checking (the gateway accepts all logins), multi-venue behavior, and any
production compliance semantics.

## Build

Requires CMake ≥ 3.24 and a C++20 compiler (developed on macOS/arm64 with
Apple Clang; the event loop is kqueue — socket code is confined to
`gateway/`, `feed/`, and the apps for an easy epoll port).

```sh
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j
ctest --test-dir build-rel
```

Sanitizer presets: `-DSANITIZE_ADDRESS=ON` (ASan+UBSan) or
`-DSANITIZE_THREAD=ON` (TSan). The full suite (97 tests) is green under
both.

## Run the market

```sh
# 1. The exchange: OUCH gateway on TCP 26400, ITCH on 239.192.0.1:26000
./build-rel/exchanged --port 26400 --capture /tmp/session.cap

# 2. A live market: 6 agents (makers + takers) for 60s
./build-rel/marketsim --port 26400 --agents 6 --seconds 60

# 3. Watch the book live (separate terminal)
./build-rel/itchview --group 239.192.0.1 --port 26000

# 4. Verify feed integrity / print the final ladder
./build-rel/itchlisten --group 239.192.0.1 --port 26000

# 5. Inspect and replay the captured session
./build-rel/itchreplay dump /tmp/session.cap | head
./build-rel/itchreplay replay /tmp/session.cap --group 239.192.0.2 --fast
```

Also included: `demo` (in-process matching walkthrough, no sockets) and
`flowgen` (single-connection random flow).

## Measured performance

Apple M-series, Release build (`./build-rel/bench`):

```
book: 2M mixed ops (55% add / 25% cancel / 20% replace)
  reference               6.25M ops/sec
  fastbook               20.49M ops/sec  (3.3x)
  fastbook per-op         p50=44ns   p90=84ns   p99=168ns   p99.9=304ns

queue: cross-thread ping-pong hop latency (200k round trips)
  mutex MpscQueue         p50=13.8µs p90=43µs   p99=106µs
  lock-free SpscRing      p50=128ns  p90=152ns  p99=368ns

end-to-end over real sockets (enter order -> response, 10k orders)
  enter -> Accepted RTT   p50=59µs   p90=82µs   p99=139µs
  enter -> ITCH datagram  p50=70µs   p90=94µs   p99=156µs
```

A note on the queues: the service wiring deliberately uses the blocking
mutex queue — at simulator message rates the end-to-end latency is
dominated by the kernel socket path, not the inter-thread hop, and blocking
consumers make shutdown simple and CPU usage polite. The lock-free
`SpscRing` is implemented, TSan-verified, and benchmarked (~100x lower hop
latency); it is the measured, drop-in upgrade path if the queues ever
become the bottleneck (it would also imply busy-poll consumer loops).

## Testing strategy

- **Golden bytes**: every codec tested against hand-computed layouts with
  spec offsets, plus encode/decode round-trips.
- **Invariant-heavy book tests**: 10k-op randomized run checking all book
  invariants after every operation; ASan-clean.
- **Differential fuzzing**: FastBook vs reference book, 500k ops, identical
  event streams and snapshots required.
- **State machines without sockets**: the SoupBinTCP session is pure
  (bytes in, bytes out, explicit time), so login/heartbeat/timeout paths are
  tested deterministically.
- **Feed integrity end-to-end**: engine events → ITCH → Mold → real UDP →
  listener reconstruction, asserted **equal** to the engine's own book;
  same check for capture files; sequence-gap detection tested with injected
  gaps.
- **Live verification**: marketsim runs produce gap-free feeds whose
  replayed captures reconstruct identical final ladders.

## Layout

```
src/common/   fixed-point price, endian, clock, histogram, queues, SPSC ring
src/book/     reference book (oracle) + FastBook + FlatMap
src/ouch/     OUCH 4.2 codec           src/soup/  SoupBinTCP framing/session
src/itch/     ITCH 5.0 codec           src/mold/  MoldUDP64 packetizer
src/engine/   matching service          src/gateway/  kqueue TCP gateway
src/feed/     ITCH publisher + capture  src/client/   book builder, OUCH client
src/apps/     exchanged, marketsim, flowgen, itchview, itchlisten, itchreplay, bench, demo
tests/        97 tests (GoogleTest)
docs/         design document
```

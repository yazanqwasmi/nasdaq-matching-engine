# nasdaq-matching-engine

[![CI](https://github.com/yazanqwasmi/nasdaq-matching-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/yazanqwasmi/nasdaq-matching-engine/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey.svg)

![Live order book demo](docs/demo.gif)

A NASDAQ-style exchange simulator in C++20: price-time-priority matching
engine, OUCH 4.2 order entry over SoupBinTCP (TCP), and ITCH 5.0 market data
over MoldUDP64 (UDP multicast), with a multi-agent market simulator, a live
terminal book viewer, capture/replay tooling, and latency benchmarks.

Implements a **documented subset** of the NASDAQ protocols (see
[Protocol subset](#protocol-subset) and [Non-goals](#non-goals)) — this is a
simulator built for engineering fidelity, not a claim of full exchange parity.

```
 OUCH clients ──TCP──▶ Gateway ──────queue──────▶ Engine thread ──queue──▶ Feed ──UDP multicast──▶ ITCH listeners
 (flowgen /           (kqueue/epoll loop,        (matching,        │       (ITCH 5.0 encode,       (itchlisten,
  marketsim)           SoupBinTCP + OUCH 4.2      single writer,   │        MoldUDP64               itchview,
                       codec, risk checks)        FastBook)        │        packetizer,             capture-file
                                                                   ▼        capture tee)            reconstruction)
                                  ◀──queue── OUCH responses (Accepted / Executed / Canceled / Replaced / Rejected)
```

Single-writer engine thread; I/O at the edges; every stage communicates
through queues. Only the engine touches the books.

## Highlights

- **Two matching books, proven equivalent.** A simple `std::map` reference
  book (the auditable oracle) and an allocation-free `FastBook` (contiguous
  price ladder, pooled orders, open-addressing id map) are held
  **byte-identical** by a 500k-op differential fuzz test — 3.3× faster, zero
  hot-path heap allocations (proven by a counting allocator).
- **Validated against real exchange data.** Decodes a full NASDAQ
  TotalView-ITCH trading day — **268.7M messages at 8.0M msg/s, zero framing
  errors** — and reconstructs the real AAPL book.
- **Full protocol stack, golden-tested.** OUCH 4.2 / SoupBinTCP and ITCH 5.0 /
  MoldUDP64, **encode and decode**, checked against hand-computed byte layouts
  with spec offsets.
- **Order types.** Limit, IOC, market, and minimum-quantity / fill-or-kill.
- **Latency measured honestly.** Open-loop, coordinated-omission-corrected
  benchmarks, plus an optional low-latency engine mode (lock-free SPSC ring,
  busy-poll, core-pin, `mlockall`).
- **Cross-platform and CI-gated.** One event-loop interface over kqueue
  (macOS) and epoll (Linux); the test suite is green on both, in Release and
  under ASan+UBSan and ThreadSanitizer.

## Contents

- [The matching engine](#the-matching-engine)
- [Protocol subset](#protocol-subset) · [Non-goals](#non-goals)
- [Build](#build) · [Run the market](#run-the-market)
- [Real market data](#real-market-data)
- [Measured performance](#measured-performance)
- [Testing strategy](#testing-strategy) · [Layout](#layout)

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

**Order types beyond plain limits** (driven by the OUCH `tif`, `min_qty`,
and `price` fields on Enter Order):

- **IOC** (`tif == 0`): match against whatever is available now and cancel
  the remainder — nothing rests, so an unfilled IOC never appears as an Add
  on the market-data feed.
- **Market** (`price == 0x7FFFFFFF`): fill against any resting price, walking
  the book; inherently IOC (cancel the remainder).
- **Minimum quantity / fill-or-kill**: if fewer than `min_qty` shares can
  execute at entry, the whole order is killed without touching the book;
  `min_qty == shares` on an IOC order is FOK.

The book gained two primitives for this: a read-only `matchable(side, limit)`
peek (how much would fill right now, used to gate min-qty orders before they
touch the book) and `execute_ioc(...)` (match without resting the remainder).

## Protocol subset

| Layer | Implemented |
|---|---|
| OUCH 4.2 in | Enter Order `O` (limit, IOC, market, min-qty/FOK), Cancel `X`, Replace `U` |
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

Requires CMake ≥ 3.24 and a C++20 compiler. Developed on macOS/arm64 with
Apple Clang and CI-tested on Linux with Clang. The gateway event loop runs
on **kqueue** (macOS/BSD) or **epoll** (Linux) behind a small `Poller`
interface (`src/gateway/poller.{hpp,cpp}`) — the only OS-specific event-loop
code; the rest of the gateway is written against that interface.

```sh
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j
ctest --test-dir build-rel
```

Sanitizer presets: `-DSANITIZE_ADDRESS=ON` (ASan+UBSan) or
`-DSANITIZE_THREAD=ON` (TSan). The suite is green on macOS (kqueue) and Linux
(epoll): 113 tests in Release; the allocation-counting test is release-only
(it replaces the global allocator, which the sanitizers already do), so 112
run under ASan+UBSan and TSan.

## Run the market

**Quick start:** `./run-demo.sh` builds if needed, starts the exchange and a
market simulator, and hands off to a live `itchview` ladder. Ctrl-C exits
and cleans up both background processes.

```sh
# 1. The exchange: OUCH gateway on TCP 26400, ITCH on 239.192.0.1:26000
./build-rel/exchanged --port 26400 --capture /tmp/session.cap
#    Low-latency variant (busy-poll lock-free engine; Linux adds core pin):
#    ./build-rel/exchanged --port 26400 --ll-mode --engine-core 2

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

## Real market data

The decoder is validated against real exchange data, not just this
simulator's own feed. NASDAQ publishes free full-day TotalView-ITCH 5.0
sample files (emi.nasdaq.com → ITCH → *Nasdaq ITCH*; ~3.5 GB gzipped per
day). `itchfile` streams them from stdin — the file is never decompressed
to disk:

```sh
curl -LO "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz"
gzcat 12302019.NASDAQ_ITCH50.gz | ./build-rel/itchfile - --symbol AAPL
```

Actual output for the full December 30, 2019 trading day (268.7M
messages, zero framing errors — skipped types are exactly the non-book
messages outside the documented subset: NOII, crosses, Reg SHO, MWCB, …):

```
itchfile: 268744780 messages in 33.6s (8.0M msgs/sec)
type        decoded      skipped
A         117145568            0
C             99917            0
D         114360997            0
E           5722824            0
F           1485888            0
H              8966            0
I                 0      4024315
J                 0           34
K                 0            3
L                 0       215161
P           1218602            0
Q                 0        17836
R              8906            0
S                 6            0
U          21639067            0
V                 0            1
X           2787676            0
Y                 0         9013
symbols tracked: 1   filtered adds: 117932712   executed shares: 711223674
```

The final ladder after the full day is empty — correctly: NASDAQ purges
the book at end of session (note deletes ≈ adds above). Capping mid-session
shows the real AAPL book (`--max-messages 150000000`):

```
AAPL (top 8):
  BID                    | ASK
  -----------------------+-----------------------
       200    291.1300   | 291.1400         206
       100    291.1100   | 291.1500         100
       210    291.1000   | 291.1600         372
       200    291.0900   | 291.1700         301
       400    291.0800   | 291.1800         400
       200    291.0700   | 291.1900         401
       266    291.0600   | 291.2000         400
       278    291.0500   | 291.2100         300
```

A penny-wide AAPL market at $291 — which is where AAPL actually traded
that day.

## Measured performance

Apple M-series (8P+4E), Release build (`./build-rel/bench`):

```
book: 2M mixed ops (55% add / 25% cancel / 20% replace)
  reference               6.25M ops/sec
  fastbook               20.49M ops/sec  (3.3x)
  fastbook per-op         p50=44ns   p90=84ns   p99=168ns   p99.9=304ns

queue: cross-thread hop latency (200k round trips)
  mutex MpscQueue         p50=38.9µs p99=246µs  p99.9=377µs
  lock-free SpscRing      p50=128ns  p99=152ns  p99.9=216ns   (~300x p50)
```

**Measuring latency honestly.** The end-to-end bench is **open-loop and
coordinated-omission-corrected**: orders are emitted on a fixed schedule at a
target rate and each order's latency is measured from its *intended* send
time, not from when we got around to sending it. A closed-loop "send, wait
for ack, repeat" harness (the common mistake) stops the clock whenever the
system stalls, so its tail is a fiction. Under a 50k orders/sec offered load,
`enter -> Accepted` over real TCP loopback:

```
                          p50     p99     p99.9    p99.99
  blocking engine         55µs    90µs    119µs    279µs
  busy-poll engine (LL)   48µs    80µs    110µs    250µs
```

**Low-latency mode** (`exchanged --ll-mode`, or the busy-poll leg above)
swaps the gateway→engine hop from the blocking mutex queue to the lock-free
`SpscRing`, which the engine **busy-polls** — no condition-variable wakeup, and
no lock for the spinning consumer to contend with the producer over. On Linux
it also pins the engine thread (`--engine-core N`) and `mlockall`s the process
to keep it off the scheduler and out of the page-fault path.

The isolated hop is ~300x faster (above); end-to-end the gain is ~10–15%
across p50–p99.9, because over loopback the two TCP traversals dominate and
the internal hop is only one serialized cost. The outer tail (p99.99+) is
kernel/socket-scheduling jitter, which core isolation on dedicated hardware
addresses but a shared laptop cannot. Correct call for a *simulator*: the
default stays blocking (simple shutdown, polite CPU); LL mode is one flag away
when the queue is the bottleneck, and now it's wired and measured, not
promised.

**Microarchitecture (`perf`).** Throughput is a consequence of cache and
branch behaviour, so the hot path is measured there too. `bench perf` is an
isolated FastBook add/cancel/replace/match loop (~30M ops, no I/O or timing in
the measured region, allocation-free at steady state), and
`scripts/perf-engine.sh` wraps it in `perf stat` pinned to one core:

```sh
scripts/perf-engine.sh                 # Linux, bare metal
```

It reports IPC (instructions/cycle), last-level and L1-D cache-miss rates, and
branch-misprediction rate — the numbers that *explain* the ~21M ops/sec rather
than just restating it, and the ones a reviewer uses to sanity-check that a
contiguous ladder + pooled orders really is cache-friendly. Run it on real
hardware: cloud VMs and CI runners don't expose the PMU, so the hardware
counters read `<not supported>` there.

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
src/engine/   matching service          src/gateway/  kqueue/epoll TCP gateway
src/feed/     ITCH publisher + capture  src/client/   book builder, OUCH client
src/apps/     exchanged, marketsim, flowgen, itchview, itchlisten, itchreplay, bench, demo
tests/        113 tests (GoogleTest)
scripts/      perf-engine.sh (perf-stat hot-loop profiler)
docs/         design document
```

## License

MIT — see [LICENSE](LICENSE).

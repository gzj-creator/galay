# Plain Redis Fast Path Design

Date: 2026-03-24

## Goal

Improve `galay-redis` plain transport throughput in the two workloads that still trail mainstream Rust clients:

- `plain normal`
- `plain pipeline`

The work should preserve the existing public API and should not change TLS behavior in this round.

## Current Context

Recent same-machine baseline results show:

- `plain normal`: `galay-redis` `66.2k ops/s`, `redis-rs` `85.3k`, `fred` `84.2k`
- `plain pipeline`: `galay-redis` `1.04M ops/s`, `redis-rs` `1.47M`, `fred` `666k`

This indicates:

1. `plain normal` still has meaningful per-request overhead.
2. `plain pipeline` is already good, but there is still room to reduce extra encode/copy overhead.
3. TLS is no longer the primary weakness, so this round should avoid perturbing TLS internals.

## Explored Approaches

### Option 1: Internal borrowed-encoded fast path

Add a plain-only internal entry point that sends already-encoded RESP bytes by borrowing a `std::string_view` instead of always copying into an owning `std::string`.

Pros:

- Directly removes avoidable copy/allocation from the current plain exchange path.
- Helps both `plain normal` and `plain pipeline`.
- Keeps the current state machine model and parser behavior.
- Leaves public semantics unchanged.

Cons:

- Requires careful lifetime rules for borrowed command bytes.
- Needs benchmark call sites to opt into the new path explicitly.

### Option 2: Builder template/patch model

Teach `RedisCommandBuilder` to prebuild command templates and patch key/value slots in place.

Pros:

- Reduces encode cost further than simple borrowing.

Cons:

- Higher complexity and much more fragile invariants.
- Harder to validate quickly.
- Does not directly solve all extra copying in the exchange path.

### Option 3: Fixed-flow specialized machine

Introduce special-purpose plain fast machines for common fixed flows such as one-command/one-reply or benchmark-only `SET`/`GET`.

Pros:

- Highest upside if the generic machine is the dominant cost.

Cons:

- Highest implementation cost and regression risk.
- Easy to overfit to benchmark code.

## Chosen Approach

Choose Option 1 for this round.

Specifically:

1. Keep the current public `command` and `batch` APIs unchanged.
2. Add internal plain fast-path entry points that can borrow encoded bytes for the duration of one await cycle.
3. Use those fast paths only from benchmark/internal call sites first.
4. Do not change TLS send/recv machinery in this round.

## Architecture

### 1. Borrowed encoded command packets

Introduce a plain-only internal packet type alongside `RedisEncodedCommand`, for example:

- `RedisBorrowedCommand`

This packet carries:

- `std::string_view encoded`
- `size_t expected_replies`

It does not own storage.

### 2. Redis exchange state supports owned or borrowed bytes

Update `detail::RedisExchangeSharedState` in `galay-redis/async/redis_client.h` and `galay-redis/async/redis_client.cc` so the send path can read from either:

- an owned `std::string`
- a borrowed `std::string_view`

The machine should always send from a single read-only view, so the rest of the send logic stays uniform.

Important rule:

- Existing public APIs continue to use owned storage.
- New internal fast-path APIs may use borrowed storage.

### 3. New internal RedisClient entry points

Add explicit internal methods for the plain client only, such as:

- `RedisExchangeOperation commandBorrowed(RedisBorrowedCommand packet);`
- `RedisExchangeOperation batchBorrowed(std::string_view encoded, size_t expected_replies);`

These methods are intentionally lower-level than the public `command`/`batch` APIs and are only meant for trusted internal callers such as benchmarks.

### 4. Benchmark integration

Update `benchmark/b1_client.cc`:

- `plain normal`
  - pre-encode the `SET` and `GET` commands per loop iteration
  - call the borrowed command fast path directly
- `plain pipeline`
  - continue using `RedisCommandBuilder::append`
  - send `builder.encoded()` via the borrowed batch fast path
  - avoid re-copying the entire already-encoded pipeline payload into exchange state

TLS benchmark code remains unchanged in this round.

## Validation Plan

### Functional

Add targeted tests for:

1. Surface/API availability of the borrowed plain fast path
2. Local Redis smoke flow using borrowed `SET/GET`
3. Local Redis pipeline smoke flow using borrowed encoded batch bytes
4. Existing owning `command`/`batch` paths still compile and behave normally

### Performance

Re-run at minimum:

```bash
./build-ssl-probe/benchmark/b1_client --clients 10 --operations 5000 --mode normal -q
./build-ssl-probe/benchmark/b1_client --clients 10 --operations 5000 --mode pipeline --batch-size 50 -q
```

Then re-run the aligned Rust comparison to confirm the plain gaps shrink while TLS remains stable.

## Scope Boundaries

Included:

- plain internal fast path
- benchmark opt-in to that path
- regression tests and benchmark verification

Excluded:

- TLS fast-path rewrite
- RESP parser redesign
- generic template-patching encoder system
- benchmark-only protocol shortcuts that would diverge from library behavior

## Risks

### Borrowed lifetime misuse

If borrowed command bytes outlive the source storage, send behavior becomes unsafe.

Mitigation:

- keep the borrowed path internal and explicit
- only use it with storage that remains alive until `co_await` completes
- add focused local smoke tests

### Overfitting to benchmark code

If the optimization only helps one benchmark shape, the library becomes harder to maintain without broad value.

Mitigation:

- optimize at the exchange layer, not in a special benchmark-only transport
- keep public owning path intact

### TLS regression through accidental code sharing

Mitigation:

- leave `RedissClient` and SSL state-machine code untouched in this round

## Success Criteria

This design is successful if:

1. `plain normal` materially improves versus the current `66.2k ops/s` baseline.
2. `plain pipeline` improves versus the current `1.04M ops/s` baseline.
3. Existing public APIs remain source-compatible.
4. TLS benchmark results do not regress as a side effect.

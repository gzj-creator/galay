# LRU Cache Design

## Goal

Add a reusable, extensible LRU cache utility for galay-utils.

## Scope

The cache is a C++23 header-only template under `galay-utils/cache/lru_cache.hpp`.
It is exported through `galay-utils/galay_utils.hpp` and the C++ module facade.

## Thread Safety

The cache is intentionally not thread-safe and does not use `std::mutex`.
Concurrent access from multiple threads or coroutines must be synchronized by the caller.
This keeps the class usable in coroutine paths without hidden blocking.

## Eviction

The cache supports two lazy eviction policies:

- Capacity eviction: when write-like APIs are called, the cache removes least-recently-used entries until `size() <= capacity()`.
- Time eviction: when cache APIs are called, expired entries are removed before the requested operation observes them.

There is no background thread, timer, condition variable, or internal lock.

## Data Structures

The cache uses:

- `std::list<Entry>` to maintain recency order, with the most recently used entry at the front.
- `std::unordered_map<Key, ListIterator, Hash, KeyEqual>` for O(1) key lookup.
- `std::priority_queue<ExpireNode, ..., Compare>` as a min-heap of expiration candidates.

The expiration heap uses lazy invalidation. Each heap node stores `expiresAt`, `key`, and `version`.
The live entry also stores `expiresAt` and `version`; stale heap nodes are discarded when popped.

## Public API

Core operations:

- `put(key, value)` and overloads with per-entry TTL.
- `emplace(key, args...)` and overloads with per-entry TTL.
- `get(key)` returning `Value*` or `const Value*`.
- `contains(key)`, `remove(key)`, `clear()`.
- `size()`, `empty()`, `capacity()`, `setCapacity(capacity)`.
- Optional default TTL and eviction callback support.

The value pointer returned by `get` remains valid until the cache is mutated in a way that erases or moves that entry.

## Testing

Add focused assertions to `test/cache/cache_test.cpp`:

- capacity-based LRU eviction
- `get` refreshes recency
- updating an existing key refreshes value and recency
- expired entries are lazily removed
- stale expiration heap entries do not remove updated keys
- zero capacity keeps the cache empty

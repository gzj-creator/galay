# Bytes Migration Design

## Context

`galay-kernel` currently owns a move-only `Bytes` container in `galay-kernel/common/bytes.{h,cc}`. The container wraps a raw metadata structure named `StringMetaData` and supports both owning byte storage and non-owning views. `galay-utils` already hosts reusable cache and buffer utilities under `galay-utils/cache/`, exports them through `galay-utils/galay_utils.hpp`, and keeps the library target header-only.

The migration should move this reusable byte container into `galay-utils`, rename the metadata type to `ByteMetaData`, and then let `galay-kernel` remove its local implementation and consume the shared utility.

## Goals

- Add `Bytes` to `galay-utils` as a reusable public cache/buffer utility.
- Rename the metadata type from `StringMetaData` to `ByteMetaData`.
- Preserve move-only ownership semantics and non-owning view construction.
- Keep `galay-utils` header-only.
- Export the new API through the umbrella header and C++ module facade.
- Add focused tests and documentation for the migrated API.
- Prepare `galay-kernel` to delete its local `Bytes` implementation and include the `galay-utils` API.

## Non-Goals

- Do not change `galay-utils` from an interface/header-only target into a compiled library.
- Do not preserve `StringMetaData` as a public compatibility alias in `galay-utils`.
- Do not broaden `Bytes` into a general mutable vector-like container beyond the existing kernel behavior.

## Recommended Approach

Create `galay-utils/cache/bytes.hpp` with inline definitions for:

- `struct ByteMetaData`
- `mallocBytes(size_t)`
- `deepCopyBytes(const ByteMetaData&)`
- `reallocBytes(ByteMetaData&, size_t)`
- `clearBytes(ByteMetaData&)`
- `freeBytes(ByteMetaData&)`
- `class Bytes`

`Bytes` will live in `namespace galay::utils`, remain move-only, and support:

- owning deep-copy construction from `std::string`, `const char*`, `const uint8_t*`, and explicit pointer/length pairs
- owning capacity construction with size set to zero
- non-owning `fromString(...)` and `fromCString(...)` views
- `data()`, `c_str()`, `size()`, `capacity()`, `empty()`, `clear()`, `toString()`, `toStringView()`, equality and inequality

The implementation will keep raw allocation behavior close to the kernel source but harden obvious edge cases where the current code can write a null terminator past capacity. `c_str()` should only write a terminator when `capacity() > size()`.

## Public Surface

`galay-utils` will expose the new header through:

- `galay-utils/cache/bytes.hpp`
- `galay-utils/galay_utils.hpp`
- `galay-utils/module/galay_utils.cppm`

The docs will list `Bytes` and `ByteMetaData` in the cache/buffer section with their ownership and lifetime rules.

## Kernel Follow-Up

After the utils migration passes, `galay-kernel` should:

- remove the local `common/bytes.cc` implementation from its build
- replace local `StringMetaData` usage with `galay::utils::ByteMetaData`
- replace local `Bytes` usage with `galay::utils::Bytes`
- include `galay-utils/cache/bytes.hpp` where the local bytes header was previously used

If an old kernel include path is required by existing kernel code during the transition, a minimal forwarding header can be considered, but the target state is no local `Bytes` implementation in kernel.

## Testing

Add `buffer_test` coverage before production code:

- `Bytes` owning constructors deep-copy input
- non-owning views observe caller-owned storage and do not free it
- move construction and move assignment leave the source empty
- `clear()` resets owned and non-owned values safely
- `toString()`, `toStringView()`, `operator==`, and `operator!=` match byte content
- `ByteMetaData` allocation, deep copy, reallocation, clear, and free helpers maintain size/capacity/data invariants

Verification commands:

```bash
rtk cmake --build cmake-build-test --target buffer_test
rtk ctest --test-dir cmake-build-test -R '^buffer_test$' --output-on-failure
rtk cmake --build cmake-build-test --parallel
rtk ctest --test-dir cmake-build-test --output-on-failure
```

## Risks

- `galay-utils` is header-only, so moving kernel `.cc` implementation directly would create unresolved symbols. The migration must inline the implementation.
- `Bytes::c_str()` has subtle capacity requirements. It must not write a terminator when no spare byte exists.
- Kernel removal may expose existing dependencies on the old `galay::kernel` namespace or `StringMetaData` name. Those should be updated explicitly rather than hidden behind broad aliases.

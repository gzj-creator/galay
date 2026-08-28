# Galay vendored concurrentqueue

This directory vendors the header-only [concurrentqueue] library by Cameron
Desrochers. The upstream headers are distributed under the Simplified BSD
License and the Boost Software License; the complete upstream license text is
kept in [`LICENSE.md`](LICENSE.md). The semaphore implementation embedded in
`lightweightsemaphore.h` also retains its original zlib license notice.

## Why this copy exists

Galay exports C++23 named modules. GCC 16 diagnoses several namespace-scope
`static inline` helpers in the upstream header as TU-local entities when those
helpers are referenced by exported queue templates. The affected helpers are:

- `moodycamel::details::likely` and `unlikely`
- `moodycamel::details::hash_thread_id`
- `moodycamel::details::circular_less_than`
- `moodycamel::details::align_for`
- `moodycamel::details::ceil_to_pow_2`
- `moodycamel::details::swap_relaxed`
- `moodycamel::details::nomove`
- `moodycamel::details::deref_noexcept`
- the `invalid_thread_id` definition on the affected platform branch

Only the linkage spelling of those helpers is changed from internal linkage to
ordinary `inline` linkage. No queue algorithm, layout, synchronization rule,
or public API is changed. The source is therefore source-compatible with the
upstream header while being consumable from Galay's GCC 16 module interfaces.

The vendored copy is installed below `include/galay/thirdparty` and is used by
Galay's own headers and CMake targets. Galay no longer searches for or requires
a separately installed `concurrentqueue` package.

When updating this copy, preserve the upstream copyright, license text, and
the local adaptation note in `concurrentqueue.h`; record any additional local
changes here as well.

[concurrentqueue]: https://github.com/cameron314/concurrentqueue

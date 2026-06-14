# Galay-RPC Catch-Up Design

**Goal:** Bring `galay-rpc` up to date with the latest `galay-kernel`, refresh same-machine Rust benchmark comparisons, and release a new tag with only C++-side fixes and version metadata.

**Scope:**
- Keep and finish the existing C++ adaptation work already present in the working tree.
- Rebuild and rerun targeted regression tests and benchmark targets.
- Re-run same-machine Rust baseline comparisons for the public RPC benchmark paths.
- Commit only C++ fixes, tests, benchmark harness changes, and version metadata.

**Out of Scope:**
- Do not submit Rust baseline source changes, benchmark raw outputs, or Rust-only docs.
- Do not expand `galay-rpc` with unrelated new RPC features in this pass.

**Current State:**
- The repository already has local changes in `benchmark/`, `galay-rpc/kernel/rpc_stream.h`, and `test/t1_proto.cpp`.
- The visible CMake version is stale at `1.0.0`, while the latest git tag is already `v1.0.2`.
- Rust comparison assets exist locally under `benchmark/compare/` and `scripts/s3_bench_rust_compare.sh`, but those assets should remain uncommitted unless a C++-side script fix is required.

**Recommended Approach:**
1. Audit the current diff and separate releasable C++ changes from non-releasable Rust comparison assets and documentation churn.
2. Verify the latest `galay-kernel` adaptation by rebuilding the affected tests and benchmark targets.
3. Run fresh Rust comparison benchmarks on the same machine and require `galay-rpc` to at least catch up before release.
4. Bump the CMake version, commit the releasable subset, and create the next annotated tag only after fresh evidence is collected.

**Verification Strategy:**
- Build the narrowest affected test and benchmark targets first.
- Re-run relevant protocol/regression tests tied to the touched files.
- Re-run the Rust comparison benchmark script or equivalent same-machine commands and capture the headline throughput numbers needed for the release decision.

**Release Policy:**
- If the fresh Rust comparison does not show `galay-rpc` catching up on the intended public paths, stop and fix the bottleneck before tagging.
- If verification passes, align the new git tag with the updated CMake version and release commit.

# Galay-RPC Catch-Up Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Adapt `galay-rpc` to the latest local dependency stack, re-run same-machine Rust comparisons, and release a new tag with only verified C++-side fixes.

**Architecture:** Reuse the current in-flight `galay-rpc` changes instead of widening scope. First classify the working tree into releasable C++ changes versus Rust comparison assets, then rebuild and verify only the affected protocol, stream, and benchmark paths. Release only after fresh benchmark evidence and version metadata are aligned.

**Tech Stack:** CMake, C++23, galay-kernel, galay-rpc benchmark targets, local Rust tonic baseline tooling, git tags

---

### Task 1: Audit releasable versus non-releasable changes

**Files:**
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/CMakeLists.txt`
- Inspect: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/benchmark/b1_rpc.cpp`
- Inspect: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/benchmark/b2_rpc.cpp`
- Inspect: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/benchmark/b3_discovery.cpp`
- Inspect: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/benchmark/b4_stream.cpp`
- Inspect: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/benchmark/b5_stream.cpp`
- Inspect: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/galay-rpc/kernel/rpc_stream.h`
- Inspect: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/test/t1_proto.cpp`

**Step 1: Review git status and current diffs**

Run: `git -C /Users/gongzhijie/Desktop/projects/git/galay-rpc status --short && git -C /Users/gongzhijie/Desktop/projects/git/galay-rpc diff --stat`
Expected: See the exact modified C++ files, docs, Rust compare assets, and build artifacts.

**Step 2: Decide the releasable file set**

Keep only C++ adaptation, test, benchmark, and version files for release. Exclude Rust comparison trees, raw outputs, build directories, and Rust-only docs unless a committed C++ script requires a minimal repo-side fix.

**Step 3: Bump visible version metadata**

Update `/Users/gongzhijie/Desktop/projects/git/galay-rpc/CMakeLists.txt` so the project version matches the intended new release tag.

**Step 4: Re-check the narrowed diff**

Run: `git -C /Users/gongzhijie/Desktop/projects/git/galay-rpc diff -- <releasable-files>`
Expected: Only the intended C++-side release diff remains in scope.

### Task 2: Rebuild and verify affected regression paths

**Files:**
- Modify if needed: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/test/t1_proto.cpp`
- Modify if needed: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/galay-rpc/kernel/rpc_stream.h`

**Step 1: Configure a fresh verification build**

Run: `cmake -S /Users/gongzhijie/Desktop/projects/git/galay-rpc -B /Users/gongzhijie/Desktop/projects/git/galay-rpc/build-codex-submitcheck -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DBUILD_BENCHMARKS=ON`
Expected: Configure completes successfully against the local installed `galay-kernel`.

**Step 2: Build the affected targets**

Run: `cmake --build /Users/gongzhijie/Desktop/projects/git/galay-rpc/build-codex-submitcheck --target t1_proto b1_rpc b2_rpc b3_discovery b4_stream b5_stream -j8`
Expected: All targeted binaries link successfully.

**Step 3: Run the protocol regression**

Run: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/build-codex-submitcheck/test/t1_proto`
Expected: PASS or equivalent zero-exit test completion.

**Step 4: If the regression fails, fix the minimal root cause**

Touch only the owning protocol/stream files. Re-run the same failing test before expanding validation.

### Task 3: Re-run same-machine Rust comparison benchmarks

**Files:**
- Modify if needed: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/benchmark/b1_rpc.cpp`
- Modify if needed: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/benchmark/b2_rpc.cpp`
- Modify if needed: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/benchmark/b4_stream.cpp`
- Modify if needed: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/benchmark/b5_stream.cpp`
- Inspect only: `/Users/gongzhijie/Desktop/projects/git/galay-rpc/scripts/s3_bench_rust_compare.sh`

**Step 1: Run the public Rust comparison path**

Run: `env ... /Users/gongzhijie/Desktop/projects/git/galay-rpc/scripts/s3_bench_rust_compare.sh /Users/gongzhijie/Desktop/projects/git/galay-rpc/build-codex-submitcheck`
Expected: Fresh same-machine C++ versus Rust benchmark output for the public RPC modes.

**Step 2: Compare the headline metrics**

Require `galay-rpc` to at least catch up on the intended public release benchmarks before tagging. If not, stop and inspect the C++ benchmark implementation and runtime settings.

**Step 3: Re-run any benchmark after the smallest fix**

Use the narrowest benchmark rerun that proves the improvement before re-running the whole comparison.

### Task 4: Stage, commit, and tag only after fresh evidence

**Files:**
- Stage only: releasable C++ files and version metadata

**Step 1: Stage the narrowed release set**

Run: `git -C /Users/gongzhijie/Desktop/projects/git/galay-rpc add <releasable-files>`
Expected: No Rust compare source, build output, or unrelated docs are staged.

**Step 2: Review the staged diff**

Run: `git -C /Users/gongzhijie/Desktop/projects/git/galay-rpc diff --cached --stat && git -C /Users/gongzhijie/Desktop/projects/git/galay-rpc diff --cached`
Expected: The staged release matches the verified C++ changes.

**Step 3: Commit with a detailed Chinese message**

Run: `git -C /Users/gongzhijie/Desktop/projects/git/galay-rpc commit -m "<detailed Chinese subject>" -m "<detail 1>" -m "<detail 2>"`
Expected: A clean release commit is created.

**Step 4: Create the annotated release tag**

Run: `git -C /Users/gongzhijie/Desktop/projects/git/galay-rpc tag -a <new-tag> -m "galay-rpc <new-tag>"`
Expected: The new tag points at the verified release commit.

**Step 5: Confirm post-release state**

Run: `git -C /Users/gongzhijie/Desktop/projects/git/galay-rpc rev-parse HEAD && git -C /Users/gongzhijie/Desktop/projects/git/galay-rpc rev-list -n 1 <new-tag> && git -C /Users/gongzhijie/Desktop/projects/git/galay-rpc status --short`
Expected: Tag and HEAD match; only intentionally uncommitted Rust/docs/build artifacts remain.

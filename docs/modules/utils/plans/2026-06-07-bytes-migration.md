# Bytes Migration Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Move the `Bytes` container from `galay-kernel` into `galay-utils`, rename the metadata type to `ByteMetaData`, and then remove the local kernel implementation.

**Architecture:** Add a header-only `galay-utils/cache/bytes.hpp` that owns the public `ByteMetaData` and `Bytes` API under `galay::utils`. Export it through the utils umbrella header and module facade, then update `galay-kernel` to consume that header and stop building its own `common/bytes.cc`.

**Tech Stack:** C++23, CMake interface library, CTest, header-only public utility APIs, sibling repositories under `/Users/gongzhijie/Desktop/projects/git`.

---

### Task 1: Add Failing Utils Tests

**Files:**
- Modify: `test/buffer/buffer_test.cpp`

**Step 1: Write the failing tests**

Add these tests before `main()`:

```cpp
void testByteMetaDataHelpers() {
    ByteMetaData meta = mallocBytes(8);
    assert(meta.data != nullptr);
    assert(meta.size == 0);
    assert(meta.capacity == 8);

    std::memcpy(meta.data, "abcd", 4);
    meta.size = 4;

    ByteMetaData copy = deepCopyBytes(meta);
    assert(copy.data != nullptr);
    assert(copy.data != meta.data);
    assert(copy.size == 4);
    assert(copy.capacity == 8);
    assert(std::memcmp(copy.data, "abcd", 4) == 0);

    reallocBytes(copy, 2);
    assert(copy.size == 2);
    assert(copy.capacity == 2);
    assert(std::memcmp(copy.data, "ab", 2) == 0);

    clearBytes(copy);
    assert(copy.data != nullptr);
    assert(copy.size == 0);
    assert(copy.capacity == 2);

    freeBytes(copy);
    assert(copy.data == nullptr);
    assert(copy.size == 0);
    assert(copy.capacity == 0);

    freeBytes(meta);
}

void testBytesContainer() {
    std::string source = "hello";
    Bytes owned(source);
    source[0] = 'H';
    assert(owned.toString() == "hello");
    assert(owned.size() == 5);
    assert(owned.capacity() == 5);
    assert(!owned.empty());

    Bytes literal("hello", 5);
    assert(owned == literal);
    assert(!(owned != literal));

    std::string viewSource = "view";
    Bytes view = Bytes::fromString(viewSource);
    assert(view.toStringView() == "view");
    viewSource[0] = 'V';
    assert(view.toStringView() == "View");

    Bytes raw = Bytes::fromCString("abcdef", 3, 6);
    assert(raw.toString() == "abc");
    assert(raw.capacity() == 6);

    Bytes moved(std::move(owned));
    assert(moved.toString() == "hello");
    assert(owned.empty());
    assert(owned.data() == nullptr);

    Bytes assigned(4);
    assigned = std::move(moved);
    assert(assigned.toString() == "hello");
    assert(moved.empty());

    assigned.clear();
    assert(assigned.empty());
    assert(assigned.data() == nullptr);
}
```

Call both tests from `main()` before the existing `ByteQueueView` test:

```cpp
testByteMetaDataHelpers();
testBytesContainer();
```

**Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build cmake-build-test --target buffer_test
```

Expected: FAIL because `ByteMetaData`, `mallocBytes`, `Bytes`, and related APIs are not declared.

### Task 2: Implement Header-Only Bytes in Utils

**Files:**
- Create: `galay-utils/cache/bytes.hpp`

**Step 1: Write minimal implementation**

Create `galay-utils/cache/bytes.hpp` with:

- include guard `GALAY_UTILS_CACHE_BYTES_HPP`
- includes: `<algorithm>`, `<cstddef>`, `<cstdint>`, `<cstdlib>`, `<cstring>`, `<new>`, `<stdexcept>`, `<string>`, `<string_view>`
- namespace `galay::utils`
- `struct ByteMetaData`
- inline helper functions `mallocBytes`, `deepCopyBytes`, `reallocBytes`, `clearBytes`, `freeBytes`
- move-only `class Bytes`

Implementation requirements:

- `ByteMetaData` constructors create non-owning views and never free memory in the destructor.
- `mallocBytes(0)` returns `{nullptr, 0, 0}`.
- `deepCopyBytes` allocates `meta.capacity`, copies `meta.size`, and handles empty/null metadata.
- `reallocBytes(meta, 0)` frees and resets.
- `reallocBytes` preserves existing bytes up to the new size, truncating `size` when needed.
- `clearBytes` zeroes existing storage only when `data != nullptr && capacity > 0`, then sets `size = 0`.
- `freeBytes` frees and resets.
- `Bytes` constructors deep-copy input data unless using `fromString` / `fromCString`.
- `Bytes` is move-only.
- `Bytes::c_str()` writes a null terminator only when `data != nullptr`, `size < capacity`, and the last readable byte is not already null.
- `Bytes::clear()` frees only owned storage and always resets to empty.

**Step 2: Run buffer target**

Run:

```bash
rtk cmake --build cmake-build-test --target buffer_test
```

Expected: PASS build.

**Step 3: Run focused buffer test**

Run:

```bash
rtk ctest --test-dir cmake-build-test -R '^buffer_test$' --output-on-failure
```

Expected: PASS.

### Task 3: Export Bytes from Utils

**Files:**
- Modify: `galay-utils/galay_utils.hpp`
- Modify: `galay-utils/module/galay_utils.cppm`
- Modify: `test/import_smoke.cpp`

**Step 1: Update exports**

Add:

```cpp
#include "galay-utils/cache/bytes.hpp"
```

near the other cache includes in both the umbrella header and module facade.

**Step 2: Extend module smoke test**

In `test/import_smoke.cpp`, after the existing `ByteQueueView` or `RingBuffer` checks, add:

```cpp
Bytes bytes("mod", 3);
assert(bytes.toStringView() == "mod");
```

**Step 3: Run direct and module-related checks**

Run:

```bash
rtk cmake --build cmake-build-test --target buffer_test
rtk ctest --test-dir cmake-build-test -R '^buffer_test$' --output-on-failure
```

Expected: PASS.

If the current build tree has module tests enabled, also run:

```bash
rtk cmake --build cmake-build-test --target test_import_smoke
rtk ctest --test-dir cmake-build-test -R 'ModuleImportSmoke' --output-on-failure
```

Expected: PASS, or target unavailable when module tests are disabled.

### Task 4: Document Bytes in Utils

**Files:**
- Modify: `docs/00-快速开始.md`
- Modify: `docs/02-API参考.md`
- Modify: `docs/03-使用指南.md`
- Modify: `README.md`

**Step 1: Update docs**

Add `galay-utils/cache/bytes.hpp`, `Bytes`, and `ByteMetaData` to the cache/buffer utility lists.

Document these semantics:

- `Bytes` is move-only.
- Owning constructors deep-copy input bytes.
- `Bytes::fromString(...)` and `Bytes::fromCString(...)` are non-owning views and require caller-owned storage to outlive the `Bytes`.
- `ByteMetaData` is a low-level raw pointer/size/capacity descriptor.
- The API is not thread-safe; concurrent access needs external synchronization.

**Step 2: Run docs-adjacent compile checks**

Run:

```bash
rtk cmake --build cmake-build-test --target buffer_test
rtk ctest --test-dir cmake-build-test -R '^buffer_test$' --output-on-failure
```

Expected: PASS.

### Task 5: Full Utils Verification

**Files:**
- No edits unless verification exposes a bug.

**Step 1: Run full build**

Run:

```bash
rtk cmake --build cmake-build-test --parallel
```

Expected: PASS.

**Step 2: Run full CTest**

Run:

```bash
rtk ctest --test-dir cmake-build-test --output-on-failure
```

Expected: 100% tests passed.

**Step 3: Commit utils migration**

Run:

```bash
rtk git status --short
rtk git add galay-utils/cache/bytes.hpp galay-utils/galay_utils.hpp galay-utils/module/galay_utils.cppm test/buffer/buffer_test.cpp test/import_smoke.cpp README.md docs/00-快速开始.md docs/02-API参考.md docs/03-使用指南.md
rtk git commit -m "feat: 迁移 Bytes 容器到 utils"
```

Expected: commit includes only the utils migration and docs. Leave `.idea/` untracked.

### Task 6: Remove Kernel Local Bytes Implementation

**Files:**
- Workdir: `/Users/gongzhijie/Desktop/projects/git/galay-kernel`
- Modify: `galay-kernel/common/bytes.h`
- Delete or stop compiling: `galay-kernel/common/bytes.cc`
- Modify: `galay-kernel/common/buffer.h`
- Modify: `galay-kernel/common/buffer.cc`
- Modify: kernel CMake/Bazel files that list `common/bytes.cc`
- Modify: kernel docs/tests as required by compile failures

**Step 1: Inspect kernel build references**

Run from kernel repo:

```bash
rtk rg -n "common/bytes|bytes\\.cc|bytes\\.h|StringMetaData|\\bBytes\\b" CMakeLists.txt BUILD.bazel galay-kernel test docs README.md
```

Expected: identify exact include and build references.

**Step 2: Replace local implementation with utils API**

Target state:

- No compiled `galay-kernel/common/bytes.cc`.
- Kernel code includes `<galay-utils/cache/bytes.hpp>`.
- `StringMetaData` is renamed to `ByteMetaData` in kernel-owned buffer code where it still describes byte metadata.
- `Bytes` use sites refer to `galay::utils::Bytes` directly, or use a narrow `using galay::utils::Bytes;` only inside kernel namespace if needed to avoid broad aliases.

**Step 3: Run kernel focused compile/test**

Use the kernel repo's documented test build directory. If an existing test build is present, prefer it. Otherwise configure a test build with kernel docs defaults.

Run likely commands:

```bash
rtk cmake --build cmake-build-test --parallel
rtk ctest --test-dir cmake-build-test --output-on-failure
```

Expected: kernel builds and relevant tests pass. If the whole kernel suite is too broad or environment-dependent, run the narrow tests that compile `Bytes`, buffer, I/O vector, TCP/UDP, and public header smoke coverage, then report the limitation.

**Step 4: Commit kernel removal**

Run from kernel repo:

```bash
rtk git status --short
rtk git add <exact changed kernel files>
rtk git commit -m "refactor: 删除 kernel 本地 Bytes 实现"
```

Expected: commit removes local implementation and records the dependency on utils.

### Task 7: Final Cross-Repo Verification

**Files:**
- No edits unless verification exposes a bug.

**Step 1: Verify utils status**

Run from utils repo:

```bash
rtk git status --short
rtk cmake --build cmake-build-test --parallel
rtk ctest --test-dir cmake-build-test --output-on-failure
```

Expected: clean except pre-existing `.idea/`; full utils tests pass.

**Step 2: Verify kernel status**

Run from kernel repo:

```bash
rtk git status --short
rtk cmake --build cmake-build-test --parallel
rtk ctest --test-dir cmake-build-test --output-on-failure
```

Expected: clean kernel worktree after commit; full kernel tests pass or documented environment limitation.

**Step 3: Summarize**

Report:

- utils commit hash
- kernel commit hash
- exact verification commands and pass/fail counts
- any remaining `.idea/` or unrelated untracked files excluded from commits

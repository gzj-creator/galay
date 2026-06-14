# Plugin Lifecycle Refactor Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 accept plugin 从 `std::function` hook 改造成由 server 托管生命周期的插件类，并在 server start/stop 时统一启动和停止插件。

**Architecture:** `plugin/common/defn.h` 提供 `AcceptPlugin<SocketType>` 虚基类，包含 `start(Runtime&)`、`stop()` 和每次 accept 执行的 `handle(Runtime&, SocketType&, const Host&)`。HTTP/HTTPS/H2C/H2 server 持有 `std::unique_ptr<AcceptPlugin<SocketType>>`，启动 runtime 后、启动 accept loop 前按注册顺序调用 `start()`，停止时按反序调用 `stop()`。本次不保留旧 lambda / `std::function` API，也不保留旧的未类型化 context 临时上下文；`BlackList` 改为直接继承插件基类，配置归实例所有，但 `ConnInfoStorage` 与 `AsyncMutex` 仍按 `SocketType` 静态共享，保证所有 `BlackList<SocketType>` 访问同一份连接统计和同一把锁。

**Tech Stack:** C++23, CMake, CTest, `galay-kernel 5.1.1`, `galay-http` header-only public plugin API

---

## Design Decisions

- Public API: `server.addAcceptPlugin(std::unique_ptr<plugin::AcceptPlugin<SocketType>> plugin)`.
- Lifecycle API:
  - `virtual bool start(galay::kernel::Runtime& runtime) { return true; }`
  - `virtual void stop() noexcept {}`
  - `virtual galay::kernel::Task<bool> handle(galay::kernel::Runtime&, SocketType&, const galay::kernel::Host&) = 0`
- Ownership: server owns plugin instances. A plugin instance must not be registered into multiple servers.
- State model: ordinary plugin-owned state belongs in plugin members and must be concurrency-safe; shared policy state can remain static when the feature explicitly needs cross-instance aggregation. Per-accept temporary state should be local variables inside `handle()`. If cross-plugin per-accept state is needed later, add a typed `AcceptContext` instead of reintroducing raw untyped context.
- BlackList shared state: `BlackListConfig` is instance-owned, while `ConnInfoStorage` and `AsyncMutex` remain `static` members per `SocketType`. This preserves global blacklist accounting across all `BlackList<TcpSocket>` instances and all servers using that socket type.
- Ordering:
  - `start()` is called in registration order.
  - `handle()` is called in registration order for each accepted socket.
  - `stop()` is called in reverse registration order.
- Failure behavior:
  - `addAcceptPlugin(nullptr)` returns `false`.
  - `addAcceptPlugin(...)` after server start returns `false`.
  - If any plugin `start()` returns `false` or throws, server start fails, already-started plugins are stopped in reverse order, and runtime is stopped.
  - If `handle()` returns `false`, later plugins and business handler are skipped for that connection.
  - If `co_await handle()` fails at the kernel task layer, log and treat it as `false`.
- No legacy compatibility: remove `AcceptPlugin = std::function<...>` and all `BlackList::createPlugin(...)` entrypoints.

## Pre-Flight

**Files:**
- Inspect: `galay-http/plugin/common/defn.h`
- Inspect: `galay-http/plugin/blacklist/blacklist.hpp`
- Inspect: `galay-http/server/http/http_server.h`
- Inspect: `galay-http/server/http2/http2_server.h`
- Inspect: `test/t81_accepthook.cc`
- Inspect: `test/t83_blacklist.cc`
- Inspect: `test/t84_h2acceptplugin.cc`

**Step 1: Confirm current branch baseline**

Run:

```bash
rtk proxy git status --short
rtk rg -n "using AcceptPlugin|createPlugin|addAcceptPlugin|m_accept_handlers" galay-http test examples docs README.md CHANGELOG.md
```

Expected:
- Worktree may already contain unrelated/kernel-upgrade changes; do not revert them.
- Current accept plugin API is still lambda/function based before this refactor.

---

### Task 1: Replace AcceptPlugin Alias With Lifecycle Base Class

**Files:**
- Modify: `galay-http/plugin/common/defn.h`
- Test: `test/t81_accepthook.cc`

**Step 1: Write the failing compile-oriented lifecycle test changes**

In `test/t81_accepthook.cc`, replace local lambda accept hooks with concrete plugin classes. Keep the response, ordering, socket-validity, and business-handler assertions; remove the old `HookPayload`, `empty_context_count`, `second_saw_first`, and `context_destroyed_count` checks because the new plugin API intentionally has no per-accept untyped context. Add lifecycle counters:

```cpp
class FirstPlugin final : public plugin::AcceptPlugin<TcpSocket> {
public:
    explicit FirstPlugin(TestState* state) : m_state(state) {}

    bool start(Runtime&) override {
        m_state->first_start_count.fetch_add(1);
        return true;
    }

    void stop() noexcept override {
        m_state->first_stop_count.fetch_add(1);
    }

    Task<bool> handle(Runtime&, TcpSocket& socket, const Host& client_host) override {
        auto continuing = co_await firstAcceptHook(socket, client_host, m_state);
        co_return continuing.value_or(false);
    }

private:
    TestState* m_state;
};
```

Add equivalent `SecondPlugin`. Extend `TestState` with:

```cpp
std::atomic<int> first_start_count{0};
std::atomic<int> second_start_count{0};
std::atomic<int> first_stop_count{0};
std::atomic<int> second_stop_count{0};
```

Register with:

```cpp
bool registered_first = server.addAcceptPlugin(std::make_unique<FirstPlugin>(&state));
bool registered_second = server.addAcceptPlugin(std::make_unique<SecondPlugin>(&state));
```

After `server.stop()`, assert:

```cpp
waitForCount(state.first_start_count, 1, "first plugin start should run once");
waitForCount(state.second_start_count, 1, "second plugin start should run once");
waitForCount(state.first_stop_count, 1, "first plugin stop should run once");
waitForCount(state.second_stop_count, 1, "second plugin stop should run once");
```

**Step 2: Run the narrow build and confirm failure**

Run:

```bash
rtk cmake --build /tmp/galay-http-kernel-511-final --target t81_accepthook --parallel
```

Expected: FAIL because `plugin::AcceptPlugin<TcpSocket>` is not a class yet and `addAcceptPlugin` does not accept `std::unique_ptr`.

**Step 3: Implement the base class**

Replace the alias in `galay-http/plugin/common/defn.h` with:

```cpp
template<typename SocketType>
class AcceptPlugin {
public:
    virtual ~AcceptPlugin() = default;

    virtual bool start(galay::kernel::Runtime& runtime) {
        (void)runtime;
        return true;
    }

    virtual void stop() noexcept {}

    virtual galay::kernel::Task<bool> handle(
        galay::kernel::Runtime& runtime,
        SocketType& socket,
        const galay::kernel::Host& client_host) = 0;
};
```

Keep the Doxygen comment explicit:
- server owns plugin lifetime
- `start()` is called after runtime starts and before accept loops are scheduled
- `stop()` is called before runtime stops
- `handle()` socket and host references are per-accept and must not be retained beyond the await chain
- plugin members are shared by all concurrent accepts on the same server and must be protected when mutable

**Step 4: Re-run the narrow build**

Run:

```bash
rtk cmake --build /tmp/galay-http-kernel-511-final --target t81_accepthook --parallel
```

Expected: Still FAIL until server storage and registration are migrated in Task 2.

---

### Task 2: Migrate HTTP/HTTPS Server Plugin Storage And Lifecycle

**Files:**
- Modify: `galay-http/server/http/http_server.h`
- Test: `test/t81_accepthook.cc`

**Step 1: Change registration ownership**

In `HttpServerImpl<SocketType>`, change:

```cpp
std::vector<plugin::AcceptPlugin<SocketType>> m_accept_handlers;
```

to:

```cpp
std::vector<std::unique_ptr<plugin::AcceptPlugin<SocketType>>> m_accept_plugins;
std::size_t m_started_plugin_count = 0;
```

Change registration to:

```cpp
bool addAcceptPlugin(std::unique_ptr<plugin::AcceptPlugin<SocketType>> plugin) {
    if (m_running.load() || !plugin) {
        return false;
    }
    m_accept_plugins.push_back(std::move(plugin));
    return true;
}
```

**Step 2: Add lifecycle helpers**

Add protected helpers near `startInternal()`:

```cpp
bool startPlugins() {
    m_started_plugin_count = 0;
    for (auto& plugin : m_accept_plugins) {
        try {
            if (!plugin->start(m_runtime)) {
                stopStartedPlugins();
                return false;
            }
            ++m_started_plugin_count;
        } catch (const std::exception& ex) {
            HTTP_LOG_ERROR("[accept-plugin] [start-fail]", "error={}", ex.what());
            stopStartedPlugins();
            return false;
        } catch (...) {
            HTTP_LOG_ERROR("[accept-plugin] [start-fail]", "error=unknown");
            stopStartedPlugins();
            return false;
        }
    }
    return true;
}

void stopStartedPlugins() noexcept {
    while (m_started_plugin_count > 0) {
        --m_started_plugin_count;
        try {
            m_accept_plugins[m_started_plugin_count]->stop();
        } catch (...) {
            HTTP_LOG_ERROR("[accept-plugin] [stop-fail]", "error=exception");
        }
    }
}
```

**Step 3: Wire lifecycle into start/stop**

In `startInternal()`:

```cpp
m_runtime.start();
if (!startPlugins()) {
    m_runtime.stop();
    return false;
}
m_running.store(true);
```

In `stop()`:

```cpp
m_running.store(false);
stopStartedPlugins();
...
m_runtime.stop();
```

Keep `stop()` idempotent: if server is not running but `m_started_plugin_count > 0` because start failed midway, `stopStartedPlugins()` must still be safe.

**Step 4: Execute plugin chain through virtual handle**

Replace each HTTP/1 accept loop call with:

```cpp
for (auto& plugin : m_accept_plugins) {
    auto plugin_result = co_await plugin->handle(getRuntime(), client_socket, client_host);
    if (!plugin_result) {
        HTTP_LOG_ERROR("[accept-plugin] [task-fail]",
                       "error={}",
                       plugin_result.error().message());
        continuing = false;
        break;
    }
    continuing = plugin_result.value();
    if (!continuing) {
        break;
    }
}
```

Apply the same shape in the HTTPS branch. This also fixes the existing HTTPS branch drift where the kernel task-layer `std::expected` is not checked.

**Step 5: Re-run targeted test**

Run:

```bash
rtk cmake --build /tmp/galay-http-kernel-511-final --target t81_accepthook --parallel
rtk /tmp/galay-http-kernel-511-final/test/t81_accepthook
```

Expected: PASS and lifecycle counters equal `1`.

---

### Task 3: Migrate H2C/H2 Server Plugin Storage And Lifecycle

**Files:**
- Modify: `galay-http/server/http2/http2_server.h`
- Test: `test/t84_h2acceptplugin.cc`

**Step 1: Convert H2C registration and storage**

In `H2cServer`, replace:

```cpp
std::vector<galay::http::plugin::AcceptPlugin<TcpSocket>> m_accept_handlers;
```

with:

```cpp
std::vector<std::unique_ptr<galay::http::plugin::AcceptPlugin<TcpSocket>>> m_accept_plugins;
std::size_t m_started_plugin_count = 0;
```

Change `addAcceptPlugin(...)` to accept `std::unique_ptr<...>`.

**Step 2: Add H2C lifecycle helpers**

Use the same `startPlugins()` / `stopStartedPlugins()` semantics as HTTP/1:
- call `m_runtime.start()`
- then `startPlugins()`
- then `m_running.store(true)`
- on failure, stop already-started plugins and runtime
- in `stop()`, call plugin stops before `m_runtime.stop()`

**Step 3: Convert H2C accept execution**

Call `plugin->handle(getRuntime(), client_socket, client_host)` and check the outer `std::expected` before reading `.value()`.

**Step 4: Convert TLS H2 registration and storage**

Apply the same conversion in `H2Server`, using:

```cpp
std::vector<std::unique_ptr<galay::http::plugin::AcceptPlugin<galay::ssl::SslSocket>>> m_accept_plugins;
```

The TLS code may not be locally buildable until `galay-ssl` is rebuilt against `galay-kernel 5.1.1`; still update the guarded source consistently.

**Step 5: Update H2 tests to class plugins**

In `test/t84_h2acceptplugin.cc`, replace lambda plugins with local plugin classes. Add lifecycle counters for H2C at minimum:

```cpp
class CountingH2cPlugin final : public plugin::AcceptPlugin<TcpSocket> {
public:
    CountingH2cPlugin(std::atomic<int>* calls, bool result)
        : m_calls(calls), m_result(result) {}

    bool start(Runtime&) override {
        m_started = true;
        return true;
    }

    void stop() noexcept override {
        m_stopped = true;
    }

    Task<bool> handle(Runtime&, TcpSocket&, const Host&) override {
        m_calls->fetch_add(1);
        co_return m_result;
    }

private:
    std::atomic<int>* m_calls;
    bool m_result;
    bool m_started = false;
    bool m_stopped = false;
};
```

Use separate plugin classes for TLS H2 only inside `#ifdef GALAY_SSL_FEATURE_ENABLED`.

**Step 6: Build and run H2C coverage**

Run:

```bash
rtk cmake --build /tmp/galay-http-kernel-511-final --target t84_h2acceptplugin --parallel
rtk /tmp/galay-http-kernel-511-final/test/t84_h2acceptplugin
```

Expected: PASS for H2C coverage. TLS H2 portions remain gated by `GALAY_SSL_FEATURE_ENABLED`.

---

### Task 4: Refactor BlackList Into A Lifecycle Plugin Class

**Files:**
- Modify: `galay-http/plugin/blacklist/blacklist.hpp`
- Test: `test/t83_blacklist.cc`
- Test: `test/t84_h2acceptplugin.cc`
- Example: `examples/include/e15_blacklist.cpp`
- Example: `examples/import/e15_blacklist.cpp`

**Step 1: Write failing BlackList call-site changes**

Replace all `BlackList<TcpSocket>::createPlugin(config)` registrations with:

```cpp
server.addAcceptPlugin(std::make_unique<BlackList<TcpSocket>>(config));
```

Keep `BlackList<TcpSocket>::clearConnInfo()` test cleanup as a static reset helper. The refactor changes how plugins are registered and executed, not the global accounting model: all `BlackList<TcpSocket>` instances still share the same `ConnInfoStorage` and `AsyncMutex`.

For helper functions currently accepting `AcceptPlugin<TcpSocket> plugin`, change them to:

```cpp
ScenarioResult runScenario(std::unique_ptr<AcceptPlugin<TcpSocket>> plugin, ...)
```

**Step 2: Run targeted build and confirm failure**

Run:

```bash
rtk cmake --build /tmp/galay-http-kernel-511-final --target t83_blacklist --parallel
```

Expected: FAIL until `BlackList` inherits the plugin base class.

**Step 3: Convert BlackList class**

Change the class declaration to:

```cpp
template<typename SocketType>
class BlackList final : public AcceptPlugin<SocketType> {
public:
    explicit BlackList(BlackListConfig config = BlackListConfig{})
        : m_config(std::move(config)) {}

    explicit BlackList(std::size_t connection_count_limit)
        : m_config(makeLimitConfig(connection_count_limit)) {}

    galay::kernel::Task<bool> handle(
        galay::kernel::Runtime& runtime,
        SocketType& socket,
        const galay::kernel::Host& host) override {
        (void)runtime;
        ...
    }

    static void clearConnInfo() {
        m_storage.clearConnInfo();
    }

private:
    static BlackListConfig makeLimitConfig(std::size_t connection_count_limit) {
        BlackListConfig config;
        BlackListConfig::IntervalBlockPolicy policy;
        policy.max_attempts_per_interval = connection_count_limit;
        config.policy = policy;
        return config;
    }

    BlackListConfig m_config;
    static ConnInfoStorage m_storage;
    static galay::kernel::AsyncMutex m_mutex;
};
```

Move the existing lambda body into `handle(...)`, replacing captured `config` with instance member `m_config`. Keep `m_storage` and `m_mutex` static.

Delete:
- `static AcceptPlugin<SocketType> createPlugin(...)`

Keep the existing static member definitions at the bottom of the header, adjusted only as needed for the new class shape.

**Step 4: Preserve BlackList behavior**

Keep existing semantics:
- excluded IPs bypass blacklist but continue downstream plugin chain
- interval policy blocks over threshold
- decay policy lazily decays per access
- `close_blocked_socket` closes the current socket before returning `false`
- lock failure logs and returns `false`

Do not add periodic cleanup in this task. The lifecycle API enables future background cleanup, but this refactor should avoid adding a new scheduler loop and a new shutdown race in the same change.

**Step 5: Re-run targeted BlackList test**

Run:

```bash
rtk cmake --build /tmp/galay-http-kernel-511-final --target t83_blacklist --parallel
rtk /tmp/galay-http-kernel-511-final/test/t83_blacklist
```

Expected: PASS.

---

### Task 5: Add Lifecycle Failure Coverage

**Files:**
- Modify: `test/t81_accepthook.cc`

**Step 1: Add start-failure plugin**

Add a local plugin:

```cpp
class FailingStartPlugin final : public plugin::AcceptPlugin<TcpSocket> {
public:
    explicit FailingStartPlugin(std::atomic<int>* stop_count)
        : m_stop_count(stop_count) {}

    bool start(Runtime&) override {
        return false;
    }

    void stop() noexcept override {
        m_stop_count->fetch_add(1);
    }

    Task<bool> handle(Runtime&, TcpSocket&, const Host&) override {
        co_return true;
    }

private:
    std::atomic<int>* m_stop_count;
};
```

Create a small test section:
- register a successful plugin first
- register `FailingStartPlugin` second
- call `server.start(...)`
- assert `server.isRunning()` is false
- assert first plugin `stop()` ran once
- assert failing plugin `stop()` did not run if its `start()` returned false before being counted as started

**Step 2: Add null registration assertion**

Add:

```cpp
if (server.addAcceptPlugin(nullptr)) {
    fail("null plugin registration should be rejected");
}
```

before the normal registration path.

**Step 3: Run targeted lifecycle test**

Run:

```bash
rtk cmake --build /tmp/galay-http-kernel-511-final --target t81_accepthook --parallel
rtk /tmp/galay-http-kernel-511-final/test/t81_accepthook
```

Expected: PASS.

---

### Task 6: Update Public Docs, Examples, And Module Surface

**Files:**
- Modify: `docs/02-API参考.md`
- Modify: `docs/03-使用指南.md` if it references accept plugin usage
- Modify: `docs/04-示例代码.md` if it references blacklist usage
- Modify: `README.md` if it references plugin usage
- Modify: `CHANGELOG.md`
- Modify: `examples/include/e15_blacklist.cpp`
- Modify: `examples/import/e15_blacklist.cpp`
- Inspect: `galay-http/module/http/galay_http.cppm`
- Inspect: `galay-http/module/module_prelude.hpp`

**Step 1: Update API reference**

Replace the current `std::function` description with the class API:

```cpp
template<typename SocketType>
class AcceptPlugin {
public:
    virtual ~AcceptPlugin() = default;
    virtual bool start(galay::kernel::Runtime& runtime);
    virtual void stop() noexcept;
    virtual galay::kernel::Task<bool> handle(
        galay::kernel::Runtime& runtime,
        SocketType& socket,
        const galay::kernel::Host& client_host) = 0;
};
```

Document:
- server owns plugin instance after registration
- `start()` runs before accept loops
- `stop()` runs during server stop
- `handle()` returns `false` to stop chain and skip business handling
- the legacy context parameter is intentionally removed; plugin members carry plugin-level state, while per-accept scratch data should stay local to `handle()`

**Step 2: Update BlackList docs and examples**

Use:

```cpp
server.addAcceptPlugin(
    std::make_unique<galay::http::plugin::BlackList<TcpSocket>>(config));
```

Delete all docs references to:

```cpp
BlackList<TcpSocket>::createPlugin(...)
```

**Step 3: Verify module surface**

`galay-http/module/http/galay_http.cppm` and `module_prelude.hpp` already include plugin headers. Rebuild module-capable targets only if local generator/compiler supports it; otherwise keep includes valid.

**Step 4: Update changelog**

In `[Unreleased]`, add:

```markdown
- 将 accept plugin 从 lambda/function hook 改为有生命周期的虚基类插件，由 server 在 start/stop 时统一启动和停止。
- `BlackList` 改为实例插件，不再通过 `createPlugin()` 生成 lambda；配置归插件实例所有，连接统计与异步锁继续按 SocketType 静态共享。
```

**Step 5: Run grep checks**

Run:

```bash
rtk rg -n "createPlugin|std::function<galay::kernel::Task<bool>|using AcceptPlugin" galay-http test examples docs README.md CHANGELOG.md
```

Expected: no stale old API references, except explanatory release notes if intentionally kept as historical text.

---

### Task 7: Full Verification

**Files:**
- No planned source edits

**Step 1: Configure clean non-SSL build**

Run:

```bash
rtk cmake -S . -B /tmp/galay-http-plugin-lifecycle \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_BENCHMARKS=OFF \
  -DBUILD_MODULE_EXAMPLES=OFF \
  -DGALAY_BUILD_SSL=OFF
```

Expected: configure succeeds.

**Step 2: Build clean non-SSL tests**

Run:

```bash
rtk cmake --build /tmp/galay-http-plugin-lifecycle --parallel
```

Expected: build succeeds.

**Step 3: Run CTest**

Run:

```bash
rtk ctest --test-dir /tmp/galay-http-plugin-lifecycle --output-on-failure
```

Expected: all non-manual tests pass.

**Step 4: Build examples**

Run:

```bash
rtk cmake -S . -B /tmp/galay-http-plugin-lifecycle-examples \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_BENCHMARKS=OFF \
  -DBUILD_MODULE_EXAMPLES=OFF \
  -DGALAY_BUILD_SSL=OFF
rtk cmake --build /tmp/galay-http-plugin-lifecycle-examples --parallel
```

Expected: include examples build succeeds. Import/module examples may be disabled on local `Unix Makefiles` + AppleClang; do not claim they were built unless the build output shows module target enabled.

**Step 5: Probe SSL only after matching galay-ssl is available**

Run only when `galay-ssl` has been rebuilt against `galay-kernel 5.1.1`:

```bash
rtk cmake -S . -B /tmp/galay-http-plugin-lifecycle-ssl \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_BENCHMARKS=OFF \
  -DBUILD_MODULE_EXAMPLES=OFF \
  -DGALAY_BUILD_SSL=ON
rtk cmake --build /tmp/galay-http-plugin-lifecycle-ssl --parallel
```

Expected: configure/build succeeds only with a compatible `galay-ssl`. If local `galay-ssl` still requests `galay-kernel 3.4.4` or `5.0.0`, record SSL as externally blocked.

**Step 6: Final diff checks**

Run:

```bash
rtk proxy git diff --check
rtk proxy git status --short
```

Expected:
- no whitespace errors
- only intended plugin lifecycle files changed, plus any pre-existing unrelated worktree entries left untouched

---

## Commit Guidance

Do not commit unless explicitly requested. If committing later, stage only files related to this refactor and use a Chinese message such as:

```bash
git add galay-http/plugin/common/defn.h \
        galay-http/plugin/blacklist/blacklist.hpp \
        galay-http/server/http/http_server.h \
        galay-http/server/http2/http2_server.h \
        test/t81_accepthook.cc \
        test/t83_blacklist.cc \
        test/t84_h2acceptplugin.cc \
        examples/include/e15_blacklist.cpp \
        examples/import/e15_blacklist.cpp \
        docs/02-API参考.md \
        CHANGELOG.md
git commit -m "feat: 改造 accept plugin 生命周期接口"
```

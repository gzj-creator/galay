# MCP Client Aggregation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the split public MCP client classes with one mode-selected `McpClient` while preserving the existing HTTP, stdio, and protocol behavior.

**Architecture:** `McpClient` is the only public client class and dispatches to internal stdio/http transports selected at construction time. `McpClient` is non-copyable and non-movable because HTTP protocol methods return cold coroutine tasks that may access the original object only after scheduling. The HTTP transport keeps the current safe request pattern: create a local HTTP session inside the coroutine and keep it alive until the request awaitable completes. This plan does not refactor `http::HttpClient`, `HttpSession`, H2/H2c, or WebSocket client APIs.

**Tech Stack:** C++23, CMake, CTest, existing `galay::kernel::Task`, `std::expected`, MCP JSON helpers, current `http::HttpClient::getSession()` behavior.

---

## Scope

- Implement `galay::mcp::McpClient`.
- Delete public `McpHttpClient` and `McpStdioClient` classes and their old headers/implementations.
- Move existing HTTP and stdio behavior behind internal transport classes.
- Add `InvalidTransportMode` for calling the wrong API set on the selected mode.
- Update MCP tests, examples, benchmarks, all MCP module docs, module prelude, and module facade to use `mcp/client/client.h`.

## Non-Goals

- Do not change `http::HttpClient` ownership or add persistent `HttpSession` storage.
- Do not add `HttpClient::post()` or request-level high-level HTTP helpers.
- Do not refactor H2, H2c, WS, or WSS client APIs.
- Do not keep compatibility layers: no forwarding headers, no aliases, no deprecated wrappers, no empty old translation units.
- Do not force stdio into coroutine form or HTTP into synchronous form.
- Do not make `McpClient` movable unless every async method captures transport ownership independently of `this`. This plan uses the simpler safer rule: `McpClient` is not movable.

## Target Public API

Create `src/mcp/client/client.h`:

```cpp
namespace galay::mcp {

enum class McpClientMode {
    Stdio,
    Http
};

struct McpStdioClientConfig {
    std::istream* input = &std::cin;
    std::ostream* output = &std::cout;
};

struct McpHttpClientConfig {
    std::string url;
};

class McpClient {
public:
    using ConnectAwaitable =
        decltype(std::declval<http::HttpClient&>().connect(std::declval<const std::string&>()));
    using CloseAwaitable = decltype(std::declval<http::HttpClient&>().close());

    explicit McpClient(McpStdioClientConfig config = {});
    McpClient(kernel::Runtime& runtime, McpHttpClientConfig config);
    ~McpClient();

    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;
    McpClient(McpClient&&) = delete;
    McpClient& operator=(McpClient&&) = delete;

    McpClientMode mode() const;

    // HTTP-mode lifecycle.
    ConnectAwaitable connect();
    ConnectAwaitable connect(std::string url);
    CloseAwaitable disconnectAsync();

    // Stdio-mode lifecycle.
    std::expected<void, McpError> disconnect();

    // Stdio-mode protocol calls.
    std::expected<void, McpError> initialize(const std::string& clientName,
                                             const std::string& clientVersion);
    std::expected<JsonString, McpError> callTool(const std::string& toolName,
                                                 const JsonString& arguments);
    std::expected<std::vector<Tool>, McpError> listTools();
    std::expected<std::vector<Resource>, McpError> listResources();
    std::expected<std::string, McpError> readResource(const std::string& uri);
    std::expected<std::vector<Prompt>, McpError> listPrompts();
    std::expected<JsonString, McpError> getPrompt(const std::string& name,
                                                  const JsonString& arguments);
    std::expected<void, McpError> ping();

    // HTTP-mode protocol calls.
    galay::kernel::Task<void> initialize(std::string clientName,
                                         std::string clientVersion,
                                         std::expected<void, McpError>& result);
    galay::kernel::Task<void> callTool(std::string toolName,
                                       JsonString arguments,
                                       std::expected<JsonString, McpError>& result);
    galay::kernel::Task<void> listTools(std::expected<std::vector<Tool>, McpError>& result);
    galay::kernel::Task<void> listResources(std::expected<std::vector<Resource>, McpError>& result);
    galay::kernel::Task<void> readResource(std::string uri,
                                           std::expected<std::string, McpError>& result);
    galay::kernel::Task<void> listPrompts(std::expected<std::vector<Prompt>, McpError>& result);
    galay::kernel::Task<void> getPrompt(std::string name,
                                        JsonString arguments,
                                        std::expected<JsonString, McpError>& result);
    galay::kernel::Task<void> ping(std::expected<void, McpError>& result);

    bool isConnected() const;
    bool isInitialized() const;
    const ServerInfo& getServerInfo() const;
    const ServerCapabilities& getServerCapabilities() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace galay::mcp
```

Wrong-mode behavior:

- HTTP-mode object calling stdio sync protocol methods returns `McpError::invalidTransportMode("stdio API called on HTTP client")`.
- Stdio-mode object calling HTTP coroutine protocol methods writes `McpError::invalidTransportMode("HTTP API called on stdio client")` into the output result when the returned cold task is scheduled/awaited.
- Stdio-mode object calling `connect()` returns an immediately completed `ConnectAwaitable` with `IOError(kParamInvalid, 0)`.
- HTTP-mode object calling sync `disconnect()` returns `McpError::invalidTransportMode("stdio disconnect called on HTTP client")`; use `disconnectAsync()` to close the HTTP socket.
- `isConnected()` in stdio mode returns the same value as `isInitialized()`. Stdio has no separate transport connection state.
- `McpStdioClientConfig{nullptr, ...}` or `{..., nullptr}` is accepted at construction but all read/write protocol operations must return `McpError::invalidParams("stdio input/output stream is null")` before dereferencing.

## Task 1: Add Red Surface Tests

**Files:**
- Create: `test/mcp/t8_client_surface.cc`
- Create: `test/mcp/t9_client_mode.cc`
- Modify: `test/mcp/t5_await.cc`

**Step 1: Add compile-surface test**

Create `test/mcp/t8_client_surface.cc`:

```cpp
#include "mcp/client/client.h"
#include "kernel/kernel/runtime.h"

#include <concepts>
#include <expected>
#include <string>
#include <utility>
#include <vector>

using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::mcp::JsonString;
using galay::mcp::McpClient;
using galay::mcp::McpError;
using galay::mcp::McpHttpClientConfig;
using galay::mcp::McpStdioClientConfig;
using galay::mcp::Prompt;
using galay::mcp::Resource;
using galay::mcp::Tool;

static_assert(requires(McpClient& client) {
    { client.mode() } -> std::same_as<galay::mcp::McpClientMode>;
    { client.isConnected() } -> std::same_as<bool>;
    { client.isInitialized() } -> std::same_as<bool>;
});

static_assert(!std::movable<McpClient>);

static_assert(requires(McpClient& client, const std::string& s, const JsonString& json) {
    { client.initialize(s, s) } -> std::same_as<std::expected<void, McpError>>;
    { client.callTool(s, json) } -> std::same_as<std::expected<JsonString, McpError>>;
    { client.listTools() } -> std::same_as<std::expected<std::vector<Tool>, McpError>>;
    { client.listResources() } -> std::same_as<std::expected<std::vector<Resource>, McpError>>;
    { client.readResource(s) } -> std::same_as<std::expected<std::string, McpError>>;
    { client.listPrompts() } -> std::same_as<std::expected<std::vector<Prompt>, McpError>>;
    { client.getPrompt(s, json) } -> std::same_as<std::expected<JsonString, McpError>>;
    { client.ping() } -> std::same_as<std::expected<void, McpError>>;
});

static_assert(requires(McpClient& client,
                       std::string s,
                       JsonString json,
                       std::expected<void, McpError>& void_result,
                       std::expected<JsonString, McpError>& json_result,
                       std::expected<std::vector<Tool>, McpError>& tools_result) {
    client.initialize(std::move(s), std::move(s), void_result);
    client.callTool(std::move(s), std::move(json), json_result);
    client.listTools(tools_result);
});

int main()
{
    McpClient stdio_client(McpStdioClientConfig{});

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    McpClient http_client(runtime, McpHttpClientConfig{.url = "http://127.0.0.1:8080/mcp"});

    return stdio_client.mode() == galay::mcp::McpClientMode::Stdio &&
           http_client.mode() == galay::mcp::McpClientMode::Http
        ? 0
        : 1;
}
```

**Step 2: Add wrong-mode test**

Create `test/mcp/t9_client_mode.cc`:

```cpp
#include "mcp/client/client.h"
#include "mcp/common/mcp_error.h"
#include "kernel/kernel/runtime.h"

#include <expected>
#include <iostream>
#include <vector>

using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::mcp::McpClient;
using galay::mcp::McpErrorCode;
using galay::mcp::McpHttpClientConfig;
using galay::mcp::McpStdioClientConfig;

int main()
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    McpClient http_client(runtime, McpHttpClientConfig{.url = "http://127.0.0.1:8080/mcp"});
    auto wrong_sync = http_client.listTools();
    if (wrong_sync || wrong_sync.error().code() != McpErrorCode::InvalidTransportMode) {
        std::cerr << "HTTP client accepted stdio sync API\n";
        return 1;
    }
    auto wrong_disconnect = http_client.disconnect();
    if (wrong_disconnect || wrong_disconnect.error().code() != McpErrorCode::InvalidTransportMode) {
        std::cerr << "HTTP client accepted stdio disconnect API\n";
        return 1;
    }

    McpClient stdio_client(McpStdioClientConfig{});
    runtime.start();
    std::expected<void, galay::mcp::McpError> wrong_async_result;
    auto join = runtime.spawn(stdio_client.ping(wrong_async_result));
    if (!join) {
        runtime.stop();
        std::cerr << "failed to spawn wrong-mode async task\n";
        return 1;
    }
    auto join_result = join->join();
    runtime.stop();
    if (!join_result) {
        std::cerr << "wrong-mode async task join failed\n";
        return 1;
    }
    if (wrong_async_result || wrong_async_result.error().code() != McpErrorCode::InvalidTransportMode) {
        std::cerr << "stdio client accepted HTTP async API\n";
        return 1;
    }

    if (stdio_client.mode() != galay::mcp::McpClientMode::Stdio) {
        std::cerr << "stdio mode not recorded\n";
        return 1;
    }
    if (stdio_client.isConnected()) {
        std::cerr << "stdio client should report disconnected before initialize\n";
        return 1;
    }

    return 0;
}
```

**Step 3: Update awaitable surface test**

Modify `test/mcp/t5_await.cc`:

- Include `mcp/client/client.h`.
- Replace `McpHttpClient` assertions with `McpClient`.
- Assert `connect()` and `disconnectAsync()` return the same awaitable types as `http::HttpClient::connect()` and `http::HttpClient::close()`.

**Step 4: Run red check**

Run:

```bash
rtk cmake -S . -B build/verify-mcp-client-aggregation -DGALAY_BUILD_MCP=ON -DGALAY_BUILD_HTTP=ON -DGALAY_BUILD_KERNEL=ON -DBUILD_TESTING=ON
rtk cmake --build build/verify-mcp-client-aggregation --target t8_client_surface t9_client_mode t5_await -j 4
```

Expected: fail because `mcp/client/client.h`, `McpClient`, and `InvalidTransportMode` do not exist.

## Task 2: Add `InvalidTransportMode`

**Files:**
- Modify: `src/mcp/common/mcp_error.h`
- Modify: `src/mcp/common/mcp_error.cc`

**Step 1: Add enum value**

Add after `InvalidParams`:

```cpp
InvalidTransportMode = 2004, ///< 当前客户端模式不支持所调用的接口
```

**Step 2: Add factory**

Add:

```cpp
static McpError invalidTransportMode(const std::string& details = "") {
    return McpError(McpErrorCode::InvalidTransportMode, "Invalid transport mode", details);
}
```

**Step 3: Update JSON-RPC mapping**

Update `McpError::toJsonRpcErrorCode()` in `src/mcp/common/mcp_error.cc` so `InvalidTransportMode` maps to JSON-RPC invalid request (`-32600`). Do not add a reverse mapping in `fromJsonRpcError()`; this error is local API misuse, not a peer JSON-RPC error.

**Step 4: Compile MCP target**

Run:

```bash
rtk cmake --build build/verify-mcp-client-aggregation --target mcp -j 4
```

Expected: may still fail until `McpClient` exists, but the error type itself should be valid.

## Task 3: Extract Shared MCP Client Parsing

**Files:**
- Create: `src/mcp/client/client_common.h`
- Create: `src/mcp/client/client_common.cc`

**Step 1: Move duplicate helpers**

Move duplicated helpers from old HTTP/stdio client implementations:

```cpp
const JsonString& EmptyObjectString();

template <typename T, typename ParseFn>
std::expected<std::vector<T>, McpError>
parseListField(std::string_view body, const char* fieldName, ParseFn&& parseFn);

std::expected<std::string, McpError>
parseFirstTextContent(std::string_view body, const char* fieldName);

std::expected<InitializeResult, McpError>
parseInitializeResult(std::string_view body);

std::expected<JsonString, McpError>
parseToolCallTextResult(std::string_view body);
```

Keep template implementation in the header and non-template functions in `.cc`.

**Step 2: Compile**

Run:

```bash
rtk cmake --build build/verify-mcp-client-aggregation --target mcp -j 4
```

Expected: may still fail until transports include the helpers.

## Task 4: Create Internal Stdio Transport

**Files:**
- Create: `src/mcp/client/stdio_transport.h`
- Create: `src/mcp/client/stdio_transport.cc`
- Source: migrate behavior from `src/mcp/client/stdio_client.h`
- Source: migrate behavior from `src/mcp/client/stdio_client.cc`

**Step 1: Define internal transport**

Create `galay::mcp::detail::StdioClientTransport` with the same sync protocol methods currently exposed by `McpStdioClient`.

**Step 2: Preserve behavior**

- Keep line-delimited JSON.
- Keep input/output mutexes.
- Keep initialized/server-info/capabilities state.
- Use `McpStdioClientConfig` input/output streams.
- If input or output stream pointer is null, return `McpError::invalidParams("stdio input/output stream is null")` before any dereference.
- `isConnected()` returns `isInitialized()` for stdio mode.
- Use `client_common` parsing helpers.

**Step 3: Compile**

Run:

```bash
rtk cmake --build build/verify-mcp-client-aggregation --target mcp -j 4
```

Expected: transport compiles once `client.h` exists.

## Task 5: Create Internal HTTP Transport Without Refactoring HTTP Client

**Files:**
- Create: `src/mcp/client/http_transport.h`
- Create: `src/mcp/client/http_transport.cc`
- Source: migrate behavior from `src/mcp/client/http_client.h`
- Source: migrate behavior from `src/mcp/client/http_client.cc`

**Step 1: Define internal transport**

Create `galay::mcp::detail::HttpClientTransport` with the same async protocol methods currently exposed by `McpHttpClient`.

**Step 2: Preserve current safe session lifetime**

In `sendRequest`, keep this lifetime shape:

```cpp
auto sessionResult = m_httpClient->getSession();
if (!sessionResult) {
    // set connection error
    co_return;
}

auto awaitable = sessionResult.value()->post(...);
while (true) {
    auto httpResult = co_await awaitable;
    ...
}
```

`sessionResult` must remain a local variable in the coroutine frame until the loop returns. Do not hide this behind `HttpClient::post()` and do not create a temporary session in a helper that returns an awaitable.

**Step 3: Preserve reconnect behavior**

- If `m_connected` is false, reconnect to `m_serverUrl`.
- Preserve current keep-alive handling based on response headers.
- Preserve current JSON-RPC ID matching and error parsing.

**Step 4: Compile**

Run:

```bash
rtk cmake --build build/verify-mcp-client-aggregation --target mcp -j 4
```

Expected: transport compiles once facade is wired.

## Task 6: Implement `McpClient` Facade

**Files:**
- Create: `src/mcp/client/client.h`
- Create: `src/mcp/client/client.cc`
- Modify: `src/mcp/module/module_prelude.hpp`
- Modify: `src/mcp/module/galay_mcp.cppm`

**Step 1: Add public header**

Implement the target public API from this plan. Use PIMPL so `client.h` does not expose internal transport headers. `client.h` may still include `http/client/http_client.h` to preserve the exact connect/close awaitable surface.

**Step 2: Add implementation**

Use:

```cpp
class McpClient::Impl {
public:
    McpClientMode mode;
    std::unique_ptr<detail::StdioClientTransport> stdio;
    std::unique_ptr<detail::HttpClientTransport> http;
};
```

Only one transport is non-null.

**Step 3: Dispatch sync methods**

For stdio sync methods:

```cpp
if (m_impl->mode != McpClientMode::Stdio) {
    return std::unexpected(McpError::invalidTransportMode("stdio API called on HTTP client"));
}
return m_impl->stdio->listTools();
```

**Step 4: Dispatch async methods**

For HTTP coroutine methods:

```cpp
if (m_impl->mode != McpClientMode::Http) {
    result = std::unexpected(McpError::invalidTransportMode("HTTP API called on stdio client"));
    co_return;
}
co_await m_impl->http->listTools(result);
```

**Step 5: Dispatch lifecycle/state**

- `connect()` and `disconnectAsync()` are valid only in HTTP mode.
- `disconnect()` is valid only in stdio mode and returns `InvalidTransportMode` in HTTP mode.
- `isConnected()` reads HTTP transport connected state in HTTP mode and returns `isInitialized()` in stdio mode.
- `isInitialized()`, `getServerInfo()`, and `getServerCapabilities()` read from the active transport.

**Step 6: Update module prelude/facade**

Remove old client header includes from both `src/mcp/module/module_prelude.hpp` and `src/mcp/module/galay_mcp.cppm`, then add:

```cpp
#include "mcp/client/client.h"
```

**Step 7: Run surface tests**

Run:

```bash
rtk cmake --build build/verify-mcp-client-aggregation --target t8_client_surface t9_client_mode t5_await -j 4
rtk ctest --test-dir build/verify-mcp-client-aggregation -R 'mcp.(client_surface|client_mode|await)' --output-on-failure
```

Expected: pass.

**Step 8: Build module facade if enabled**

Run the repository's module verification script if present:

```bash
rtk bash scripts/verify_module_matrix.sh
```

Expected: pass, or document if the local toolchain has modules disabled while direct include targets pass.

## Task 7: Delete Old Public Client Files

**Files:**
- Delete: `src/mcp/client/http_client.h`
- Delete: `src/mcp/client/http_client.cc`
- Delete: `src/mcp/client/stdio_client.h`
- Delete: `src/mcp/client/stdio_client.cc`

**Step 1: Delete files**

Delete old split public client files. Do not leave forwarding headers, aliases, deprecated wrappers, empty translation units, or compile-time hints.

**Step 2: Verify deletion**

Run:

```bash
rtk test ! -e src/mcp/client/http_client.h
rtk test ! -e src/mcp/client/http_client.cc
rtk test ! -e src/mcp/client/stdio_client.h
rtk test ! -e src/mcp/client/stdio_client.cc
```

Expected: all commands exit 0.

**Step 3: Verify no old names remain**

Run:

```bash
rtk rg -n 'McpHttpClient|McpStdioClient|mcp/client/http_client.h|mcp/client/stdio_client.h' src test examples benchmark docs/modules
```

Expected: no results.

## Task 8: Migrate MCP Tests, Examples, Benchmarks, and Docs

**Files:**
- Modify: `test/mcp/t1_stdio.cc`
- Modify: `test/mcp/t3_http.cc`
- Modify: `test/mcp/t5_await.cc`
- Modify: `benchmark/mcp/b1_stdio_request_throughput.cc`
- Modify: `benchmark/mcp/b2_http_request_throughput.cc`
- Modify: `benchmark/mcp/b3_http_concurrency_throughput.cc`
- Modify: `examples/mcp/common/e1_stdio.inc`
- Modify: `examples/mcp/common/e2_http.inc`
- Modify: `examples/mcp/include/e1_stdio.cc`
- Modify: `examples/mcp/include/e2_http.cc`
- Modify: `examples/mcp/import/e1_stdio.cc`
- Modify: `examples/mcp/import/e2_http.cc`
- Modify: every `docs/modules/mcp/*.md` file that mentions old client headers or class names.

**Step 1: Update stdio usage**

Replace:

```cpp
#include "mcp/client/stdio_client.h"
McpStdioClient client;
```

with:

```cpp
#include "mcp/client/client.h"
McpClient client(McpStdioClientConfig{});
```

**Step 2: Update HTTP usage**

Replace:

```cpp
#include "mcp/client/http_client.h"
McpHttpClient client(runtime);
auto connectResult = co_await client.connect(url);
```

with:

```cpp
#include "mcp/client/client.h"
McpClient client(runtime, McpHttpClientConfig{.url = url});
auto connectResult = co_await client.connect();
```

**Step 3: Update docs**

Document:

- One public client: `McpClient`.
- Constructor selects `Stdio` or `Http`.
- Stdio protocol APIs are synchronous.
- HTTP protocol APIs are coroutine-based.
- Wrong mode returns `InvalidTransportMode`.
- There is no compatibility include path for old client headers.

**Step 4: Run migrated tests**

Run:

```bash
rtk cmake --build build/verify-mcp-client-aggregation --target t1_stdio t3_http t5_await t8_client_surface t9_client_mode -j 4
rtk ctest --test-dir build/verify-mcp-client-aggregation -R 'mcp.(stdio|http|await|client_surface|client_mode)' --output-on-failure
```

Expected: self-contained tests pass. Integration tests that require paired server startup should be run through existing MCP scripts.

**Step 5: Run benchmark/example compile checks**

Run:

```bash
rtk cmake --build build/verify-mcp-client-aggregation --target b1_stdio_request_throughput b2_http_request_throughput b3_http_concurrency_throughput -j 4
```

If exact target names differ, list targets first with:

```bash
rtk cmake --build build/verify-mcp-client-aggregation --target help
```

Expected: MCP benchmark targets using client APIs compile.

## Task 9: Final Verification

**Files:**
- Inspect all touched files.

**Step 1: Confirm no HTTP/H2/WS refactor leaked in**

Run:

```bash
rtk git diff -- src/http src/http2 src/ws
```

Expected: no changes for this plan, except no output.

**Step 2: Confirm HTTP transport still owns session locally**

Run:

```bash
rtk rg -n 'getSession\\(\\)|sessionResult|HttpClient::post|\\.post\\(' src/mcp/client
```

Expected:

- `http_transport.cc` still calls `getSession()`.
- There is no `HttpClient::post` helper introduced.
- The session object remains alive in the coroutine until request completion.

**Step 3: Confirm old names are gone**

Run:

```bash
rtk rg -n 'McpHttpClient|McpStdioClient|mcp/client/http_client.h|mcp/client/stdio_client.h' src test examples benchmark docs/modules
```

Expected: no results.

**Step 3b: Confirm build metadata and module facade are clean**

Run:

```bash
rtk rg -n 'mcp/client/http_client.h|mcp/client/stdio_client.h|http_client.cc|stdio_client.cc' CMakeLists.txt src/mcp/CMakeLists.txt src/mcp/BUILD src/mcp/module
```

Expected: no results.

**Step 4: Build MCP**

Run:

```bash
rtk cmake --build build/verify-mcp-client-aggregation --target mcp -j 4
```

Expected: pass.

**Step 5: Run MCP tests**

Run:

```bash
rtk ctest --test-dir build/verify-mcp-client-aggregation -R '^mcp\\.' --output-on-failure
```

Expected: pass for self-contained tests. Document any integration-only tests that require paired processes.

## Implementation Notes

- The current MCP HTTP request code is safe because `sessionResult` lives in the coroutine frame while `awaitable` is awaited. Preserve that lifetime.
- Do not create helper functions that return an awaitable built from a local `HttpSession`.
- If `src/mcp/client/http_client.cc` has existing uncommitted changes, inspect and preserve them while migrating behavior into `http_transport.cc`.
- This plan intentionally leaves `http::HttpClient`, H2/H2c, and WS/WSS APIs unchanged.

## Suggested Commit Slices

Do not commit unless requested. If commits are requested later, use these slices:

1. `feat: add unified mcp client facade`
2. `refactor: move mcp transports behind facade`
3. `test: migrate mcp client tests to unified api`
4. `docs: document unified mcp client modes`

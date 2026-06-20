# 02-API参考

本页只记录当前公开头中存在且仍可从示例/测试验证的公共 API。若与旧文档冲突，以以下头文件与 `galay-http/CMakeLists.txt` 中的显式安装清单为准。

- `galay-http/server/galay-http/http_server.h`
- `galay-http/client/galay-http/http_client.h`
- `galay-http/client/websocket/ws_client.h`
- `galay-http/client/websocket/ws_session.h`
- `galay-http/client/galay-http2/h2c_client.h`
- `galay-http/client/galay-http2/h2_client.h`
- `galay-http/server/galay-http2/http2_server.h`

## 头文件索引

公开头当前分成两层：

- 稳定 direct-include 入口：主工作流建议直接包含的公共头
- 安装支撑头：仍随安装包分发，用于模板、内联实现、模块预导入或高级扩展，但不默认作为主入口

| 头文件 | 主要类型 | 说明 |
| --- | --- | --- |
| `galay-http/server/galay-http/http_server.h` | `HttpServer`、`HttpServerBuilder`、`HttpsServer`、`HttpsServerBuilder` | HTTP/HTTPS 服务端 builder、启动与停止语义 |
| `galay-http/client/galay-http/http_client.h` | `HttpClient`、`HttpClientBuilder`、`HttpsClient`、`HttpsClientBuilder` | HTTP/HTTPS 客户端连接、握手与 `HttpSession` 入口 |
| `galay-http/client/websocket/ws_client.h` | `WsClient`、`WsClientBuilder`、`WssClient`、`WssClientBuilder` | WebSocket / WSS 客户端连接与升级入口 |
| `galay-http/client/websocket/ws_session.h` | `WsSessionImpl`、`WssSession` | `upgrade()`、`sendText()`、`getMessage()` 等会话能力 |
| `galay-http/client/galay-http2/h2c_client.h` | `H2cClient`、`H2cClientBuilder` | h2c 客户端连接、Upgrade、请求与关闭 |
| `galay-http/client/galay-http2/h2_client.h` | `H2Client`、`H2ClientBuilder` | h2 客户端连接、ALPN 校验、请求与关闭 |
| `galay-http/server/galay-http2/http2_server.h` | `H2cServer`、`H2cServerBuilder`、`H2Server`、`H2ServerBuilder` | h2c / h2 服务端与 fallback 入口 |

补充公开头分组：

- 稳定 direct-include 入口：
  - `galay-http/server/galay-http/http_server.h`
  - `galay-http/client/galay-http/http_client.h`
  - `galay-http/client/websocket/ws_client.h`
  - `galay-http/client/websocket/ws_session.h`
  - `galay-http/client/galay-http2/h2c_client.h`
  - `galay-http/client/galay-http2/h2_client.h`
  - `galay-http/server/galay-http2/http2_server.h`
- 其他稳定 direct-include 公共头：
  - `galay-http/protoc/galay-http/http_base.h`
  - `galay-http/protoc/galay-http/http_body.h`
  - `galay-http/protoc/galay-http/http_chunk.h`
  - `galay-http/protoc/galay-http/http_error.h`
  - `galay-http/protoc/galay-http/http_header.h`
  - `galay-http/protoc/galay-http/parse_utils.h`
  - `galay-http/protoc/galay-http/http_request.h`
  - `galay-http/protoc/galay-http/http_response.h`
  - `galay-http/kernel/galay-http/http_conn.h`
  - `galay-http/kernel/galay-http/http_session.h`
  - `galay-http/kernel/galay-http/http_reader.h`
  - `galay-http/kernel/galay-http/http_writer.h`
  - `galay-http/kernel/galay-http/reader_settings.h`
  - `galay-http/kernel/galay-http/writer_settings.h`
  - `galay-http/server/galay-http/http_router.h`
  - `galay-http/server/galay-http/http_range.h`
  - `galay-http/server/galay-http/http_etag.h`
  - `galay-http/server/galay-http/file_settings.h`
- WebSocket：
  - `galay-http/protoc/websocket/ws_base.h`
  - `galay-http/protoc/websocket/ws_error.h`
  - `galay-http/protoc/websocket/ws_frame.h`
  - `galay-http/builder/websocket/ws_frame_builder.h`
  - `galay-http/kernel/websocket/ws_conn.h`
  - `galay-http/kernel/websocket/ws_reader.h`
  - `galay-http/kernel/websocket/ws_writer.h`
  - `galay-http/kernel/websocket/reader_settings.h`
  - `galay-http/kernel/websocket/writer_settings.h`
  - `galay-http/server/websocket/ws_upgrade.h`
- HTTP/2：
  - `galay-http/protoc/galay-http2/http2_base.h`
  - `galay-http/protoc/galay-http2/http2_error.h`
  - `galay-http/protoc/galay-http2/http2_frame.h`
  - `galay-http/builder/galay-http2/http2_frame_builder.h`
  - `galay-http/protoc/galay-http2/http2_hpack.h`
  - `galay-http/kernel/galay-http2/http2_conn.h`
  - `galay-http/kernel/galay-http2/h2_core.h`
  - `galay-http/kernel/galay-http2/frame_disp.h`
  - `galay-http/kernel/galay-http2/out_sched.h`
  - `galay-http/kernel/galay-http2/http2_stream.h`
  - `galay-http/kernel/galay-http2/stream_manager.h`
- 工具与模块：
  - `galay-http/common/http_log.h`
  - `galay-http/builder/galay-http/http_builder.h`
  - `galay-http/galay-utils/galay-http2/h2_helper.h`
  - `galay-http/galay-utils/galay-http/http_helper.h`
  - `galay-http/galay-utils/websocket/ws_helper.h`
- 安装支撑头：
  - `galay-http/common/iovec_utils.h`
  - `galay-http/common/http_log.h`
  - `galay-http/protoc/galay-http/parse_utils.h`
  - `galay-http/protoc/galay-http/http_request.inl`
  - `galay-http/protoc/galay-http/http_response.inl`
  - `galay-http/module/module_prelude.hpp`
  - `galay-http/module/galay-http/galay_http.cppm`
  - `galay-http/module/galay-http2/galay_http2.cppm`
  - `galay-http/module/websocket/galay_websocket.cppm`

## 模块与导入边界

来源：`galay-http/module/galay-http/galay_http.cppm`、`galay-http/module/galay-http2/galay_http2.cppm`、`galay-http/module/websocket/galay_websocket.cppm`

### `galay.http`

- 模块声明是 `export module galay.http;`
- 这是 HTTP/1.x 的 canonical import，直接导出 `HttpBase` / `HttpBody` / `HttpChunk` / `HttpError` / `HttpHeader` / `HttpRequest` / `HttpResponse`
- 同时导出 `HttpClient` / `HttpConn` / `HttpReader` / `HttpRouter` / `HttpServer` / `HttpSession` / `HttpWriter`
- 请求 / 响应快速构造入口 `Http1_1RequestBuilder`、`Http1_1ResponseBuilder` 和 `HttpHelper` 也在这个模块里

### `galay.http2`

- 模块声明是 `export module galay.http2;`
- 这是 HTTP/2 的 canonical import，直接导出 `Http2Base` / `Http2Error` / `Http2Frame` / `Http2Hpack`
- 同时导出 `H2cClient`、`Http2Conn`、`Http2Server`、`Http2Stream`、`Http2StreamManager`
- `H2Client` 只在顶层构建选项 `GALAY_BUILD_SSL=ON` 时被这个模块额外导出
- 当 RAG 问题落到 `Http2ErrorCode`、`Http2SettingsId`、`Http2FrameType`、`Http2FlowControlStrategy`、`H2cServerBuilder`、`H2ServerBuilder` 时，优先回到这个模块与其对应头文件

### `galay.websocket`

- 模块声明是 `export module galay.websocket;`
- 这是 WebSocket 的 canonical import，直接导出 `WebSocketFrame`
- 同时导出 `WsClient`、`WsConn`、`WsReader`、`WsReaderSetting`、`WsSession`、`WsUpgrade`、`WsWriter`、`WsWriterSetting`
- `WssClient` / `WssSession` 也经由对应头文件进入该模块，但它们仍受 `GALAY_BUILD_SSL=ON` 约束
- 模块 consumer 的真实验证入口是 `examples/import/*.cpp` 与 `test/t59_module.cpp`

## HTTP / HTTPS 服务端

### `HttpServerConfig`

来源：`galay-http/server/galay-http/http_server.h`

```cpp
struct HttpServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    int backlog = 128;
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    size_t compute_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    RuntimeAffinityConfig affinity;
};
```

- `io_scheduler_count` / `compute_scheduler_count` 都复用 `galay-kernel` Runtime 语义：`GALAY_RUNTIME_SCHEDULER_COUNT_AUTO` 表示自动推导，`0` 表示禁用对应 scheduler
- `affinity` 直接沿用 `RuntimeAffinityConfig`；`HttpServerBuilder::sequentialAffinity(...)` 和 `customAffinity(...)` 只是往这个结构里写值

### `HttpServerBuilder`

`HttpServerBuilder` 的真实配置项来自 `galay-http/server/galay-http/http_server.h`：

- `host(std::string)`
- `port(uint16_t)`
- `backlog(int)`
- `ioSchedulerCount(size_t)`
- `computeSchedulerCount(size_t)`
- `sequentialAffinity(size_t io_count, size_t compute_count)`
- `customAffinity(std::vector<uint32_t> io_cpus, std::vector<uint32_t> compute_cpus)`
- `build()`

`HttpServer` 的常用生命周期方法：

- `addAcceptPlugin(std::unique_ptr<plugin::AcceptPlugin<TcpSocket>> plugin)`
- `start(ConnHandler handler)`
- `start(HttpRouter&& router)`
- `stop()`
- `isRunning() const`
- `getRuntime()`

`AcceptPlugin` 在每次 accept 创建底层 socket 并完成基础 socket 选项设置后执行，类型定义在 `galay-http/plugin/common/defn.h`：

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

- plugin 必须以 `std::unique_ptr` 在 `start(...)` 前注册，注册成功后 server 接管实例生命周期；启动后注册会返回 `false`
- `start()` 在 runtime 启动后、accept loop 投递前按注册顺序调用；任一插件启动失败会使 server start 失败，并反序停止已启动插件
- `stop()` 在 server 停止 runtime 前按注册反序调用
- `handle()` 返回 `false` 时停止后续插件，并跳过当前连接的业务处理
- 旧版 context 参数已移除；插件级状态放在插件成员中，单次 accept 的临时数据应作为 `handle()` 内局部变量保存
- HTTP 连接包装和业务 `ConnHandler` / `HttpRouter` 分发发生在 accept plugin 链之后

内置 `BlackList` accept 插件按客户端 IP 统计连接次数；超过阈值后关闭当前 socket 并返回 `false`：

```cpp
#include "galay-http/plugin/blacklist/blacklist.hpp"

server.addAcceptPlugin(
    std::make_unique<galay::http::plugin::BlackList<TcpSocket>>(100));
```

`BlackListConfig::exclude_ips` 可配置不受 blacklist 策略限制的客户端 IP。命中后当前 blacklist
插件直接放行，但后续 accept plugin 仍会继续执行，业务连接也会正常进入 handler。

### `HttpsServerConfig`

来源：`galay-http/server/galay-http/http_server.h`

```cpp
struct HttpsServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 443;
    int backlog = 128;
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    size_t compute_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    RuntimeAffinityConfig affinity;
    HttpReaderSetting reader_setting;
    HttpWriterSetting writer_setting;
    std::string cert_path;
    std::string key_path;
    std::string ca_path;
    bool verify_peer = false;
    int verify_depth = 4;
};
```

- `cert_path` / `key_path` / `ca_path` 是 TLS 上下文真实读取的路径字段；证书或私钥加载失败会让启动阶段直接记录错误
- `reader_setting` / `writer_setting` 是公开结构体字段，但当前 `HttpsServerBuilder` 没有对应 fluent setter；如果你要覆写它们，应该直接构造 `HttpsServerConfig` 再传给 `HttpsServer`
- `verify_peer=false` 时服务端把 `galay-ssl` 验证模式设为 `None`；`true` 时会同时设置 `verify_depth`

### `HttpsServerBuilder`

`HttpsServerBuilder` 在 `HttpServerBuilder` 基础上增加 TLS 参数：

- `certPath(std::string)`
- `keyPath(std::string)`
- `caPath(std::string)`
- `verifyPeer(bool)`
- `verifyDepth(int)`

典型服务端调用顺序：

```cpp
HttpsServer server(HttpsServerBuilder()
    .host("0.0.0.0")
    .port(8443)
    .certPath("test/test.crt")
    .keyPath("test/test.key")
    .build());

server.start(handler);
```

说明：

- `HttpsServer` 没有单独的 `listen()` 或 `bind()` 公共 API。
- 证书加载发生在服务端启动阶段；缺少 `certPath` / `keyPath` 会直接影响 TLS 上线。
- 参考真实示例：`examples/include/e5_https.cpp`。

## HTTP / HTTPS 客户端

### `HttpUrl`

来源：`galay-http/client/galay-http/http_client.h`

```cpp
struct HttpUrl {
    std::string scheme;
    std::string host;
    int port;
    std::string path;
    bool is_secure;

    static std::optional<HttpUrl> parse(const std::string& url);
};
```

- `parse(...)` 使用正则 `^(http|https)://([^:/]+)(?::(\\d+))?(/.*)?$`，只接受 `http` / `https` scheme、可选数字端口和可选路径
- 端口缺省时，`http` 默认 `80`，`https` 默认 `443`
- 路径缺省时自动补成 `/`
- URL 形状不匹配或端口无法 `stoi` 时返回 `std::nullopt`

### `HttpClientConfig` / `HttpsClientConfig`

来源：`galay-http/client/galay-http/http_client.h`

```cpp
struct HttpClientConfig {
    HeaderPair::Mode header_mode = HeaderPair::Mode::ClientSide;
};

struct HttpsClientConfig {
    std::string ca_path;
    bool verify_peer = false;
    int verify_depth = 4;
    HeaderPair::Mode header_mode = HeaderPair::Mode::ClientSide;
};
```

- `header_mode` 是客户端请求头存储模式；`HttpClientBuilder` 只暴露这一个配置项
- `HttpsClientConfig` 在 `HttpClientConfig` 的基础上增加证书校验参数；`HttpsClientBuilder` 会把它转成底层 `HttpClientConfig` 并单独初始化 TLS 上下文
- `HttpsClient::connect()` 内部会调用 `setHostname(m_url.host)` 尝试设置 SNI；失败只记 warning，不会在设置阶段抛出

### `HttpClient`

`HttpClientBuilder` 只暴露：

- `headerMode(HeaderPair::Mode)`
- `build()`

`HttpClient` 的常用入口：

- `connect(const std::string& url)`
- `getSession(size_t ring_buffer_size = 8192, const HttpReaderSetting& = {}, const HttpWriterSetting& = {})`
- `close()`

关键语义：

- `HttpClient::connect()` 只接受 `http://` URL。
- 传入 `https://` 会抛出“HTTPS requires HttpsClient”异常。

### `HttpsClient`

`HttpsClientBuilder` 的真实配置项：

- `caPath(std::string)`
- `verifyPeer(bool)`
- `verifyDepth(int)`
- `headerMode(HeaderPair::Mode)`
- `build()`

`HttpsClient` 的常用入口：

- `connect(const std::string& url)`
- `handshake()`
- `isHandshakeCompleted() const`
- `getSession(...)`
- `close()`

真实调用顺序必须是：

```cpp
HttpsClient client(HttpsClientBuilder()
    .verifyPeer(false)
    .build());

co_await client.connect("https://127.0.0.1:8443/");
co_await client.handshake();
auto session = client.getSession();
```

这与旧文档里“`connect()` 自动完成 HTTPS 握手”的说法不同；当前公开头和真实示例 `examples/include/e6_https.cpp` 明确要求显式 `handshake()`。

补充边界：

- `HttpsClient::connect()` 复用 `HttpUrl::parse(...)`，因此传入 `http://` 形状的 URL 不会立刻报错；当前实现只记录 warning，随后仍按 TLS 连接处理。为了避免语义歧义，实际调用应始终传 `https://`

### `HttpSession`

来源：`galay-http/kernel/galay-http/http_session.h`

`HttpSession` 是 `HttpClient::getSession()` / `HttpsClient::getSession()` 返回的 HTTP/1.x 会话层。

常用入口：

- `get(...)`
- `post(const std::string& uri, const std::string& body, ...)`
- `post(const std::string& uri, std::string&& body, ...)`
- `put(...)`
- `del(...)`
- `sendRequest(HttpRequest&)`
- `sendSerializedRequest(std::string)`
- `getResponse(HttpResponse&)`
- `sendChunk(...)`

关键语义：

- 常规调用优先使用 `get()` / `post()` / `put()` 这类按语义构造请求的入口。
- `post(..., std::string&& body, ...)` 会把请求体直接移动进内部 `HttpRequest`，适合热点路径减少一次 body 拷贝。
- `sendSerializedRequest(std::string)` 属于高级入口：调用方直接提供完整 HTTP/1.x 请求报文，`HttpSession` 只负责发送、超时控制和响应解析，不再帮你构造请求头。
- 使用 `sendSerializedRequest(...)` 时，调用方必须自行保证请求行、Header、空行、Body 和 `Content-Length` 一致；该接口不会再校正这些字段。
- 传入 `sendSerializedRequest(...)` 的字符串所有权会转移到 awaitable 内部；await 完成前不需要额外保活外部缓冲。

## WebSocket / WSS

### `WsClient` 与 `WsSession`

`WsClientBuilder` 只暴露：

- `headerMode(HeaderPair::Mode)`
- `build()`

`WsClient` 的常用入口：

- `connect(const std::string& url)`
- `getSession(WsWriterSetting writer_setting, size_t ring_buffer_size = 8192, const WsReaderSetting& = {})`
- `close()`

`WsSessionImpl` 的常用入口：

- `upgrade()`
- `sendText(...)`
- `sendBinary(...)`
- `getMessage(std::string&, WsOpcode&)`

`ws://` 客户端的最小流程：

```cpp
WsClient client = WsClientBuilder().build();
co_await client.connect("ws://127.0.0.1:8080/ws");
auto session = client.getSession(WsWriterSetting::byClient());
auto upgrader = session.upgrade();
co_await upgrader();
```

### `WssClient`

`WssClientBuilder` 的真实配置项：

- `caPath(std::string)`
- `verifyPeer(bool)`
- `verifyDepth(int)`
- `headerMode(HeaderPair::Mode)`
- `build()`

`WssClient` 的真实流程必须是：

```cpp
WssClient client(WssClientBuilder()
    .verifyPeer(false)
    .build());

co_await client.connect("wss://127.0.0.1:9443/ws");
co_await client.handshake();
auto session = client.getSession(WsWriterSetting::byClient());
auto upgrader = session.upgrade();
co_await upgrader();
```

说明：

- `connect()` 只建立 TCP/TLS 连接准备；WebSocket 升级仍由 `session.upgrade()` 完成。
- `handshake()` 必须在 `connect()` 成功之后调用。
- 参考真实示例：`examples/include/e8_wss.cpp`。

### 服务端边界

当前仓库没有独立的 `WsServer` / `WssServer` 公共类。服务端模式分为：

- `ws`：通过 `HttpServer` handler + `WsSession`/Upgrade 组合实现，参考 `examples/include/e3_ws.cpp`
- `wss`：通过 `HttpsServer` + `WsUpgrade` + 手动帧处理实现，参考 `examples/include/e7_wss.cpp`

`e7_wss_server.cpp` 的文件头已经明确说明：当前 `SslSocket` 缺少 `readv`，因此 WSS 服务端是“示例级模式”，不是单独高层 server API。

## h2c

### `H2cClientConfig`

来源：`galay-http/client/galay-http2/h2c_client.h`

```cpp
struct H2cClientConfig {
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool ping_enabled = true;
    std::chrono::milliseconds ping_interval{30000};
    std::chrono::milliseconds ping_timeout{10000};
    std::chrono::milliseconds settings_ack_timeout{10000};
    std::chrono::milliseconds graceful_shutdown_rtt{100};
    std::chrono::milliseconds graceful_shutdown_timeout{5000};
    uint32_t flow_control_target_window = kDefaultInitialWindowSize;
    Http2FlowControlStrategy flow_control_strategy;
};
```

- `flow_control_strategy` 留空时，连接使用内置低水位策略：当连接窗或流窗低于目标窗口的一半时，自动补到目标值
- `flow_control_target_window` 若被显式设成 `0`，运行时会退回本地 `initial_window_size`，再不合法时才回退到 `kDefaultInitialWindowSize`

### `H2cClientBuilder`

`galay-http/client/galay-http2/h2c_client.h` 当前暴露的配置项：

- `maxConcurrentStreams(uint32_t)`
- `initialWindowSize(uint32_t)`
- `maxFrameSize(uint32_t)`
- `maxHeaderListSize(uint32_t)`
- `pingEnabled(bool)`
- `pingInterval(std::chrono::milliseconds)`
- `pingTimeout(std::chrono::milliseconds)`
- `settingsAckTimeout(std::chrono::milliseconds)`
- `gracefulShutdownRtt(std::chrono::milliseconds)`
- `gracefulShutdownTimeout(std::chrono::milliseconds)`
- `flowControlTargetWindow(uint32_t)`
- `flowControlStrategy(Http2FlowControlStrategy)`
- `build()`

`H2cClient` 的真实调用顺序：

```cpp
auto client = H2cClientBuilder().build();
co_await client.connect("127.0.0.1", 9080);
co_await client.upgrade("/");
auto stream = client.post("/echo", "hello", "text/plain");
co_await client.shutdown();
```

说明：

- `get()` / `post()` 只能在 `upgrade()` 成功后调用。
- `shutdown()` 是当前公开头暴露的优雅关闭入口。

### `H2cServerConfig`

来源：`galay-http/server/galay-http2/http2_server.h`

```cpp
struct H2cServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    int backlog = 128;
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    size_t compute_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    RuntimeAffinityConfig affinity;
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool enable_push = false;
    bool ping_enabled = true;
    std::chrono::milliseconds ping_interval{30000};
    std::chrono::milliseconds ping_timeout{10000};
    std::chrono::milliseconds settings_ack_timeout{10000};
    std::chrono::milliseconds graceful_shutdown_rtt{100};
    std::chrono::milliseconds graceful_shutdown_timeout{5000};
    uint32_t flow_control_target_window = kDefaultInitialWindowSize;
    Http2FlowControlStrategy flow_control_strategy;
    Http2ConnectionHandler stream_handler;
    Http2ActiveConnHandler active_conn_handler;
};
```

- `enable_push` 默认是 `false`；头文件注释明确说明这是出于客户端兼容性考虑
- `stream_handler` 与 `active_conn_handler` 都是配置字段；`start(handler)` 的重载本质上是启动前覆盖它们
- 明文回退入口 `setHttp1Fallback(...)` 不在配置结构里，而是在 `H2cServer` 实例上单独设置

### `H2cServerBuilder`

`galay-http/server/galay-http2/http2_server.h` 中的 `H2cServerBuilder` 暴露：

- `host` / `port` / `backlog`
- `ioSchedulerCount` / `computeSchedulerCount`
- `maxConcurrentStreams` / `initialWindowSize` / `maxFrameSize` / `maxHeaderListSize`
- `enablePush`
- `pingEnabled` / `pingInterval` / `pingTimeout`
- `settingsAckTimeout`
- `gracefulShutdownRtt` / `gracefulShutdownTimeout`
- `flowControlTargetWindow` / `flowControlStrategy`
- `streamHandler(Http2ConnectionHandler)`
- `activeConnHandler(Http2ActiveConnHandler)`
- `sequentialAffinity(...)` / `customAffinity(...)`
- `build()`

`H2cServer` 的服务端入口：

- `start()`
- `start(Http2ConnectionHandler handler)`
- `start(Http2ActiveConnHandler handler)`
- `setHttp1Fallback(Http1FallbackHandler handler)`
- `stop()`

## h2（HTTP/2 over TLS）

### `H2ClientConfig`

来源：`galay-http/client/galay-http2/h2_client.h`

```cpp
struct H2ClientConfig {
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool verify_peer = false;
    std::string ca_path;
};
```

- `H2ClientBuilder` 只暴露这 6 个字段；和 `H2cClientBuilder` 不同，TLS 客户端这一路没有单独暴露 ping / graceful shutdown / flow control 策略参数
- `verify_peer` / `ca_path` 只影响 TLS 证书校验；HTTP/2 preface、ALPN 校验和后续帧处理不由这个结构体配置

### `H2Client`

`H2ClientBuilder` 暴露：

- `maxConcurrentStreams(uint32_t)`
- `initialWindowSize(uint32_t)`
- `maxFrameSize(uint32_t)`
- `maxHeaderListSize(uint32_t)`
- `verifyPeer(bool)`
- `caPath(std::string)`
- `build()`

`H2Client` 的关键入口：

- `connect(const std::string& host, uint16_t port = 443)`
- `get(const std::string& path)`
- `post(const std::string& path, const std::string& body, const std::string& content_type = "application/x-www-form-urlencoded")`
- `close()`
- `isConnected() const`
- `getALPNProtocol() const`

与 `HttpsClient` / `WssClient` 不同，`H2Client::connect()` 内部已经完成：

1. TCP 连接
2. TLS 握手
3. ALPN 协商并校验结果必须为 `h2`
4. 发送 HTTP/2 preface 与本地 settings

最小流程：

```cpp
H2Client client(H2ClientBuilder()
    .verifyPeer(false)
    .build());

auto connect_result = co_await client.connect("127.0.0.1", 9443);
if (!connect_result) {
    co_return;
}

auto stream = client.post("/echo", "hello", "text/plain");
```

### `H2ServerConfig`

来源：`galay-http/server/galay-http2/http2_server.h`

```cpp
struct H2ServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 9443;
    int backlog = 128;
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    size_t compute_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    RuntimeAffinityConfig affinity;
    std::string cert_path;
    std::string key_path;
    std::string ca_path;
    bool verify_peer = false;
    int verify_depth = 4;
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool enable_push = false;
    bool ping_enabled = true;
    std::chrono::milliseconds ping_interval{30000};
    std::chrono::milliseconds ping_timeout{10000};
    std::chrono::milliseconds settings_ack_timeout{10000};
    std::chrono::milliseconds graceful_shutdown_rtt{100};
    std::chrono::milliseconds graceful_shutdown_timeout{5000};
    uint32_t flow_control_target_window = kDefaultInitialWindowSize;
    Http2FlowControlStrategy flow_control_strategy;
    Http2ConnectionHandler stream_handler;
    Http2ActiveConnHandler active_conn_handler;
};
```

- `H2ServerConfig` 本质上是 `H2cServerConfig + TLS 字段`，默认端口是 `9443`
- `cert_path` / `key_path` / `ca_path` / `verify_peer` / `verify_depth` 决定 TLS 握手与客户端证书校验策略
- `enable_push`、ping、graceful shutdown 和流控字段都是真实公开配置，不是文档层概念

### `H2ServerBuilder`

`H2ServerBuilder` 在 `H2cServerBuilder` 基础上增加 TLS 参数：

- `certPath(std::string)`
- `keyPath(std::string)`
- `caPath(std::string)`
- `verifyPeer(bool)`
- `verifyDepth(int)`

同时保留 HTTP/2 运行时参数：

- `maxConcurrentStreams`
- `initialWindowSize`
- `maxFrameSize`
- `maxHeaderListSize`
- `enablePush`
- `pingEnabled`
- `pingInterval`
- `pingTimeout`
- `settingsAckTimeout`
- `gracefulShutdownRtt`
- `gracefulShutdownTimeout`
- `flowControlTargetWindow`
- `flowControlStrategy`
- `streamHandler`
- `activeConnHandler`

`H2Server` 的常用入口：

- `start()`
- `start(Http2ConnectionHandler handler)`
- `start(Http2ActiveConnHandler handler)`
- `setHttp1Fallback(std::function<Task<void>(HttpConnImpl<SslSocket>, HttpRequestHeader)>)`
- `stop()`
- `isRunning() const`
- `getRuntime()`

## HttpRouter 与静态文件配置

来源：`galay-http/server/galay-http/http_router.h`、`galay-http/server/galay-http/file_settings.h`

### `FileTransferMode`

```cpp
enum class FileTransferMode {
    MEMORY,
    CHUNK,
    SENDFILE,
    AUTO,
};
```

- `MEMORY`：完整读入内存后发送，适合小文件。
- `CHUNK`：使用 HTTP chunked 编码分块发送，适合中等文件。
- `SENDFILE`：使用零拷贝 `sendfile`，适合大文件。
- `AUTO`：按阈值自动选择；默认是小文件 `MEMORY`、中等文件 `CHUNK`、大文件 `SENDFILE`。

### `StaticFileSetting`

```cpp
class StaticFileSetting {
public:
    StaticFileSetting();

    void setTransferMode(FileTransferMode mode);
    FileTransferMode getTransferMode() const;

    void setSmallFileThreshold(size_t threshold);
    size_t getSmallFileThreshold() const;

    void setLargeFileThreshold(size_t threshold);
    size_t getLargeFileThreshold() const;

    void setChunkSize(size_t size);
    size_t getChunkSize() const;

    void setSendFileChunkSize(size_t size);
    size_t getSendFileChunkSize() const;

    void setEnableCache(bool enable);
    bool isEnableCache() const;

    void setEnableETag(bool enable);
    bool isEnableETag() const;

    void setMaxCacheSize(size_t size);
    size_t getMaxCacheSize() const;

    FileTransferMode decideTransferMode(size_t file_size) const;
};
```

- `StaticFileSetting` 没有公开 `mode` 字段；示例代码必须使用 `setTransferMode(FileTransferMode::...)`。
- 默认阈值是：小文件 `64KB`、大文件 `1MB`、chunk 大小 `64KB`、sendfile 分块 `10MB`。
- `setEnableCache(...)` 仅对 `mountHardly(...)` 的启动期预加载缓存生效。
- `decideTransferMode(...)` 只在 `AUTO` 模式下根据文件大小决策；其他模式直接返回显式设置值。

### `HttpRouter`

```cpp
class HttpRouter {
public:
    HttpRouter();

    template<HttpMethod... Methods>
    void addHandler(const std::string& path, HttpRouteHandler handler);

    RouteMatch findHandler(HttpMethod method, const std::string& path);
    bool delHandler(HttpMethod method, const std::string& path);
    void clear();
    size_t size() const;

    void mount(const std::string& routePrefix,
               const std::string& dirPath,
               const StaticFileSetting& config = StaticFileSetting());

    void mountHardly(const std::string& routePrefix,
                     const std::string& dirPath,
                     const StaticFileSetting& config = StaticFileSetting());

    void tryFiles(const std::string& routePrefix,
                  const std::string& dirPath,
                  const std::string& upstreamHost,
                  uint16_t upstreamPort,
                  const StaticFileSetting& config = StaticFileSetting(),
                  ProxyMode mode = ProxyMode::Http);

    void proxy(const std::string& routePrefix,
               const std::string& upstreamHost,
               uint16_t upstreamPort,
               ProxyMode mode = ProxyMode::Http);
};
```

- `mount(...)`：运行时查文件系统，适合动态静态资源目录。
- `mountHardly(...)`：调用时扫描目录并注册精确路由，适合启动期预热和配合缓存。
- `tryFiles(...)`：静态命中优先，未命中回源到上游；`mode` 决定代理走 `HTTP` 还是 `Raw`。
- `proxy(...)`：无本地静态文件阶段，直接把命中的前缀转发到上游。

## 生命周期与返回语义

- 所有 `connect()` / `handshake()` / `close()` / `upgrade()` 入口都按协程 awaitable 设计，需 `co_await`
- `HttpsClient`、`WssClient`：`connect()` 之后必须显式 `handshake()`
- `H2Client`：`connect()` 已包含 TLS 握手与 ALPN 校验，不存在单独的公开 `handshake()` API
- `H2cClient`：必须先 `connect()` 再 `upgrade()`，否则 `get()` / `post()` 不可用
- WebSocket 客户端：`connect()` 只完成底层连接；协议升级发生在 `session.upgrade()`

## 错误与协议级细节定位

### `HttpRangeParser`

来源：`galay-http/server/galay-http/http_range.h`

```cpp
enum class RangeType {
    SINGLE_RANGE,
    MULTIPLE_RANGES,
    SUFFIX_RANGE,
    PREFIX_RANGE,
    INVALID
};

struct RangeParseResult {
    RangeType type;
    std::vector<HttpRange> ranges;
    std::string boundary;
    bool isValid() const;
};

class HttpRangeParser {
public:
    static RangeParseResult parse(const std::string& rangeHeader, uint64_t fileSize);
    static std::string makeContentRange(uint64_t start, uint64_t end, uint64_t fileSize);
    static std::string makeContentRange(const HttpRange& range, uint64_t fileSize);
    static bool checkIfRange(const std::string& ifRangeHeader,
                             const std::string& etag,
                             std::time_t lastModified);
};
```

- `parse(...)` 只接受以 `bytes=` 开头的 Range 值；任何其他单位或空值都会得到 `RangeType::INVALID`
- 多范围请求会在 `RangeParseResult.boundary` 中生成随机 multipart boundary；如果所有子范围都非法，则最终仍回退到 `INVALID`
- `checkIfRange(...)` 只是把 `If-Range` 判定委托给 `ETagGenerator::matchIfRange(...)`

### `Http2ErrorCode`

来源：`galay-http/protoc/galay-http2/http2_base.h`、`galay-http/protoc/galay-http2/http2_base.cc`

```cpp
enum class Http2ErrorCode : uint32_t {
    NoError = 0x0,
    ProtocolError = 0x1,
    InternalError = 0x2,
    FlowControlError = 0x3,
    SettingsTimeout = 0x4,
    StreamClosed = 0x5,
    FrameSizeError = 0x6,
    RefusedStream = 0x7,
    Cancel = 0x8,
    CompressionError = 0x9,
    ConnectError = 0xa,
    EnhanceYourCalm = 0xb,
    InadequateSecurity = 0xc,
    Http11Required = 0xd
};

std::string http2ErrorCodeToString(Http2ErrorCode code);
```

`http2ErrorCodeToString(...)` 的真实映射是：

- `NoError -> "NO_ERROR"`
- `ProtocolError -> "PROTOCOL_ERROR"`
- `InternalError -> "INTERNAL_ERROR"`
- `FlowControlError -> "FLOW_CONTROL_ERROR"`
- `SettingsTimeout -> "SETTINGS_TIMEOUT"`
- `StreamClosed -> "STREAM_CLOSED"`
- `FrameSizeError -> "FRAME_SIZE_ERROR"`
- `RefusedStream -> "REFUSED_STREAM"`
- `Cancel -> "CANCEL"`
- `CompressionError -> "COMPRESSION_ERROR"`
- `ConnectError -> "CONNECT_ERROR"`
- `EnhanceYourCalm -> "ENHANCE_YOUR_CALM"`
- `InadequateSecurity -> "INADEQUATE_SECURITY"`
- `Http11Required -> "HTTP_1_1_REQUIRED"`

当 RAG 问题已经落到“解析失败 / 升级失败 / path 安全 / range / hpack / frame”这类底层细节时，优先回到这些公开头：

- HTTP/1.x：`galay-http/protoc/galay-http/http_error.h`、`http_parse_utils.h`、`http_header.h`
- HTTP/2：`galay-http/protoc/galay-http2/http2_error.h`、`http2_frame.h`、`http2_hpack.h`
- WebSocket：`galay-http/protoc/websocket/ws_error.h`、`web_socket_frame.h`
- 静态文件与安全边界：`http_range.h`、`http_e_tag.h`、`static_file_config.h`
- 请求/响应快速构造：`galay-http/builder/galay-http/http_builder.h`

这些头文件属于公开面，但不在主工作流里逐一展开。它们是回答协议级细节问题时的 canonical source。

## 交叉验证入口

- HTTPS 客户端示例：`examples/include/e6_https.cpp`
- WSS 客户端示例：`examples/include/e8_wss.cpp`
- h2c 客户端示例：`examples/include/e10_h2cecho.cpp`
- h2 客户端示例：`examples/include/e14_h2echo.cpp`
- HTTPS 测试：`test/t21_https.cc`、`test/t22_https.cc`
- h2 测试：`test/t28_h2.cc`、`test/t29_h2.cc`

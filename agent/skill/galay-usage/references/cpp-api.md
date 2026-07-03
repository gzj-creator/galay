# galay C++ API 参考

13 个 C++ 模块的公开类型、方法签名与最小示例。每个模块开头列出 CMake target、include 前缀、命名空间。签名摘自头文件；深入用法见 `docs/cpp/modules/<name>/` 与 `examples/cpp/<name>/`。

> 通用规则：IO 操作返回 awaitable，`co_await` 得 `std::expected<...>`；`if (!r)` 判失败，`r.error().message()` 取原因。客户端形态 `Builder().scheduler(sched).build()` → `connect` → `<op>` → `close`。

---

## 核心层

### galay-kernel
- **用途**: C++23 协程运行时内核——多调度器 Runtime、`Task<T>` 协程、异步 TCP/UDP/文件 IO、通道与同步原语、定时器。
- **CMake link**: `galay::kernel`（异步 socket 类型在 `galay::async` 命名空间，同库提供，无独立 target）
- **头文件前缀**: `<galay/cpp/galay-kernel/...>`（如 `core/runtime.h`、`core/task.h`、`async/tcp_socket.h`）
- **命名空间**: `galay::kernel`（Runtime/Task/调度器/Host/通道/mutex/sleep），`galay::async`（TcpSocket/UdpSocket/AsyncFile）
- **核心类型与接口**:
  - `RuntimeBuilder` — `RuntimeBuilder& ioSchedulerCount(size_t)`、`computeSchedulerCount(size_t)`、`sequentialAffinity(size_t ioCount, size_t computeCount)`、`bool customAffinity(std::vector<uint32_t> io, std::vector<uint32_t> compute)`、`Runtime build() const`
  - `Runtime` — `std::expected<void, RuntimeError> start()`、`void stop()`、`template<T> std::expected<T, RuntimeError> blockOn(Task<T>)`、`template<T> std::expected<JoinHandle<T>, RuntimeError> spawn(Task<T>)`、`template<F> std::expected<JoinHandle<...>, RuntimeError> spawnBlocking(F&&)`、`RuntimeHandle handle()`、`IOScheduler* getNextIOScheduler()`
  - `RuntimeHandle` — `static std::expected<RuntimeHandle, RuntimeError> current()`、`static std::optional<RuntimeHandle> tryCurrent()`、`spawn(...)`、`spawnBlocking(...)`
  - `Task<T>` — 协程返回类型，只移动；`auto operator co_await()`；`bool done()`
  - `JoinHandle<T>` — `std::expected<T, TaskResultError> join()`、`std::expected<void, TaskResultError> wait() const`
  - `galay::async::TcpSocket` — `static std::expected<TcpSocket, IOError> create(IPType = IPV4)`；`std::expected<void, IOError> bind(const Host&)`、`listen(int backlog = 128)`；`HandleOption option()`（`handleReuseAddr()`/`handleNonBlock()`）；`co_await`: `accept(Host*)→GHandle`、`connect(const Host&)`、`recv(char*, size_t)→size_t`、`send(const char*, size_t)→size_t`、`readv/writev(std::span<const iovec>)`、`sendfile(int fd, off_t, size_t)`、`close()`；也可 `TcpSocket(GHandle)` 从已接受 fd 构造
  - `galay::async::UdpSocket` — `static std::expected<UdpSocket, IOError> create(IPType)`、`bind(const Host&)`；`co_await`: `recvfrom(char*, size_t, Host* from)`、`sendto(const char*, size_t, const Host& to)`、`close()`
  - `Host` / `IPType{IPV4,IPV6}` — `Host(IPType, const std::string& ip, uint16_t port)`、`bool valid()`、`std::string ip()`、`uint16_t port()`
  - `galay::async::AsyncFile` — `std::expected<void, IOError> open(const std::string&, FileOpenMode, int perm = 0644)`、`co_await read/write/close`、`std::expected<size_t, IOError> size()`、`sync()`
  - `MpscChannel<T>` — `bool send(T&&)`/`sendBatch(...)`；`co_await recv()→std::expected<T, IOError>`、`recvBatch(size_t)`（可 `.timeout(...)`）；`std::optional<T> tryRecv()`、`size_t size()`
  - `AsyncMutex` — `lock()`（`co_await`→`std::expected<void, IOError>`，可 `.timeout(ms)`）、`void unlock()`、`bool isLocked()`
  - `sleep(Duration)` — `co_await sleep(std::chrono::milliseconds(...))`
  - `IOError` — `code()`、`std::string message()`；错误码 `kTimeout`、`kConnectFailed` 等
- **最小示例**:
```cpp
#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/core/task.h>
using namespace galay::async;
using namespace galay::kernel;

Task<void> echoServer() {
    TcpSocket listener;
    listener.option().handleReuseAddr();
    listener.option().handleNonBlock();
    if (!listener.bind(Host(IPType::IPV4, "127.0.0.1", 9082))) co_return;
    if (!listener.listen(16)) co_return;

    Host peer;
    auto accepted = co_await listener.accept(&peer);
    if (!accepted) { co_await listener.close(); co_return; }

    TcpSocket client(accepted.value());
    client.option().handleNonBlock();
    char buf[256]{};
    auto n = co_await client.recv(buf, sizeof(buf));
    if (n && *n > 0) co_await client.send(buf, *n);
    co_await client.close();
    co_await listener.close();
}
```
- **文档**: `docs/cpp/modules/kernel/`（协程 11、网络IO 12、文件IO 13、并发 14、定时器 15、环形缓冲 16、Runtime 18、异步同步原语 20）

### galay-utils
- **用途**: 无依赖头文件工具集——字符串/时间/随机、缓存（LRU/环形缓冲）、编码（Base64）、加密（SHA1/SHA256/MD5/HMAC/PBKDF2/MurmurHash3）、算法（一致性哈希/布隆过滤器/Trie/Huffman/MVCC）、限流/熔断/负载均衡、配置解析。
- **CMake link**: `galay::utils`
- **头文件前缀**: `<galay/cpp/galay-utils/...>`；一站式总头 `<galay/cpp/galay-utils/galay_utils.hpp>`
- **命名空间**: `galay::utils`
- **核心类型与接口**:
  - `StringUtils`（`core/string.hpp`）— `static std::vector<std::string> split(std::string_view, char)`、`join(const std::vector<std::string>&, std::string_view)`、`trim/trimLeft/trimRight(std::string_view)`
  - `LruCache<Key, Value>`（`cache/lru_cache.hpp`）— `put(K&&, V&&)` / `put(..., ttl)`、`Value* get(const Key&)`、`EvictCallback`、`Stats`；不可拷贝/移动
  - `RingBuffer`（`cache/ring_buffer.hpp`）、`Bytes`（`cache/bytes.hpp`，`data()`/`size()`/`toStringView()`）
  - `Base64Util`（`encoding/base64.hpp`）— `static std::string Base64Encode(std::string const&, bool url=false)`、`Base64Decode(std::string const&, bool remove_linebreaks=false)`
  - 加密 — `SHA1::hashHex(const std::string&)`、`SHA256`、`HMAC::hmacSha256Hex(const std::string& key, const std::string& data)`、`MD5`、`PBKDF2`、`MurmurHash3::hash32(const std::string&, uint32_t seed=0)`
  - `TokenBucketLimiter`（`tool/rate_limiter.hpp`）— `TokenBucketLimiter(double rate, size_t capacity)`、`bool tryAcquire(size_t tokens=1)`、`double availableTokens() const`；另有 `CountingSemaphore`
  - `CircuitBreaker`（`tool/circuit_breaker.hpp`）— `explicit BasicCircuitBreaker(CircuitBreakerConfig = {})`、`bool allowRequest()`、`onSuccess()`、`onFailure()`、`template<F> auto execute(F&&)`（熔断打开返回 `std::unexpected(CircuitBreakerError::Open)`）、`executeWithFallback(F&&, Fallback&&)`、`CircuitState state()`
  - 负载均衡（`tool/balancer.hpp`）— `RoundRobinLoadBalancer<T>` / `WeightRoundRobinLoadBalancer<T>` / `RandomLoadBalancer<T>` / `WeightedRandomLoadBalancer<T>`：`std::optional<T> select()`、`void append(T[, uint32_t weight])`、`size_t size()`
  - `ConsistentHash`、`BloomFilter`、`Trie`、`Huffman`、`MVCC`（`algorithm/`）
  - 配置（`config/parser_manager.hpp`）— `ParserManager::instance().createParser(const std::string& path)`（按 `.conf/.ini/.env/.toml` 分发）；解析器 `bool parseFile(...)`、`std::optional<std::string> getValue(key)`、`get<T>(key)`
  - `System`（`process/system.hpp`）— `System::cpuCount()` 等
- **最小示例**:
```cpp
#include <galay/cpp/galay-utils/galay_utils.hpp>
using namespace galay::utils;
auto parts = StringUtils::split("a,b,c", ',');
TokenBucketLimiter limiter(100.0, 200);
if (limiter.tryAcquire(1))
    auto sig = SHA1::hashHex("payload");
auto b64 = Base64Util::Base64Encode("data");
```
- **文档**: `docs/cpp/modules/utils/`

### galay-ssl
- **用途**: 基于 OpenSSL Memory-BIO 的异步 SSL/TLS——协程友好的 `SslSocket` 与共享 `SslContext`，复用 galay-kernel 的 IO 调度器。
- **CMake link**: `galay::ssl`（依赖 `galay::kernel` + `galay::utils`）
- **头文件前缀**: `<galay/cpp/galay-ssl/...>`（`async/ssl_socket.h`、`ssl/ssl_context.h`）
- **命名空间**: `galay::ssl`（内部 `using namespace galay::kernel`，复用 `Host`/`IPType`/`IOError`/`Task`）
- **核心类型与接口**:
  - `SslContext` — `explicit SslContext(SslMethod)`；`std::expected<void, SslError> loadCertificate(const std::string&, SslFileType=PEM)`、`loadCertificateChain(...)`、`loadPrivateKey(const std::string&, SslFileType=PEM)`、`loadCACertificate(...)`、`loadCAPath(...)`、`useDefaultCA()`、`setCiphers(...)`、`setCiphersuites(...)`、`setALPNProtocols(const std::vector<std::string>&)`；`void setVerifyMode(SslVerifyMode, ...)`、`setVerifyDepth(int)`、`setMin/MaxProtocolVersion(int)`；`bool isValid()`、`SSL_CTX* native()`
  - `SslSocket` — `SslSocket(SslContext*, IPType=IPV4)` / `SslSocket(SslContext*, GHandle)`；`bind(const Host&)`、`listen(int=128)`；`HandleOption option()`；`std::expected<void, SslError> setHostname(const std::string&)`（SNI）；`co_await`: `accept(Host*)→GHandle`、`connect(const Host&)`、`handshake()→std::expected<void, SslError>`、`recv(char*, size_t)→std::expected<Bytes, SslError>`、`send(const char*, size_t)→std::expected<size_t, SslError>`、`shutdown()`、`close()`；`bool isHandshakeCompleted()`、`std::string getProtocolVersion()/getCipher()/getALPNProtocol()`、Session 复用 `getSession()/setSession(...)/isSessionReused()`
  - 枚举 — `SslMethod{TLS_Server, TLS_Client, ...}`、`SslVerifyMode{None, Peer, FailIfNoPeerCert, ClientOnce}`、`SslFileType{PEM, ASN1}`、`SslError`
  - **注意**: 同一 `SslSocket` 须由单一 owner 串行驱动，勿并发挂多个 SSL 状态机 awaitable。
- **最小示例**:
```cpp
#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
using namespace galay::ssl;

Task<void> handleClient(SslContext* ctx, GHandle h) {
    SslSocket client(ctx, h);
    client.option().handleNonBlock();
    if (auto hs = co_await client.handshake(); !hs) { co_await client.close(); co_return; }
    char buf[4096];
    auto recv = co_await client.recv(buf, sizeof(buf));
    if (recv && recv->size() > 0)
        co_await client.send(reinterpret_cast<const char*>(recv->data()), recv->size());
    co_await client.shutdown();
    co_await client.close();
}
// SslContext ctx(SslMethod::TLS_Server); ctx.loadCertificate("s.crt"); ctx.loadPrivateKey("s.key");
```
- **文档**: `docs/cpp/modules/ssl/`

---

## Web 协议层

### galay-http
- **用途**: 基于 C++23 协程的 HTTP/1.1 服务端与客户端，支持路由模式、自定义连接处理器与 HTTPS/TLS。
- **CMake link**: `galay::http`
- **头文件前缀**: `<galay/cpp/galay-http/...>`（`server/http_server.h`、`server/http_router.h`、`builder/http_builder.h`、`client/http_client.h`、`protoc/http_request.h`）
- **命名空间**: `galay::http`（协程/Runtime 类型来自 `galay::kernel`）
- **核心类型与接口**:
  - `HttpServerBuilder`：`host(std::string)`、`port(uint16_t)`、`backlog(int)`、`ioSchedulerCount(size_t)`、`computeSchedulerCount(size_t)`、`policy(HttpServerPolicy)`、`HttpServer build() const`
  - `HttpServer`：`void start(HttpConnHandler)`（自定义连接处理）、`void start(HttpRouter&&)`（路由模式，框架驱动读取/Keep-Alive/分发）、`void stop()`、`bool isRunning()`
  - `HttpsServerBuilder` / `HttpsServer`：额外 `certPath` / `keyPath` / `caPath` / `verifyPeer(bool)` / `verifyDepth(int)`
  - `HttpRouter`：`template<HttpMethod... Methods> void addHandler(const std::string& path, HttpRouteHandler)`（如 `addHandler<HttpMethod::GET>`、`addHandler<HttpMethod::GET, HttpMethod::POST>`）、`bool delHandler(...)`、`void clear()`、`mount(...)`/`mountHardly(...)`（静态文件）、`tryFiles(...)`、`proxy(...)`
  - `HttpRouteHandler = std::function<Task<void>(HttpConn&, HttpRequest)>`
  - `HttpConn`：`HttpWriter getWriter()`、`HttpReader getReader(const HttpReaderSetting& = {})`、`auto close()`
  - `HttpWriter`（`co_await` 得 `std::expected<bool, HttpError>`）：`sendResponse(HttpResponse&)`、`sendRequest(HttpRequest&)`、`send(std::string&&)`、`sendChunk(const std::string&, bool is_last=false)`；`HttpReader`：`getRequest(HttpRequest&)`、`getResponse(HttpResponse&)`
  - `HttpRequest`：`getBodyStr()`、`header().uri()`、`header().method()`；`HttpResponse`：`header().code()`、`getBodyStr()`、`setBodyStr(std::string&&)`
  - `Http1_1ResponseBuilder`：静态 `ok()`/`created()`/`noContent()`/`badRequest()`/`notFound()`/`internalServerError()`；链式 `status(int)`、`header(k,v)`、`contentType(...)`、`text(...)`、`html(...)`、`json(...)`、`body(...)`；`HttpResponse build()`
  - `Http1_1RequestBuilder`：静态 `get/post/put/del/patch/head/options(uri)`；链式 `method`/`uri`/`header`/`host`/`contentType`/`body`/`json`/`form`；`build()`
  - `HttpClient`（`HttpClientBuilder().build()`）：`Task<std::expected<void, IOError>> connect(const std::string& url)`、`socket()`、`getSession(...)`、`close()`
  - `HttpSession`：`sendRequest(HttpRequest&)`、`getResponse(HttpResponse&)`；便捷 `co_await session.get/post(path, body, contentType, headers)` → `std::expected<std::optional<HttpResponse>, HttpError>`
- **最小示例**:
```cpp
#include <galay/cpp/galay-http/server/http_server.h>
#include <galay/cpp/galay-http/server/http_router.h>
#include <galay/cpp/galay-http/builder/http_builder.h>
using namespace galay::http;
using namespace galay::kernel;

Task<void> echoHandler(HttpConn& conn, HttpRequest req) {
    std::string body = req.getBodyStr();
    auto response = Http1_1ResponseBuilder::ok()
        .header("Server", "Galay-HTTP-Echo/1.0")
        .text(body.empty() ? "Echo: (empty body)" : "Echo: " + body)
        .build();
    auto writer = conn.getWriter();
    auto result = co_await writer.sendResponse(response);  // std::expected<bool, HttpError>
    if (!result) std::cerr << "send failed: " << result.error().message() << "\n";
    co_return;  // 路由模式下框架按 Keep-Alive 决定关闭
}

int main() {
    HttpRouter router;
    router.addHandler<HttpMethod::POST>("/echo", echoHandler);
    HttpServer server(HttpServerBuilder().host("0.0.0.0").port(8080).backlog(128).build());
    server.start(std::move(router));
    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
}
```
- **文档**: `docs/cpp/modules/http/`

### galay-http2
- **用途**: 基于 C++23 协程的 HTTP/2 服务端与客户端，支持 h2c（明文，Upgrade / prior-knowledge）与 h2（TLS + ALPN），帧级流处理、流控、HPACK。
- **CMake link**: `galay::http2`
- **头文件前缀**: `<galay/cpp/galay-http2/...>`（`server/http2_server.h`、`client/h2c_client.h`、`client/h2_client.h`、`protoc/http2_hpack.h`）
- **命名空间**: `galay::http2`
- **核心类型与接口**:
  - `H2cServerBuilder`：`host/port/backlog/ioSchedulerCount/computeSchedulerCount`、`maxConcurrentStreams(uint32_t)`、`initialWindowSize`、`maxFrameSize`、`maxHeaderListSize`、`enablePush(bool)`、`pingEnabled/pingInterval/pingTimeout`、`streamHandler(Http2ConnectionHandler)`、`staticResponse(path, H2StaticResponse)`、`staticFiles(prefix, H2StaticFileConfig)`、`H2cServer build() const`
  - `H2cServer`：`void start()`、`void start(Http2ConnectionHandler)`、`void stop()`
  - `H2ServerBuilder` / `H2Server`（TLS h2）：额外 `certPath` / `keyPath` / `caPath` / `verifyPeer` / `verifyDepth`
  - `Http2ConnectionHandler = std::function<Task<void>(Http2Stream::ptr)>`
  - `Http2Stream`（`::ptr` = shared_ptr）：
    - 读：`co_await stream->getFrame()` → `std::expected<Http2Frame::uptr, ...>`、`getFrames(size_t)`；帧 `isHeaders()`/`isData()`/`isEndStream()`
    - 请求/响应：`Http2Request& request()`（`req.body`、`req.takeSingleBodyChunk()`）、`response()`
    - 服务端应答：`co_await stream->replyHeader(const std::vector<Http2HeaderField>&, bool end_stream)`、`replyData(std::string, bool end_stream=false)`、`replyHeadersAndData(...)`
    - 客户端发送：`sendHeaders(...)`、`sendData(std::string, bool end_stream=false)`
    - `uint32_t streamId()`、`sendWindow()`/`recvWindow()`
  - `Http2Headers`（`protoc/http2_hpack.h`）：链式 `method/scheme/authority/path`、`status(int)`、`contentType(...)`、`contentLength(size_t)`、`server(...)`；`Http2HeaderField = {name, value}`
  - `H2cClient`（`H2cClientBuilder().build()`）：`co_await connect(host, uint16_t port)`、`co_await upgrade(path="/")`、`Http2Conn* getConn()`（`getConn()->streamManager()->allocateStream()`）、`co_await shutdown()`、`bool isUpgraded()`
  - `H2Client`（TLS，`H2ClientBuilder().caPath(...).build()`）：`co_await connect(host, uint16_t port=443)` → `std::expected<bool, Http2ErrorCode>`、`alpnProtocol()`、`co_await close()`
- **最小示例**:
```cpp
#include <galay/cpp/galay-http2/server/http2_server.h>
using namespace galay::http2;
using namespace galay::kernel;

Task<void> handleStream(Http2Stream::ptr stream) {
    while (true) {
        auto fr = co_await stream->getFrame();
        if (!fr || !fr.value()) co_return;
        auto frame = std::move(fr.value());
        if ((frame->isHeaders() || frame->isData()) && frame->isEndStream()) break;
    }
    auto& req = stream->request();
    co_await stream->replyHeader(
        Http2Headers().status(200).contentType("text/plain").contentLength(req.body.size()),
        req.body.empty());
    if (!req.body.empty()) co_await stream->replyData(req.takeSingleBodyChunk(), true);
    co_return;
}

int main() {
    H2cServer server(H2cServerBuilder().host("0.0.0.0").port(8080)
        .ioSchedulerCount(4).maxConcurrentStreams(100).enablePush(false).build());
    server.start(handleStream);
    while (true) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
```
- **文档**: `docs/cpp/modules/http2/`

### galay-ws (websocket)
- **用途**: 基于 C++23 协程的 WebSocket（RFC 6455）服务端升级与客户端，支持 Text/Binary/Ping/Pong/Close 帧、消息级读写与 WSS（TLS）。
- **CMake link**: `galay::ws`（CMake target 是 `galay::ws`；C++ 命名空间是 `galay::websocket`）
- **头文件前缀**: `<galay/cpp/galay-ws/...>`（`server/ws_upgrade.h`、`kernel/ws_conn.h`、`kernel/writer_cfg.h`、`client/ws_client.h`、`client/ws_session.h`）；服务端升级常配合 `<galay/cpp/galay-http/...>`
- **命名空间**: `galay::websocket`（HTTP 类型来自 `galay::http`）
- **核心类型与接口**:
  - `WsUpgrade`（服务端）：静态 `WsUpgradeResult handleUpgrade(HttpRequest& request)`；`WsUpgradeResult { bool success; HttpResponse response; }`（把 `response` 用 HTTP writer 发出即完成 101）
  - `WsConn`：静态 `WsConn from(HttpConn&& http_conn, bool is_server=true)`、`WsReader getReader(const WsReaderSetting& = {})`、`WsWriter getWriter(WsWriterSetting)`、`auto close()`
  - `WsWriter`（`co_await` 返回 `std::expected<bool, WsError>`）：`sendText(std::string, bool fin=true)`、`sendBinary(...)`、`sendPing(data="")`、`sendPong(data="")`、`sendClose(WsCloseCode=Normal, reason="")`、`sendFrame(WsFrame)`
  - `WsReader`：`co_await reader.getMessage(std::string& message, WsOpcode& opcode)` → `std::expected<bool, WsError>`（`true`=消息完整；错误含 `kWsConnectionClosed`）；`getFrame(WsFrame&)`；可链式 `.timeout(1000ms)`
  - `WsWriterSetting`：`static byServer()`（不掩码）、`byClient()`（客户端掩码）；`WsReaderSetting`：`max_frame_size`、`max_message_size`
  - `WsOpcode`：`Continuation/Text/Binary/Close/Ping/Pong`
  - `WsClient`（`WsClientBuilder().build()`；WSS 用 `WssClient`）：`co_await connect(const std::string& url)`、`getSession(const WsWriterSetting&, size_t ring_buffer_size, const WsReaderSetting&)`、`co_await close()`
  - `WsSession`：`auto upgrade()` 得 upgrader，`co_await upgrader()` → `std::expected<bool, WsError>`；`getReader()`、`getWriter()`、便捷 `sendText(...)`
- **最小示例（客户端）**:
```cpp
#include <galay/cpp/galay-ws/client/ws_client.h>
#include <galay/cpp/galay-ws/kernel/writer_cfg.h>
using namespace galay::websocket;
using namespace galay::kernel;

Task<bool> runWsClient(const std::string& url) {
    auto client = WsClientBuilder().build();
    if (!(co_await client.connect(url))) co_return false;
    WsReaderSetting rs; rs.max_frame_size = 1 << 20; rs.max_message_size = 10 << 20;
    auto session_result = client.getSession(WsWriterSetting::byClient(), 8192, rs);
    if (!session_result) co_return false;
    auto& session = *session_result.value();
    auto up = co_await session.upgrade()();
    if (!up || !up.value()) co_return false;
    auto reader = session.getReader();
    auto writer = session.getWriter();
    if (!(co_await writer.sendText("Hello"))) { co_await client.close(); co_return false; }
    std::string msg; WsOpcode op;
    while (true) {
        auto r = co_await reader.getMessage(msg, op);
        if (!r) { co_await client.close(); co_return false; }
        if (r.value() && (op == WsOpcode::Text || op == WsOpcode::Binary)) break;
    }
    co_await writer.sendClose();
    co_await client.close();
    co_return true;
}
```
- **文档**: `docs/cpp/modules/ws/`

---

## 数据客户端层

> 数据类客户端普遍为异步 + 连接池；`await_resume` 常返回 `std::expected<std::optional<...>, Error>`：**外层判 IO 错误，内层 `has_value()` 判是否有结果**。多数模块另有 `sync/` 同步变体。

### galay-redis
- **用途**: 高性能异步 Redis/Rediss(TLS) 客户端，支持单命令、Pipeline、Pub/Sub 与连接池。
- **CMake link**: `galay::redis`；**头文件前缀** `<galay/cpp/galay-redis/...>`（`async/redis_client.h`、`async/conn_pool.h`）；**命名空间** `galay::redis`
- **核心类型与接口**:
  - 结果别名: `RedisResult = std::expected<std::vector<RedisValue>, RedisError>`；`RedisVoidResult = std::expected<void, RedisError>`
  - `RedisClientBuilder` / `RedissClientBuilder`（TLS）：`.scheduler(IOScheduler*)`、`.config(AsyncRedisConfig)`、`.sendTimeout(ms)`、`.recvTimeout(ms)`、`.bufferSize(size_t)`；Rediss 额外 `.tlsConfig(...)`、`.caPath()`、`.verifyPeer(bool)`、`.serverName()`；`RedisClient build() const`
  - `RedisClient`（`co_await`）：`connect(const std::string& url)` / `connect(ip, port)`、`command(RedisEncodedCommand)`、`commandBorrowed(const RedisBorrowedCommand&)`、`batch(std::span<const RedisCommandView>)`（Pipeline）、`close()`、`bool isClosed()`
  - `RedisCommandBuilder`：`get/set/del`、`auth(pwd)` / `auth(user,pwd)`、`select(db)`、`ping()`、`publish(channel,msg)`、`subscribe()/psubscribe()`、`clusterInfo()`、`clusterNodes()`；`commands()`（供 `batch`）、`build()`→`RedisEncodedCommand`
  - `RedisValue`：`isStatus()/isError()/isInteger()/toInteger()/isString()/toString()/isArray()/toArray()/toMap()/toSet()`
  - 连接池 `RedisConnectionPool`：`initialize()`、`acquire()`（→ `std::expected<std::shared_ptr<PooledConnection>, RedisError>`）、`release(...)`；配置 `ConnectionPoolConfig`（`min/max/initial_connections`、`acquire_timeout`）
  - 同步变体 `sync/redis_session.h`（`RedisSession`）
- **最小示例**:
```cpp
#include <galay/cpp/galay-redis/async/redis_client.h>
using namespace galay::kernel;
using namespace galay::redis;

Task<void> demo(IOScheduler* scheduler, std::string host, int port) {
    auto client = RedisClientBuilder().scheduler(scheduler).build();
    RedisCommandBuilder cb;
    auto conn = co_await client.connect(host, port).timeout(std::chrono::seconds(5));
    if (!conn) { std::cerr << conn.error().message() << '\n'; co_return; }
    if (!(co_await client.command(cb.set("k", "v")))) { (void)co_await client.close(); co_return; }
    auto get = co_await client.command(cb.get("k"));
    if (get && get->has_value()) {
        const auto& vals = get->value();
        if (!vals.empty() && vals[0].isString()) std::cout << vals[0].toString() << '\n';
    }
    (void)co_await client.close();
}
```
- **文档**: `docs/cpp/modules/redis/`

### galay-mysql
- **用途**: 异步 MySQL 客户端，原生协议实现，支持普通查询、预处理语句、Pipeline、事务与连接池。
- **CMake link**: `galay::mysql`；**头文件前缀** `<galay/cpp/galay-mysql/...>`（`async/client.h`、`async/conn_pool.h`）；**命名空间** `galay::mysql`
- **核心类型与接口**:
  - 结果别名: `MysqlResult = std::expected<MysqlResultSet, MysqlError>`；Awaitable `await_resume` → `std::expected<std::optional<...>, MysqlError>`
  - `AsyncMysqlClientBuilder`：`.scheduler(IOScheduler*)`、`.config(AsyncMysqlConfig)`、`.sendTimeout(ms)`、`.recvTimeout(ms)`、`.bufferSize(size_t)`、`.resultRowReserveHint(size_t)`；`build()`
  - `AsyncMysqlClient`（`co_await`）：`connect(MysqlConfig)` / `connect(host, port, user, password, database="")`、`query(std::string_view sql)`、`prepare(std::string_view sql)`→`PrepareResult{stmt_id}`、`stmtExecute(uint32_t stmt_id, ...)`、`pipeline(std::span<const std::string_view>)`、`beginTransaction()`/`commit()`/`rollback()`、`ping()`、`useDatabase(...)`、`close()`
  - `MysqlResultSet`：`fieldCount()`、`field(i)`/`fields()`（`MysqlField::name()`、`type()`）、`rowCount()`、`row(i)`/`rows()`、`affectedRows()`、`lastInsertId()`
  - `MysqlRow`：`size()`、`at(i)`（`std::optional<std::string>`）、`isNull(i)`、`getString(i, def="")`、`getInt64(i, def)`、`getUint64(i, def)`、`getDouble(i, def)`
  - 配置: `MysqlConfig`（`host`/`port=3306`/`username`/`password`/`database`/`charset="utf8mb4"`/`connect_timeout_ms`）、`AsyncMysqlConfig`
  - 连接池 `MysqlConnectionPool`：`acquire()`、`acquireLease()`（RAII `MysqlPoolLease` 析构自动归还）、`release(...)`
  - 同步变体 `sync/mysql_client.h`
- **最小示例**:
```cpp
#include <galay/cpp/galay-mysql/async/client.h>
using namespace galay::kernel;
using namespace galay::mysql;

Task<void> run(IOScheduler* scheduler, const DbCfg& cfg) {
    auto client = AsyncMysqlClientBuilder().scheduler(scheduler).build();
    auto conn = co_await client.connect(cfg.host, cfg.port, cfg.user, cfg.password, cfg.database);
    if (!conn || !conn->has_value()) { co_return; }
    auto q = co_await client.query("SELECT 1");
    if (!q || !q->has_value()) { co_await client.close(); co_return; }
    const MysqlResultSet& rs = q->value();
    if (rs.rowCount() > 0) std::cout << rs.row(0).getString(0) << '\n';
    co_await client.close();
}
```
- **文档**: `docs/cpp/modules/mysql/`

### galay-mongo
- **用途**: 异步 MongoDB 客户端，原生 OP_MSG/BSON，通过命令文档执行任意命令、ping 与 Pipeline；CRUD 便捷方法在同步客户端上提供。
- **CMake link**: `galay::mongo`；**头文件前缀** `<galay/cpp/galay-mongo/...>`（`async/client.h`、`base/mongo_value.h`、`protoc/bson.h`）；**命名空间** `galay::mongo`
- **核心类型与接口**:
  - 协程别名（`co_await` 后即得 `std::expected`）：`MongoConnectAwaitable = Task<std::expected<bool, MongoError>>`、`MongoCommandAwaitable = Task<std::expected<MongoReply, MongoError>>`、`MongoPipelineAwaitable = Task<std::expected<std::vector<MongoPipelineResponse>, MongoError>>`
  - `AsyncMongoClientBuilder`：`.scheduler(IOScheduler*)`、`.config(AsyncMongoConfig)`、`.sendTimeout(ms)`、`.recvTimeout(ms)`、`.bufferSize(size_t)`；`build()`
  - `AsyncMongoClient`：`connect(MongoConfig)` / `connect(host, port, database="admin")`、`command(std::string database, MongoDocument command)`、`ping(database="admin")`、`pipeline(std::string database, std::span<const MongoDocument>)`、`close()`
  - `MongoDocument`：`append(std::string key, MongoValue)`、`set(...)`、`at(key)`、`getString/getInt32/getInt64/getDouble/getBool(key, default)`、`size()`、`fields()`；`MongoArray`、`MongoValue`（`fromObjectId`、`toDocument()`）
  - `MongoReply`：`document()`、`bool ok()`；BSON `protocol::BsonCodec`（`encodeDocument`/`decodeDocument`）
  - 配置: `MongoConfig`（`host`/`port=27017`/`seeds`/`username`/`password`/`database`/`auth_database`/`app_name`）
  - 同步变体 `sync/mongo_client.h`（`MongoClient`）：`findOne/insertOne/updateOne/deleteOne(db, coll, ...)` 等 CRUD 便捷方法
- **最小示例**:
```cpp
#include <galay/cpp/galay-mongo/async/client.h>
using namespace galay::kernel;
using namespace galay::mongo;

Task<void> run(IOScheduler* scheduler, const MongoConfig& mcfg) {
    auto client = AsyncMongoClientBuilder().scheduler(scheduler).build();
    auto conn = co_await client.connect(mcfg);
    if (!conn) { std::cerr << conn.error().message() << '\n'; co_return; }
    MongoDocument doc;  doc.append("_id", int64_t(1)); doc.append("name", "galay");
    MongoArray docs;    docs.append(std::move(doc));
    MongoDocument insert; insert.append("insert", "mycoll"); insert.append("documents", std::move(docs));
    auto res = co_await client.command(mcfg.database, std::move(insert));
    if (!res) std::cerr << "insert failed: " << res.error().message() << '\n';
    co_await client.close();
}
```
- **文档**: `docs/cpp/modules/mongo/`

### galay-etcd
- **用途**: 异步 etcd v3 客户端（HTTP/JSON gateway），支持 KV put/get/del、租约与续期、Pipeline 事务及 key watch。
- **CMake link**: `galay::etcd`；**头文件前缀** `<galay/cpp/galay-etcd/...>`（`async/client.h`、`base/etcd_types.h`、`cluster/etcd_cluster_client.h`）；**命名空间** `galay::etcd`
- **核心类型与接口**:
  - 结果别名: `EtcdBoolResult`、`EtcdGetResult = std::expected<std::vector<EtcdKeyValue>, EtcdError>`、`EtcdDeleteResult = std::expected<int64_t, EtcdError>`、`EtcdLeaseGrantResult = std::expected<int64_t, EtcdError>`、`EtcdPipelineResult`
  - `AsyncEtcdClientBuilder`：`.scheduler(IOScheduler*)`、`.endpoint(std::string)`、`.apiPrefix(std::string)`、`.requestTimeout(ms)`、`.config(EtcdConfig)`、`.productionConfig(EtcdProductionConfig)`；`build()`
  - `AsyncEtcdClient`（`co_await`，直接得 `std::expected`）：`connect()`、`close()`、`put(key, value, std::optional<int64_t> lease_id=nullopt)`、`get(key, bool prefix=false, std::optional<int64_t> limit=nullopt)`、`del(key, bool prefix=false)`、`grantLease(int64_t ttl_seconds)`、`keepAliveOnce(int64_t lease_id)`、`pipeline(std::span<const PipelineOp>)`、`watch(key, WatchTaskHandler)`（`Task<void>(EtcdWatchResponse)`）
  - `PipelineOp`：`static Put(key,value,lease_id?)` / `Get(key,prefix?,limit?)` / `Del(key,prefix?)`（枚举 `PipelineOpType{Put,Get,Delete}`）
  - `EtcdKeyValue`：`key`、`value`、`create_revision`、`mod_revision`、`version`、`lease`
  - 配置: `EtcdConfig`（`endpoint="http://127.0.0.1:2379"`、`api_prefix="/v3"`）
  - 同步变体 `sync/etcd_client.h`（`EtcdClient`）；集群客户端 `cluster/etcd_cluster_client.h`
- **最小示例**:
```cpp
#include <galay/cpp/galay-etcd/async/client.h>
using namespace galay::kernel;

Task<void> run(IOScheduler* scheduler, std::string endpoint, std::string key, std::string value) {
    galay::etcd::EtcdConfig cfg; cfg.endpoint = endpoint;
    auto client = galay::etcd::AsyncEtcdClientBuilder().scheduler(scheduler).config(cfg).build();
    if (auto conn = co_await client.connect(); !conn) { std::cerr << conn.error().message() << '\n'; co_return; }
    if (auto put = co_await client.put(key, value); !put) { std::cerr << put.error().message() << '\n'; co_return; }
    auto get = co_await client.get(key);
    if (get && !get->empty()) std::cout << get->front().key << " => " << get->front().value << '\n';
    (void)co_await client.del(key);
    (void)co_await client.close();
}
```
- **文档**: `docs/cpp/modules/etcd/`

---

## 服务化层

### galay-rpc
- **用途**: C++23 协程 RPC 框架，提供 unary + 四种流式调用、内置 Runtime 服务端、异步客户端、真实流协议与服务发现/负载均衡。
- **CMake link**: `galay::rpc`（`GALAY_RPC_ENABLE_ETCD=ON` 时含 etcd 服务发现）；**头文件前缀** `<galay/cpp/galay-rpc/...>`（`kernel/rpc_server.h`、`kernel/rpc_service.h`、`kernel/rpc_client.h`、`kernel/rpc_managed_client.h`、`discovery/rpc_discovery.h`）；**命名空间** `galay::rpc`
- **核心类型与接口**:
  - `RpcService`（`kernel/rpc_service.h`）：`explicit RpcService(std::string_view name)`；受保护注册 `registerMethod(name, handler)`（unary 别名）、`registerUnaryMethod / registerClientStreamingMethod / registerServerStreamingMethod / registerBidiStreamingMethod(name, handler)`、`registerStreamMethod(name, RpcStreamHandler)`，均有成员函数指针模板重载。`RpcMethodHandler = std::function<Task<void>(RpcContext&)>`、`RpcStreamHandler = std::function<Task<void>(RpcStream&)>`
  - `RpcContext`：`RpcRequest& request()`、`RpcResponse& response()`、`void setError(RpcErrorCode)`、`setPayload(const char*, size_t)` / `(const std::string&)` / `(std::vector<char>&&)` / `(const RpcPayloadView&)`
  - `RpcServer`（`kernel/rpc_server.h`）：`explicit RpcServer(const RpcServerConfig&)`、`void registerService(std::shared_ptr<RpcService>)`、`void start()`（非阻塞）、`void stop()`、`bool isRunning()`、`Runtime& runtime()`。`RpcServerBuilder`：`.host().port().backlog().ioSchedulerCount().computeSchedulerCount().ringBufferSize().build()`。`RpcServerConfig{host="0.0.0.0", port=9000, backlog=128, ring_buffer_size=8192}`
  - `RpcClient`（`kernel/rpc_client.h`）：`connect(const std::string& host, uint16_t port)`；unary `call(service, method, payload, len)` / `(service, method, const std::string&)`，可 `.timeout(ms)`；流式帧 `callClientStreamFrame(...)`、`callServerStreamRequest(...)`、`callBidiStreamFrame(...)`；真实流 `std::expected<RpcStream, RpcError> createStream(service, method)`；`close()`。返回 `CallResult = std::expected<std::optional<RpcResponse>, RpcError>`（`nullopt`=需继续 `co_await`）
  - `RpcManagedClient`（`kernel/rpc_managed_client.h`）：`explicit RpcManagedClientImpl(Discovery&, RpcManagedClientConfig={})`；`Task<RpcManagedCallResult> call(service, method, payload, len, options={})`；round-robin + 连接池 + 重试/熔断；`shutdown()`。`RpcStaticDiscovery::set(service, RpcEndpointList)`；`RpcEndpoint{host, port}`
  - `RpcStream`（`kernel/rpc_stream.h`）：`sendInit()`、`sendData(...)`、`sendEnd()`、`sendCancel()`、`read(StreamMessage&)`，均 `std::expected<bool, RpcError>` awaitable
  - 协议 `protoc/`：`RpcErrorCode`、`rpcErrorCodeToString(code)`、`RpcError{code(), message(), isOk(), static from(kernel::IOError, default)}`、`enum class RpcCallMode{UNARY, CLIENT_STREAMING, SERVER_STREAMING, BIDI_STREAMING}`
- **最小示例**:
```cpp
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
using namespace galay::rpc;
using namespace galay::kernel;

class EchoService : public RpcService {
public:
    EchoService() : RpcService("EchoService") { registerMethod("echo", &EchoService::echo); }
    Task<void> echo(RpcContext& ctx) { ctx.setPayload(ctx.request().payloadView()); co_return; }
};

int main() {
    RpcServerConfig config; config.port = 9000;
    RpcServer server(config);
    server.registerService(std::make_shared<EchoService>());
    server.start();
    while (server.isRunning()) std::this_thread::sleep_for(std::chrono::seconds(1));
}
```
- **文档**: `docs/cpp/modules/rpc/`

### galay-mcp
- **用途**: C++23 Model Context Protocol 实现，stdio 与 HTTP 两种传输的服务端/客户端，支持 tool/resource/prompt 注册与调用。
- **CMake link**: `galay::mcp`；**头文件前缀** `<galay/cpp/galay-mcp/...>`（`server/stdio_server.h`、`server/http_server.h`、`client/client.h`、`common/schema_builder.h`、`common/mcp_json.h`、`common/mcp_error.h`）；**命名空间** `galay::mcp`
- **核心类型与接口**:
  - `McpStdioServer`：`setServerInfo(name, version)`；`addTool(name, description, inputSchema, ToolHandler)`、`addResource(uri, name, description, mimeType, ResourceReader)`、`addPrompt(name, description, std::vector<PromptArgument>, PromptGetter)`；`void run()`（阻塞 stdin/stdout）、`stop()`、`isRunning()`。回调**同步**：`ToolHandler = std::function<std::expected<JsonString, McpError>(const JsonElement&)>`
  - `McpHttpServer`：`McpHttpServer(host="0.0.0.0", port=8080, ioSchedulers=8, computeSchedulers=0)`；同名 `setServerInfo/addTool/addResource/addPrompt`（须在 `start()` 前注册）；`void start()`（阻塞，监听 `POST /mcp`）。回调**协程**：`ToolHandler = std::function<kernel::Task<void>(const JsonElement&, std::expected<JsonString, McpError>&)>`（结果写回引用）
  - `McpClient`（构造决定模式）：
    - stdio：`explicit McpClient(McpStdioClientConfig={})`；同步 `initialize(name, version)`、`callTool(name, arguments)`、`listTools/listResources/listPrompts()`、`readResource(uri)`、`getPrompt(name, args)`、`ping()`、`disconnect()`
    - HTTP：`McpClient(kernel::Runtime&, McpHttpClientConfig{.url=url})`；`connect()`、协程 API 把结果写回引用 `initialize(name, version, result&)`、`callTool(name, args, result&)` 等
  - `SchemaBuilder`：链式 `addString/addNumber/addInteger/addBoolean(name, description, required=false)`、`addArray(...)`、`addObject(...)`、`addEnum(name, description, std::vector<std::string>, required=false)`、`JsonString build()`
  - JSON/错误：`JsonDocument::Parse(...)`、`JsonWriter`、`JsonHelper`；`enum class McpErrorCode`、`McpError`（工厂 `invalidParams/toolNotFound/notInitialized/...`）；协议常量 `MCP_VERSION="2024-11-05"`
- **最小示例**:
```cpp
#include <galay/cpp/galay-mcp/server/http_server.h>
#include <galay/cpp/galay-mcp/common/schema_builder.h>
using namespace galay::mcp;

int main() {
    McpHttpServer server("0.0.0.0", 8080);
    server.setServerInfo("example-http-server", "1.0.0");
    auto schema = SchemaBuilder().addNumber("a", "op1", true).addNumber("b", "op2", true).build();
    server.addTool("add", "sum", schema,
        [](const JsonElement& args, std::expected<JsonString, McpError>& result) -> galay::kernel::Task<void> {
            JsonObject obj;
            if (!JsonHelper::getObject(args, obj)) {
                result = std::unexpected(McpError(McpErrorCode::InvalidParams, "Invalid arguments"));
                co_return;
            }
            JsonWriter w; w.startObject(); w.key("result");
            w.number(obj["a"].get_double().value() + obj["b"].get_double().value());
            w.endObject();
            result = w.takeString();
            co_return;
        });
    server.start();  // 阻塞，监听 POST /mcp
}
```
- **文档**: `docs/cpp/modules/mcp/`

### galay-tracing
- **用途**: 轻量分布式追踪库：Span 数据模型、RAII span 作用域、采样器、W3C Trace Context 传播、OTLP/HTTP 导出与日志-链路关联。
- **CMake link**: `galay::tracing`（另有 header-only `galay::tracing-kernel`；spdlog sink 适配 `galay::tracing-spdlog`）；**头文件前缀** `<galay/cpp/galay-tracing/...>`（`kernel/span.h`、`kernel/span_guard.h`、`kernel/sampler.h`、`kernel/otlp_http_exporter.h`、`context/traceparent.h`、`context/context_storage.h`、`log/logger.h`）；**命名空间** `galay::tracing`
- **核心类型与接口**:
  - `Span`：`Span(std::string name, TraceContext context [, SpanTimingPolicy])`；`name()`、`context()`、`kind()/setKind(SpanKind)`、`status()/setStatus(SpanStatusCode, message={})`、`[[nodiscard]] bool setAttribute(name, value)`（多类型重载）、`addEvent(name, attrs={})`、`addLink(...)`、`void end()`。枚举 `SpanKind{kInternal,kServer,kClient,kProducer,kConsumer}`、`SpanStatusCode{kUnset,kOk,kError}`、`SpanTimingPolicy{kDisabled,kEnabled}`
  - Scope/Context：`[[nodiscard]] SpanGuard startSpan(std::string_view name)`（内部 span，自动采样 + 设当前 context，析构恢复）、`startServerSpan(std::string_view name, const TraceContext& parent)`；`std::optional<TraceContext> currentContext()`、`setCurrentContext(...)`、`clearCurrentContext()`。`TraceContext(TraceId, SpanId, uint8_t traceFlags=0, std::string tracestate={})`；`TraceId::fromHex(...)` / `SpanId::fromHex(...)`
  - W3C 传播：`std::expected<TraceContext, TraceparentError> extractTraceparent(std::string_view traceparent, std::string_view tracestate)`、`std::string injectTraceparent(const TraceContext&)`、`injectTracestate(const TraceContext&)`
  - Sampler：抽象 `Sampler::shouldSample(const SpanContext* parent, const TraceId&)`；内置 `AlwaysOnSampler / AlwaysOffSampler / ParentBasedSampler(const Sampler& root) / TraceIdRatioSampler(double ratio)`；`setSampler(const Sampler*)` / `currentSampler()`
  - Processor/Exporter：`SpanProcessor{ onEnd(Span&&); forceFlush(ms); shutdown(ms) }`；`setSpanProcessor(SpanProcessor*)` + RAII `SpanProcessorScope`；`BatchSpanProcessor`（`std::unique_ptr<SpanExporter>` + `BatchSpanProcessorConfig`）。OTLP `OtlpHttpExporter(OtlpHttpExporterConfig={})`（`endpoint`/`timeout`/`headers`/`resource_attributes`）；`GALAY_TRACING_ENABLE_OTLP_HTTP` 下 `makeGalayHttpOtlpTransport(...)`
  - 日志关联：宏 `GALAY_LOG_TRACE/DEBUG/INFO/WARN/ERROR(fmt, args...)` 及 `*_CTX(context, ...)`，自动附带当前 context 的 trace/span id；`Logger`、`defaultLogger()`、`ContextLogger`
- **最小示例**:
```cpp
#include <galay/cpp/galay-tracing/context/context_storage.h>
#include <galay/cpp/galay-tracing/context/traceparent.h>
#include <galay/cpp/galay-tracing/kernel/span_guard.h>
#include <galay/cpp/galay-tracing/log/logger.h>

int main() {
    auto inbound = galay::tracing::extractTraceparent(
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", "vendor=value");
    if (!inbound.has_value()) return 1;
    auto span = galay::tracing::startServerSpan("GET /orders", *inbound);  // SpanGuard，析构自动 end
    GALAY_LOG_INFO("handling request {}", 123);
    auto current = galay::tracing::currentContext();
    if (current) std::cout << galay::tracing::injectTraceparent(*current) << '\n';
}
```
- **文档**: `docs/cpp/modules/tracing/`

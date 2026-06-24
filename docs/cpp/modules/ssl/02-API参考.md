# 02-API参考

本页按公开头文件整理 API，源头如下：

- `galay-ssl/common/defn.hpp`
- `galay-ssl/common/error.h`
- `galay-ssl/ssl/ssl_context.h`
- `galay-ssl/ssl/ssl_engine.h`
- `galay-ssl/async/ssl_socket.h`

## 公开头文件与模块入口

| 路径 | 角色 | 说明 |
| --- | --- | --- |
| `galay-ssl/common/defn.hpp` | 基础枚举与类型别名 | `SslMethod`、`SslVerifyMode`、`SslHandshakeState`、`SslIOResult`、`SslFileType` |
| `galay-ssl/common/error.h` | 错误模型 | `SslErrorCode`、`SslError` |
| `galay-ssl/ssl/ssl_context.h` | 进程级 / 配置级 TLS 上下文 | 证书、CA、验证、cipher、ALPN、session cache |
| `galay-ssl/ssl/ssl_engine.h` | 单连接低层 TLS 引擎 | Memory BIO、握手、读写、session 细节 |
| `galay-ssl/async/ssl_socket.h` | 协程业务入口 | bind/listen/connect/handshake/recv/send/shutdown/close |
| `galay-ssl/module/module_prelude.hpp` | 模块前置头 | 供 `galay_ssl.cppm` 复用，不额外导出业务 API |
| `galay-ssl/module/galay_ssl.cppm` | C++23 模块接口 | `import galay.ssl;` 的真实模块文件 |

导出边界：

- 安装包稳定导出 target：`galay-ssl::galay-ssl`
- 条件生成但**安装包不导出**的模块 target：`galay-ssl-modules`
- `galay-ssl/async/awaitable.h` 是 `ssl_socket.h` 的实现支撑头，不是稳定独立入口

## 基础枚举

### `SslMethod`

- `TLS_Server`
- `TLS_Client`
- `TLS_1_2_Server`
- `TLS_1_2_Client`
- `TLS_1_3_Server`
- `TLS_1_3_Client`
- `DTLS_Client`
- `DTLS_Server`

### `SslVerifyMode`

- `None`
- `Peer`
- `FailIfNoPeerCert`
- `ClientOnce`

### `SslHandshakeState`

- `NotStarted`
- `InProgress`
- `Completed`
- `Failed`

### `SslIOResult`

- `Success`
- `WantRead`
- `WantWrite`
- `Error`
- `ZeroReturn`
- `Syscall`

### `SslFileType`

- `PEM`
- `ASN1`

## `SslErrorCode`

以下错误码定义在 `galay-ssl/common/error.h`：

- `kSuccess`
- `kContextCreateFailed`
- `kCertificateLoadFailed`
- `kPrivateKeyLoadFailed`
- `kPrivateKeyMismatch`
- `kCACertificateLoadFailed`
- `kSslCreateFailed`
- `kSslSetFdFailed`
- `kHandshakeFailed`
- `kHandshakeTimeout`
- `kHandshakeWantRead`
- `kHandshakeWantWrite`
- `kReadFailed`
- `kWriteFailed`
- `kShutdownFailed`
- `kPeerClosed`
- `kVerificationFailed`
- `kSNISetFailed`
- `kALPNSetFailed`
- `kTimeout`
- `kUnknown`

`SslError` 本身提供：

- `bool isSuccess() const`
- `bool needsRetry() const`
- `SslErrorCode code() const`
- `unsigned long sslError() const`
- `std::string message() const`
- `std::string sslErrorString() const`
- `static SslError fromOpenSSL(SslErrorCode code)`

## `SslContext`

头文件：`galay-ssl/ssl/ssl_context.h`

### 生命周期与状态

- `explicit SslContext(SslMethod method)`
- `~SslContext()`
- `SslContext(SslContext&& other) noexcept`
- `SslContext& operator=(SslContext&& other) noexcept`
- `bool isValid() const`
- `SSL_CTX* native() const`
- `const SslError& error() const`

### 证书与 CA

- `std::expected<void, SslError> loadCertificate(const std::string& certFile, SslFileType type = SslFileType::PEM)`
- `std::expected<void, SslError> loadCertificateChain(const std::string& certChainFile)`
- `std::expected<void, SslError> loadPrivateKey(const std::string& keyFile, SslFileType type = SslFileType::PEM)`
- `std::expected<void, SslError> loadCACertificate(const std::string& caFile)`
- `std::expected<void, SslError> loadCAPath(const std::string& caPath)`
- `std::expected<void, SslError> useDefaultCA()`

### 验证与 TLS 策略

- `void setVerifyMode(SslVerifyMode mode, std::function<bool(bool, X509_STORE_CTX*)> callback = nullptr)`
- `void setVerifyDepth(int depth)`
- `std::expected<void, SslError> setCiphers(const std::string& ciphers)`
- `std::expected<void, SslError> setCiphersuites(const std::string& ciphersuites)`
- `std::expected<void, SslError> setALPNProtocols(const std::vector<std::string>& protocols)`
- `void setMinProtocolVersion(int version)`
- `void setMaxProtocolVersion(int version)`
- `void setSessionCacheMode(long mode)`
- `void setSessionTimeout(long timeout)`

## `SslEngine`

头文件：`galay-ssl/ssl/ssl_engine.h`

`SslEngine` 是单连接级低层 API；如果只是写业务协程，优先使用 `SslSocket`。

### 生命周期与状态

- `explicit SslEngine(SslContext* ctx)`
- `~SslEngine()`
- `SslEngine(SslEngine&& other) noexcept`
- `SslEngine& operator=(SslEngine&& other) noexcept`
- `bool isValid() const`
- `SSL* native() const`
- `SslHandshakeState handshakeState() const`
- `bool isHandshakeCompleted() const`

### BIO 与握手

- `std::expected<void, SslError> setFd(int fd)`（旧模式，头文件中标注 deprecated）
- `std::expected<void, SslError> initMemoryBIO()`
- `int feedEncryptedInput(const char* data, size_t length)`
- `int extractEncryptedOutput(char* buffer, size_t length)`
- `size_t pendingEncryptedOutput() const`
- `std::expected<void, SslError> setHostname(const std::string& hostname)`
- `void setConnectState()`
- `void setAcceptState()`
- `SslIOResult doHandshake()`
- `SslIOResult shutdown()`

### 数据读写

- `SslIOResult read(char* buffer, size_t length, size_t& bytesRead)`
- `SslIOResult write(const char* buffer, size_t length, size_t& bytesWritten)`
- `int getError(int ret) const`
- `size_t pending() const`

### 协商结果与 Session

- `X509* getPeerCertificate() const`
- `long getVerifyResult() const`
- `std::string getProtocolVersion() const`
- `std::string getCipher() const`
- `std::string getALPNProtocol() const`
- `bool setSession(SSL_SESSION* session)`
- `SSL_SESSION* getSession() const`
- `bool isSessionReused() const`

## `SslSocket` 返回的 awaitable 对象

`SslSocket::handshake()` / `recv()` / `send()` / `shutdown()` 会返回 `galay::ssl::*Awaitable` 对象。

- 这些类型定义在 `galay-ssl/async/awaitable.h`
- 该头文件由 `ssl_socket.h` 传递包含，用来满足编译需要
- 这层属于协程桥接细节，不应视为稳定的独立消费入口
- 业务代码应直接 `co_await socket.handshake()` / `recv()` / `send()` / `shutdown()`，而不是依赖其内部状态机辅助类型
- `RecvCtx` / `SendCtx` / `HandshakeRecvCtx` / `HandshakeSendCtx` / `ShutdownRecvCtx` / `ShutdownSendCtx` 以及 `ReadAction` / `SendChunkState` 都是 `awaitable.h` 中的内部状态机辅助类型，不属于独立 API 面

## `SslSocket`

头文件：`galay-ssl/async/ssl_socket.h`

`SslSocket` 依赖 `galay-kernel` 中的 `Host`、`IPType`、`GHandle`、`IOController` 与若干 awaitable 类型；其中 SSL 专用 awaitable 现已收敛到 `galay::ssl` 命名空间。

### 生命周期与句柄

- `SslSocket(SslContext* ctx, galay::kernel::IPType type = galay::kernel::IPType::IPV4)`
- `SslSocket(SslContext* ctx, GHandle handle)`
- `~SslSocket()`
- `SslSocket(SslSocket&& other) noexcept`
- `SslSocket& operator=(SslSocket&& other) noexcept`
- `GHandle handle() const`
- `galay::kernel::IOController* controller()`
- `SslEngine* engine()`
- `bool isValid() const`
- `bool isHandshakeCompleted() const`
- `galay::kernel::HandleOption option()`

### 建连与监听

- `std::expected<void, galay::kernel::IOError> bind(const galay::kernel::Host& host)`
- `std::expected<void, galay::kernel::IOError> listen(int backlog = 128)`
- `std::expected<void, SslError> setHostname(const std::string& hostname)`
- `galay::kernel::AcceptAwaitable accept(galay::kernel::Host* clientHost)`
- `galay::kernel::ConnectAwaitable connect(const galay::kernel::Host& host)`
- `galay::ssl::SslHandshakeAwaitable handshake()`

### 收发与关闭

- `galay::ssl::SslRecvAwaitable recv(char* buffer, size_t length)`
- `galay::ssl::SslSendAwaitable send(const char* buffer, size_t length)`
- `galay::ssl::SslShutdownAwaitable shutdown()`
- `galay::kernel::CloseAwaitable close()`

### 连接属性与 Session

- `X509* getPeerCertificate() const`
- `long getVerifyResult() const`
- `std::string getProtocolVersion() const`
- `std::string getCipher() const`
- `std::string getALPNProtocol() const`
- `bool setSession(SSL_SESSION* session)`
- `SSL_SESSION* getSession() const`
- `bool isSessionReused() const`

## 返回值、生命周期与协程语义

- `SslContext` / `SslEngine` 的配置与低层接口主要返回 `std::expected<void, SslError>` 或 `SslIOResult`
- `SslSocket` 的业务路径统一是 awaitable 风格：`connect()`、`accept()`、`handshake()`、`recv()`、`send()`、`shutdown()`、`close()` 都应通过 `co_await` 使用
- 配置失败、证书失败、握手失败、读写失败等都统一通过 `SslError` / `SslErrorCode` 解释，而不是依赖 OpenSSL 原始错误文本做业务分支
- `SslSocket` 与 `SslEngine` 构造函数都接收 `SslContext*`，因此 `SslContext` 必须至少活到相关 `SslEngine` / `SslSocket` 生命周期结束
- `connect()` / `bind()` / `listen()` 只处理 TCP / 句柄层，不等价于 TLS 握手；TLS 建连是否完成应看 `handshake()` 或 `isHandshakeCompleted()`
- `SslEngine` 是单连接低层抽象；如果你已经在协程里处理网络 I/O，优先使用 `SslSocket`，不要把 `SslEngine` 当成共享 TLS 全局对象
- 平台 I/O 后端由 `galay-kernel` 决定；`galay-ssl` 的公开 API 不按 `kqueue/epoll/io_uring` 拆成不同类型

## 交叉验证入口

- include 示例：`examples/include/e1_echo.cc`、`examples/include/e2_echo.cc`
- import 示例：`examples/import/e1_echo.cc`、`examples/import/e2_echo.cc`
- 测试入口统一位于 `test/`，用于交叉验证 socket、loopback、advanced TLS 行为
- socket / loopback / advanced smoke：`test/t1_socket.cc`、`test/t2_loopback.cc`、`test/t3_policy.cc`
- 状态机 / builder / 错误桥接回归：`test/t4_state.cc`、`test/t5_io.cc`、`test/t6_custom.cc`、`test/t7_builder.cc`、`test/t8_proto.cc`、`test/t9_bridge.cc`

## 当前 API 边界

以下内容在头文件中可以确认：

- `ssl_socket.h` 是稳定的协程入口；`galay-ssl/async/awaitable.h` 只是其传递包含的内部支撑头
- 有 ALPN API：`SslContext::setALPNProtocols()`、`SslEngine::getALPNProtocol()`、`SslSocket::getALPNProtocol()`
- 有 Session API：`setSessionCacheMode()`、`setSessionTimeout()`、`setSession()`、`getSession()`、`isSessionReused()`
- 有 CA 文件、CA 路径与系统默认 CA API：`loadCACertificate()`、`loadCAPath()`、`useDefaultCA()`

以下内容在头文件中不能确认，文档不应臆造：

- 没有公开的 `SslSocket::setTimeout()` 之类的显式超时配置接口
- 安装包没有导出的 `galay-ssl::galay-ssl-modules` target

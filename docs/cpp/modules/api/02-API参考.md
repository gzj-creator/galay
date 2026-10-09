# API 参考

## 头文件与契约

原生入口为 `galay-http/server/http_server.h`、`galay-http2/server/http2_server.h`。
使用 typed 注册时再包含 `galay-api/api_router.h` 并链接 `galay::api`；HTTP/2
另链接 `galay::http2`。轻量契约声明在 HTTP 中，不引入 serde；绑定、schema、
JSON 和 Swagger 实现留在可选 API 模块。内部准备产物不作为应用构建入口。

| `galay::api` 类型 / 函数 | 用途 |
| --- | --- |
| `ApiResult<T>` | `std::expected<T, ApiError>` |
| `ApiError{code,message,status}` | 类别、具体原因和 HTTP 状态 |
| `api_error_name(code)` | 完整错误码枚举的稳定名称映射 |
| `ApiInfo` | `title`、`version`、`description` |
| `Operation` | id、说明、tags、成功状态、业务错误响应 |
| `InputBinding<I>` | 成员指针 path/query 绑定 |
| `NoInput` / `NoContent` | 无输入 / 无 body 输出 |
| `ApiContext` | 编码完成前借用只读请求；HTTP/2 也使用此语义视图 |
| `DocsConfig` | `spec_path="/openapi.json"`、`ui_path="/docs"` |

## Builder

`HttpServerBuilder`、`HttpsServerBuilder`、`H2cServerBuilder`、`H2ServerBuilder`
都有 `EnableSwagger = false` 模板参数和以下公共注册接口：

| 接口 | 返回值 / 语义 |
| --- | --- |
| `add_api<Method, Input, Output>(path, handler, operation, binding = {})` | `ApiResult<void>`；执行路由和类型契约同时登记 |
| `add_handler<Methods...>(path, handler)` | `ApiResult<void>`；明文 HTTP 连接 handler |
| `add_request_handler<Methods...>(path, handler)` | `ApiResult<void>`；跨传输、自持响应 handler |
| `api_info(info)` | `void`，仅开启文档模式 |
| `docs(config = {})` | `void`，仅开启模式；文档路径和内嵌资源 |
| `docs(config, directory)` | `void`，仅开启模式；显式目录，无 fallback |
| `export_openapi()` | `ApiResult<std::string>`，仅开启模式；不创建 server |
| `build()` | `ApiResult<std::unique_ptr<NativeServer>>` |
| `build_config()` | 仅导出原生配置，不包含路由和文档 |

原生 fluent 配置保留，亦可 `Builder(config)`。`false` 保留 typed 请求校验、
描述符稳定性检查、业务错误和 JSON 编解码，但没有文档状态及文档配置/导出
接口。普通 route 没有 DTO 元数据，参与冲突检查但不进入 OpenAPI。

重复方法路径、native router 的等价路径、同形改名参数、不可比较的重叠参数
路径及重复 operationId 显式失败。多方法注册失败不留下部分路由。普通
wildcard 保留 exact / parameter / wildcard 优先级；文档与任意方法的普通或
typed 路由冲突都在 `build()` 前失败。

## 生命周期

先准备文档和全部资源，再构造原生 server。路径或资源失败不创建 Runtime、
不监听，可修正 `docs` 后重试。成功构建冻结注册，后续注册或构建返回
`kFrozenBuilder`。server 自持路由、handler 和资源，builder 可以销毁。

managed `server.start()` 返回 `ApiResult<void>`。监听、TLS、Runtime 启动失败
返回 `kTransportError` 并保留具体原因。尝试启动后重复调用返回 `kServerError`；
对已运行的原生实例调用 managed 启动同样返回 `kServerError`，不停止现有服务。
`stop()` 不支持 restart。控制线程可重复停止，关闭监听和连接 IO 后排空受管理
任务，再停止 Runtime。应用必须保证 handler 最终返回，并管理独立后台任务。
不能在业务协程中同步调用 `stop()`。原生连接 handler / 协议升级仍可接管
socket，其生命周期由原生调用方负责。

HTTPS、h2c 和 H2 拒绝绑定到明文 `HttpConn` 的普通连接路由，使用
`add_request_handler` / `add_api`。HTTP/2 builder 路由与原生 stream/active
handler、static routes 或 static mounts 不可混用，返回 `kInvalidBinding`，
不会静默覆盖配置。

## Handler 与 Schema

typed handler 接受 `ApiContext&`、按值 `Input`，返回
`galay::kernel::Task<ApiResult<Output>>`。请求和 context 在挂起、处理和编码
期间有效，不能保留引用到任务结束后的工作。优先独立协程函数，避免临时
coroutine lambda 闭包；可变共享捕获的线程安全由应用负责。
输出优先自持 DTO；`string_view` 的底层存储须覆盖编码，不能引用 handler
局部变量或已销毁的 input。当前 request view 只在本次编码前有效。

非 500 业务错误状态须登记在 `Operation::errors`，否则转换为 500。错误 JSON
使用 `{"code":"business_error","message":"..."}` 等稳定字符串，C++ 枚举使用
`ApiErrorCode::kBusinessError` 等命名，由 `api_error_name` 显式映射。
HEAD、204/205 不发送 body；204/205 必须使用 `NoContent` 输出。

`schema_for<T>(SchemaUse::input/output)`、`schema_for_field`、`schema_json`、
`inspect_path`、`validate_endpoint`、`render_openapi` 均返回 `ApiResult`。
schema 保留 int64/uint64/double，optional 的 nullable 与存在性分开。输入
optional 可缺失或为 null，输出字段仍存在。静态与运行期 serde 字段名、指针、
约束和枚举须一致；输入在绑定前后、输出在编码前检查，漂移显式失败。

## 文档资源

默认固定 `swagger-ui-dist@5.17.14`，九项资源直接内嵌在 `galay-api` 源码中。显式
目录须包含下面非空普通文件，每个最多 16 MiB：

```text
swagger-ui.css
swagger-ui-bundle.js
swagger-ui-standalone-preset.js
favicon-16x16.png
favicon-32x32.png
LICENSE
NOTICE
README.md
SHA256SUMS
```

缺项、空文件、非普通文件、超限、读取或关闭失败返回 `kResourceError`，保留
原因，不补用内嵌资源或 CDN。加载器不会替自管资源执行官方哈希认证。
资源一次加载后由 server 自持，请求不读文件、不生成 schema、不改注册表。

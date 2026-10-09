# galay-api

`galay-api` 为 HTTP、HTTPS、h2c 和 H2 原生服务提供类型化参数绑定、serde JSON
编解码、OpenAPI 3.1.0 和内嵌官方 Swagger UI。入口统一到各原生 server builder
的 `add_api`，不再使用独立的 API builder、API server 或文档策略。

四个 builder 都具有 `EnableSwagger = false` 模板参数：

| Builder | 原生 Server | 额外构建条件 |
| --- | --- | --- |
| `HttpServerBuilder<true>` | `HttpServer` | HTTP |
| `HttpsServerBuilder<true>` | `HttpsServer` | SSL |
| `H2cServerBuilder<true>` | `H2cServer` | HTTP2 |
| `H2ServerBuilder<true>` | `H2Server` | HTTP2、SSL |

`false` 保留 typed 绑定、运行期校验、序列化和错误处理，不保存文档状态、不
生成或挂载 OpenAPI/UI。`true` 在 `build()` 中生成文档并安装资源，原生 server
自持最终路由和资源。普通 `add_handler` / `add_request_handler` 参与冲突检查，
但没有 DTO 元数据，不会自动变成 OpenAPI 操作。

默认资源固定为 `swagger-ui-dist@5.17.14`，九个服务资源已作为源码内置到
`galay-api`。部署和构建都不需要 `assets` 目录、额外 JS、CSS、图标、源码路径
或特定工作目录，不使用 CDN。显式 `docs(config, directory)` 仍可使用完整外置
资源，缺项失败，不回退。

`GALAY_BUILD_API` 默认 `OFF`，启用时要求 HTTP 和 serde。类型化应用包含
`galay-api/api_router.h` 及所选原生 server 头，链接 `galay::api`；使用 HTTP/2
时另链接 `galay::http2`。普通 HTTP 只需 `galay::http`，不引入 API、serde 或 UI。
当前 API 模板使用 C++23 头文件接口，原生 HTTP/HTTP2 模块 facade 仍可消费。

- [快速开始](00-快速开始.md)：构建、运行、四种传输及安装消费。
- [API 参考](02-API参考.md)：注册、错误、冻结和生命周期。
- [使用指南](03-使用指南.md)：绑定、文档资源和生成注册函数。
- [示例代码](04-示例代码.md)：用户 API 和离线导出。
- [验证记录](verification.md)：历史证据和本次实际回归。

TLS 使用原生证书配置；h2c 使用 prior knowledge，浏览器直连 HTTP/2 使用
H2 over TLS 并确认 ALPN `h2`。原生 HTTP/1 fallback 404 不算 typed H2 的
HTTP/1 兼容入口。HTTPS 是 HTTP/1 over TLS。

不包含 `.api` 解析器或生成器、WebSocket/AsyncAPI、API C ABI、multipart、
多值 query、热更新或额外反射系统。公开头、构建 target、示例和测试优先于文档。

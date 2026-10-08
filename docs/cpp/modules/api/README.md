# galay-api

`galay-api` 为 HTTP/1 明文、HTTPS（HTTP/1 over TLS）、h2c 和 H2 over TLS
REST 服务提供类型化参数绑定、JSON 编解码、
OpenAPI 3.1.0 导出和内嵌官方 Swagger UI。DTO 使用 serde 的字段登记和约束；
路径、参数来源、成功状态、业务错误及说明来自同一次 `ApiBuilder::add`。
`ApiServer<Policy = NoSwagger>` 通过 concept 约束无虚函数的文档策略；框架
提供 `HttpSwagger`，应用可实现自定义策略。四种传输共用 DTO、路由和契约，
使用现有 HTTP/TLS/HTTP2 引擎，不为每种传输复制业务 handler。

`HttpSwagger{}` 默认使用构建时编译进可选 API 模块的
`swagger-ui-dist@5.17.14`，部署不需要额外 JS、CSS、图标或资源目录，不依赖
源码路径和工作目录，也不使用 CDN。构建时校验固定上游文件的 SHA256；
许可证、来源及校验清单与 UI 一起内嵌。需要外置资源的自定义策略必须
显式调用 `install_docs_from_directory`，缺少任何必要文件均失败，不回退
内嵌资源或 CDN。

- [快速开始](00-快速开始.md)：四种传输、测试证书、离线导出、安装后消费。
- [API 参考](02-API参考.md)：公开类型、错误处理、冻结及生命周期。
- [使用指南](03-使用指南.md)：绑定规则、文档路径、资源、离线部署和边界。
- [示例代码](04-示例代码.md)：GET、POST、业务错误及命令行。
- [验证记录](verification.md)：实际执行的单测、loopback、安装和外部验收。

真相来源优先级：公开头文件和导出 target、实现、示例、测试、本文档。
本模块为可选项，`GALAY_BUILD_API` 默认 `OFF`；启用时要求 HTTP 和 serde。
使用 C++23 头文件接口，外部工程链接 `galay::api`。

| 传输 | 原生配置 / 引擎 | typed 路由、OpenAPI、全部 Swagger 资源 | 浏览器 |
| --- | --- | --- | --- |
| HTTP/1 明文 | `HttpServerConfig` / `HttpServer` | 支持 | HTTP/1 Try it out |
| HTTPS | `HttpsServerConfig` / `HttpsServer` | 支持 | HTTP/1 over TLS Try it out |
| HTTP/2 明文 | `H2cServerConfig` / `H2cServer` | 支持，prior knowledge | Chromium 不建立 h2c 连接 |
| HTTP/2 over TLS | `H2ServerConfig` / `H2Server` | 支持，ALPN `h2` | H2 Try it out |

TLS 需要 `GALAY_BUILD_SSL=ON`，HTTP/2 需要 `GALAY_BUILD_HTTP2=ON`；
未启用的原生配置不出现在 `ApiServerConfig` 中。h2c 使用现有 prior-knowledge
方式，没有为本任务新增 Upgrade。H2 原生 HTTP/1 fallback 返回 404，
不是 typed 路由或 Swagger 的 HTTP/1 兼容入口。

不包含 WebSocket 消息契约、AsyncAPI、RPC/MCP/数据库协议、原始 TCP/UDP
契约、源码注释扫描、API C ABI、OAuth 服务端、multipart、复杂 query 数组
或热更新；也不声称实现 OpenAPI 的所有可选特性。实际验证范围和平台限制
见验证记录。

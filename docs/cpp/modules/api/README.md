# galay-api

`galay-api` 为 HTTP/1 明文 REST 服务提供类型化参数绑定、JSON 编解码、
OpenAPI 3.1.0 导出和内嵌官方 Swagger UI。DTO 使用 serde 的字段登记和约束；
路径、参数来源、成功状态、业务错误及说明来自同一次 `ApiBuilder::add`。
`ApiServer<Policy = NoSwagger>` 通过 concept 约束无虚函数的文档策略；框架
提供 `HttpSwagger`，应用可实现自定义策略，无需修改底层 HTTP server。

`HttpSwagger{}` 默认使用构建时编译进可选 API 模块的
`swagger-ui-dist@5.17.14`，部署不需要额外 JS、CSS、图标或资源目录，不依赖
源码路径和工作目录，也不使用 CDN。构建时校验固定上游文件的 SHA256；
许可证、来源及校验清单与 UI 一起内嵌。需要外置资源的自定义策略必须
显式调用 `install_docs_from_directory`，缺少任何必要文件均失败，不回退
内嵌资源或 CDN。

- [快速开始](00-快速开始.md)：构建、真实 HTTP 示例、离线导出、安装后消费。
- [API 参考](02-API参考.md)：公开类型、错误处理、冻结及生命周期。
- [使用指南](03-使用指南.md)：绑定规则、文档路径、资源、离线部署和边界。
- [示例代码](04-示例代码.md)：GET、POST、业务错误及命令行。
- [验证记录](verification.md)：实际执行的单测、loopback、安装和外部验收。

真相来源优先级：公开头文件和导出 target、实现、示例、测试、本文档。
本模块为可选项，`GALAY_BUILD_API` 默认 `OFF`；启用时要求 HTTP 和 serde。
使用 C++23 头文件接口，外部工程链接 `galay::api`。

首版不包含 HTTP/2、HTTPS 路由、WebSocket 消息契约、AsyncAPI、源码注释
扫描、C ABI、OAuth 服务端、multipart、复杂 query 数组或热更新。
底层 HTTP 能力不受影响，这些能力没有被包装成类型化 API 的兼容层。

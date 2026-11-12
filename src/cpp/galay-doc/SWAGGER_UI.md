# 内嵌 Swagger UI

可选 `galay-doc` 模块在 `ui_assets.cc` 中固定内嵌官方
`swagger-ui-dist@5.17.14` 发行包。HTML 外壳和初始化脚本定义在 `docs.cc`。
构建和默认部署不需要单独的 UI 文件、下载、CDN 或资源生成步骤。

## 来源与许可

- 发布方：`swagger-api`（Swagger / SmartBear）。
- 于 2026-10-07 导入，未修改上游浏览器资源。
- 包元数据：<https://registry.npmjs.org/swagger-ui-dist/5.17.14>
- 发行包：<https://registry.npmjs.org/swagger-ui-dist/-/swagger-ui-dist-5.17.14.tgz>
- 上游项目：<https://github.com/swagger-api/swagger-ui>
- 许可证：Apache-2.0；原始 `LICENSE` 和 `NOTICE` 包含在快照中，并通过配置的文档路径提供。
- npm `dist.shasum`：`e2c222e5bf9e15ccf80ec4bc08b4aaac09792fd6`。
- npm `dist.integrity`：`sha512-CVbSfaLpstV65OnSjbXfVd6Sta3q3F7Cj/yYuvHMp1P90LztOLs6PfUnKEVAeiIVQt9u2SaPwv0LiH/OyMjHRw==`。
- 发行包 SHA-256：`c57badf459aa6e65cc036b3862d0502a63f9a22546407ffcb0e64f85f816bb28`。

快照包含 CSS、浏览器 bundle、独立 preset、两个 favicon、`LICENSE`、`NOTICE`、
作为 `README.md` 提供的本文以及 `SHA256SUMS`。源码映射、npm 工具、上游 HTML、
Petstore 初始化脚本和 OAuth 重定向页都不是产品依赖。初始化脚本关闭外部校验器、
授权信息持久化和 query 参数配置覆盖。

`api.transports` 测试通过真实 HTTP 响应校验七项上游资源的固定 SHA-256；浏览器
验收脚本使用相同校验，并在桌面和移动浏览器中测试 UI。

应用可以显式调用 `docs(config, directory)`，在启动时加载完整的自定义发行包。
资源缺失、为空、不可读或超限时，会在文档路由注册前失败；缺失资源不会从内嵌
快照补齐。成功加载的资源字节由 server 持有。

## 更新资源快照

升级上游版本时，必须同时更新 `ui_assets.cc` 中的资源、内嵌许可证和来源信息，
以及 `test/cpp/api/transport_acceptance.cjs` 中固定的哈希值。将资源编码为 C++
字符串前，应先核实包完整性和原始文件。更新后运行文档、内嵌资源、传输、安装和
浏览器验收测试。仓库不保留构建期生成器或外部资源目录。

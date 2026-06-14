# Release Notes

## v3.1.1 - 2026-05-20

- **版本级别**: patch
- **Git 提交消息**: docs: 为所有头文件接口添加中文 Doxygen 文档注释
- **Git Tag**: v3.1.1

### 变更摘要

- 为项目所有头文件添加完整的中文 Doxygen 文档注释
- 注释覆盖文件级（@file/@brief/@author/@version/@details）、类/结构体级（@brief/@details/@tparam/@note）、方法级（@brief/@param/@return）以及成员变量和枚举值的行尾 ///< 注释
- 更新 CMakeLists.txt 版本号至 v3.1.1

## v3.1.2 - 2026-06-02

- **版本级别**: patch
- **Git 提交消息**: fix: 发布 v3.1.2 修订版本
- **Git Tag**: v3.1.2

### 变更摘要

- HTTP、WebSocket、H2C 客户端连接与会话获取路径改为通过 `std::expected` 返回错误，避免非法 URL、未连接 socket/session、关闭未连接客户端等场景抛异常或触发硬崩路径
- HTTP Range、chunk size、HTTP/1.1 响应状态码与 HTTP/2 `:status` 解析改为无异常解析，解析失败返回现有错误码或无效结果
- HTTP/2 stream manager 的后台协程调度失败改为显式返回失败并通知 waiter/关闭队列，避免调度异常穿透或业务等待悬挂
- 修复大体积 HTTP/1.1 PUT body 读取过程中可能进入状态机 abort 路径的问题，将读取、解析、body 上限和分块解析错误传播为业务可处理的 `HttpError`
- 修复文件打开、服务端 accept 后 socket 构造、代理上游 session/socket 获取等路径的异常穿透风险，失败时返回错误或发送 502
- 修复 HTTP/HTTPS/WS/WSS 客户端连接失败后保留半初始化 socket 的问题，后续 session/socket 获取会继续返回错误而不是暴露无效对象
- 修复 WebSocket upgrader 空输入和构造异常可能穿透的问题，统一转换为业务可处理的 `WsError`
- 修复 SSL 开启时 `E6-HttpsClient` 示例中 `Task<void>` 返回布尔值导致的编译失败
- 增加大文件 PUT 与错误传播回归测试，并将需要外部服务的手动测试从默认 CTest 自动套件中排除
- 更新 CMakeLists.txt 版本号至 v3.1.2

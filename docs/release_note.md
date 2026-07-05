# Release Note

## v4.0.1 - 2026-07-05

- **版本级别**：修订版本（patch）
- **Git 提交消息**：`chore: 发版 v4.0.1（移动 tag 至最新提交，补齐累计变更说明）`
- **Git tag**：`v4.0.1`（force-move 至本次发布提交）

### 变更摘要

本次为 `v4.0.0` 之后的修订版本发版，并把 `v4.0.1` tag 从初始发布提交移动到最新发布提交，使其覆盖 `v4.0.0 → HEAD` 的全部累计变更。主干内容如下。

- **新增 galay-framework 开发 skill**（`agent/skill/galay-usage/`）：作为在 galay 上开发服务端 / 客户端 / 中间件与 C/FFI 的统一入口；`SKILL.md` 与 `references/{cpp-api,c-api}.md` 覆盖 Runtime / `Task<T>` / `std::expected` 心智模型、include 前缀与命名空间、CMake 链接 target、构建开关、平台后端速查与 13 个 C++ 模块 + C ABI 模块地图。顶层 `SKILL.md` 与 `references/` 已迁入 `agent/skill/galay-usage/` 命名目录，便于安装与复用。
- **统一 CMake 安装包为单一 `galay` 包**：移除顶层 `CMakeLists.txt` 按模块生成独立 package config 的 foreach，删除模板 `cmake/galay-module-config.cmake.in`；安装后只在 `lib/cmake/galay` 下导出 `galayConfig.cmake` / `galayConfigVersion.cmake` / `galayTargets.cmake`，外部项目统一通过 `find_package(galay CONFIG REQUIRED)` 后按需链接 `galay::<module>`，并由 install 布局校验断言不再安装按模块的 package 目录。
- **全模块新增 TCP_NODELAY 可配置化**：etcd / http / http2 / mcp / mysql / redis / rpc / ws 各模块客户端与服务端配置新增 `tcp_no_delay` 字段与 `tcpNoDelay()` builder，连接建立或 accept 后按配置启用 `TCP_NODELAY`，选项失败按各模块语义显式传播。
- **kernel 写路径改用局部 SIGPIPE 抑制**：`handleWritev` 改为 `sendmsg()` + `MSG_NOSIGNAL`，与 `handleSend` / `handleSendTo` 对齐；支持平台默认启用 `SO_NOSIGPIPE`，框架不再依赖全局 SIGPIPE 处置，向断连 socket 写入返回 `EPIPE`。
- **修复 HTTP server 路由参数丢失**：路由命中后立即把解析出的路径参数回填到 `request`，业务 handler 现在能正确读取 `/users/:id` 这类路径参数。
- **全模块性能测试文档与基准数据落地**：为 kernel / http / http2 / ws / rpc / ssl / tracing / utils / redis / mysql / mongo / etcd 各模块补全 `05-性能测试.md`，并归档 `benchmark_data/`、`configs/` 压测结果与复现配置；新增 `docs/benchmark_plan.md` 与 `docs/machine_config.md`。
- **构建与脚本整理**：`BUILD_TESTING` / `GALAY_BUILD_EXAMPLES` / `GALAY_BUILD_BENCHMARKS` 默认改为 `OFF`（按需开启）；`scripts/` 按模块归类并加数字前缀；构建版本号（`CMakeLists.txt` 与 `MODULE.bazel`）对齐 `4.0.1`。
- **清理过期文档**：删除被新 skill 取代的旧优化建议、陈旧 `docs/release_note.md`（仅停留在 v3.0.0）与 `docs/README.md` 等。

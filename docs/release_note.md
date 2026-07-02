# Release Note

## v4.0.1 - 2026-07-03

- **版本级别**：修订版本（patch）
- **Git 提交消息**：`feat: 新增 galay-framework 开发 skill 并对齐构建版本号至 4.0.1`
- **Git tag**：`v4.0.1`

### 变更摘要

本次为 `v4.0.0` 之后的修订版本发版，核心是补齐 galay 框架的开发入口 skill，并顺带清理被其取代的过期文档、对齐构建版本号。

- **新增 galay-framework 开发 skill**（`agent/skill/`）：作为在 galay 上开发服务端 / 客户端 / 中间件与 C/FFI 的统一入口。`SKILL.md` 给出 Runtime / `Task<T>` / `std::expected` 错误传播的心智模型、include 前缀与命名空间、CMake 链接 target、构建开关与平台后端（io_uring / epoll / kqueue）速查，以及 13 个 C++ 模块 + C ABI 的模块地图；`references/cpp-api.md` 汇总 13 个 C++ 模块的公开类型与方法签名；`references/c-api.md` 覆盖 C ABI 的错误约定、runtime / coro 驱动模型、每模块 handle 与完整 C 示例。
- **清理过期文档**：删除被新 skill 取代的旧优化建议（`c-abi-encapsulation-optimization.md`、`cpp-modules-optimization.md`、`naming-and-cmake-optimization.md`、`rust-ffi-zero-overhead-guide.md`）、陈旧的 `docs/release_note.md`（仅停留在 v3.0.0）、`docs/README.md` 与 `文档审查报告.md`。
- **构建版本号对齐**：`CMakeLists.txt` 的 `project(galay VERSION ...)` 与 `MODULE.bazel` 的 `module(... version = ...)` 自 `4.0.0` 升至 `4.0.1`，与本次 git tag 一致。

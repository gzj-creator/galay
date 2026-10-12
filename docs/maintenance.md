---
doc_id: galay.maintenance.shared-skills
title: Galay 共享工程规范维护
system: galay
module: repository
doc_type: governance
status: active
applies_to: v7.0.1 之后的未发布工程规范；Galay 与 gts-ai 为同级目录的开发环境
updated_at: 2026-10-12
access_level: internal
---

# Galay 共享工程规范维护

## 共享规范的入口与范围

Galay 的仓库规则由根目录 [AGENTS.md](../AGENTS.md) 定义。维护或审查 mcpp 依赖清单、相关构建入口和依赖文档时，必须先读取 [gts-mcpp-dependencies](../.codex/skills/gts-mcpp-dependencies/SKILL.md)。更新代码、配置、接口或准备提交时，按 [gts-docs-rag](../.codex/skills/gts-docs-rag/SKILL.md) 同步 `docs/` 并检查文档质量；用户要求提交或发版时使用 [commit_change](../.codex/skills/commit_change/SKILL.md)。

当前仓库通过 `.codex -> ../gts-ai/.codex` 软链接读取共享 skill，规则内容由同级 `gts-ai` 仓库维护，Galay 不复制一套本地实现。开发环境须保留该目录关系，并确认对应 `SKILL.md` 可读；迁移工作目录前核对软链接目标，不覆盖已有 `.codex` 内容或链接。Git 记录的是软链接本身，不会把目标仓库的规则文件一并打包。

## mcpp 依赖维护规则与当前清单

按照共享依赖规范，GTS 系列项目的第三方 mcpp 声明必须使用 `/opt/mcpp-deps/` 下的绝对 `path`，包括开发、构建、平台条件和 feature 依赖；不使用 Git、相对路径或仅版本号的索引声明。包身份由目标目录的 `mcpp.toml` 核实，不能依据目录名推断或随路径修改命名空间。

2026-10-12 在当前开发环境核对的共享包如下。版本只记录本次实际读取的清单，不承诺这些共享目录以后保持相同内容。

| 清单路径 | 清单声明的包身份与版本 |
|---|---|
| `/opt/mcpp-deps/galay/mcpp.toml` | `gzj-creator.galay@7.0.1` |
| `/opt/mcpp-deps/serde/mcpp.toml` | `gzj-creator.serde@0.5.1` |
| `/opt/mcpp-deps/gts-common/mcpp.toml` | `gzj-creator.gts-common@0.1.0` |

本次工程规范接入没有修改 [mcpp.toml](../mcpp.toml) 的依赖声明。当前清单仍含 `serde` 的 `thirdparty/serde` 相对路径，以及 `compat.libaio`、`compat.openssl`、`compat.spdlog` 的索引声明，尚未完成共享路径规范迁移。后续依赖维护需同步清单、生成入口和权威文档，核对目标包身份，并执行适用的离线锁定构建；不能把本次规则接入视为依赖迁移或迁移后的构建验收。

本次提交范围不包含工作区 `mcpp.lock` 中已有的 `compat.libaio` 内容哈希变化，该改动保留原状。共享规则入口和根目录工程准则不改变现有构建依赖的实际来源。

## 兼容性与验证依据

共享工程规范接入只变更规则入口、仓库工程准则及维护文档，不修改生产代码、公开 API、模块导出或构建参数。因此本次以软链接解析、skill 文件可读性、文档本地链接、清单事实核对和 `git diff --check` 为验收依据，没有为规则接入重复运行生产行为测试。

2026-10-12 已确认 `.codex` 指向 `../gts-ai/.codex`，三个共享 skill 及文档规范可读；共享包的身份和版本与上表一致，`mcpp.toml` 没有本次 diff。utils/RDTSC 的构建、CTest、原生模块消费与性能工具验证由 [Rdtsc 验证与未覆盖范围](cpp/modules/utils/rdtsc.md#验证与未覆盖范围) 记录，生产计时路径仍保留标准时钟。

## 来源

- [AGENTS.md](../AGENTS.md)：工程准则第 3 节的依赖与兼容性规则。
- [.codex](../.codex)：共享规则软链接入口，目标为 `../gts-ai/.codex`。
- [gts-mcpp-dependencies](../.codex/skills/gts-mcpp-dependencies/SKILL.md)：共享路径、包身份和验证要求。
- [gts-docs-rag](../.codex/skills/gts-docs-rag/SKILL.md) 与 [文档规范](../.codex/skills/gts-docs-rag/references/documentation-standard.md)：源文档同步和质量门禁。
- [mcpp.toml](../mcpp.toml)：当前依赖声明的实际来源和 feature 范围。

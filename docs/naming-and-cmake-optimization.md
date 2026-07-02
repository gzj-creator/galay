# galay 命名重构与 CMake 优化建议

> 范围:项目文件/目标命名一致性,以及 CMake 构建系统结构优化。基于对根 `CMakeLists.txt`、`cmake/option.cmake`、各模块 `src/cpp/*/CMakeLists.txt`、`src/c/**` 文件命名的实际核查。
> **核心论点**:项目里那段被复制 10 遍的 CMake alias 解析,**根因是模块用了裸目标名(`kernel`/`utils`/`http`…)**。把目标重命名为 `galay-<module>` + `galay::<module>` alias,既是命名一致性修复,**也直接消掉了 CMake 的重复胶水**。所以本文把“重命名”和“CMake 优化”合并——它们是同一问题的两面。

---

## 第一部分:命名与重命名

### 1.1【最关键】CMake 目标命名:裸名 → `galay-<module>` + `galay::<module>`

**现状(已核查)**:核心模块都用**裸通用名**:
```cmake
add_library(kernel  ${KERNEL_SOURCES})   # src/cpp/galay-kernel/CMakeLists.txt
add_library(utils   ${UTILS_SOURCES})
add_library(http    ...) / http2 / redis / rpc / ssl / ws / etcd / mongo / mysql / mcp / tracing
```
整改前同仓较新的目标又带前缀,例如旧的 RPC module facade、`galay-tracing-spdlog`、`galay-tracing-kernel`,命名规约自相矛盾。

**问题**:
- `kernel` / `utils` / `http` / `ssl` 这类名字**极易与外部工程冲突**。一旦被 `add_subdirectory` 或 `FetchContent` 引入,几乎必然撞上宿主工程自己的 `utils`/`http` 目标。
- 正因为目标是裸名,下游无法确定该引用 `galay::kernel` 还是 `galay-kernel` 还是 `kernel`——这就是 1.3 节那段 **10× 重复 alias 解析**的直接起因。

**建议**:
```cmake
# 每个模块统一:
add_library(galay-kernel ${KERNEL_SOURCES})     # 内部规范名,带命名空间前缀
add_library(galay::kernel ALIAS galay-kernel)   # 唯一对外 alias
```
规约:**内部规范名 `galay-<module>`;对外只暴露 `galay::<module>`;不保留旧裸名 alias。** 这条落地后,1.3 的重复胶水直接坍缩成一行 `galay::kernel`。

> ⚠️ 破坏性提示:重命名公开 CMake 目标对现有下游是 breaking change。本次按破坏性重构处理,不保留旧名 alias。

### 1.2 `.wroktree` 目录疑似拼写错误

根目录存在 `.wroktree/`——疑为 `worktree` 拼写错误的 stray 目录。
**建议**:确认用途后,重命名为 `.worktree/` 或删除,并确保其在 `.gitignore` 中(与 `.workbuddy`、`.cache` 等本地工具目录一致处理),避免误入版本库。属于零风险清理。

**落地**:`.wroktree/` 已重命名为 `.worktree/`,`.gitignore` 仅保留 `.worktree/`。

### 1.3 `.h` / `.hpp` 后缀无统一规则

**现状(已核查)**:`src/cpp` 下 212 个 `.h`、70 个 `.hpp`,且**同一目录混排无明显规律**。例如 `galay-kernel/core/` 里:
```
awaitable.h  task.h  scheduler.hpp  io_scheduler.hpp  timeout.hpp  runtime.h
```
`scheduler.hpp` 与 `runtime.h` 并列,`io_scheduler.hpp` 与 `awaitable.h` 并列,读者无法从后缀推断“是否 header-only / 是否含模板”。

**建议**:确立并文档化**一条**规则,二选一即可:
- 方案 A(推荐,改动小):`.h` = 有对应 `.cc` 的声明头;`.hpp` = header-only / 模板/内联为主的实现头。按此**核对少数不符项**逐步归位,不强制一次全改。
- 方案 B:全部统一为 `.h`(C++ 生态两者皆可,一致性优先于偏好)。

关键不是选哪个,而是**有规则且写进 `docs/`**,新文件照办。属于 `CLAUDE.md` 外科手术式修改:只动不符规则的少数文件。

### 1.4 C ABI 文件命名三套并存 → 收敛

**现状(已核查)**,`src/c/**` 存在三种 C 文件命名:
| 形态 | 例子 | 使用处 |
| --- | --- | --- |
| `<name>_c.h` | `tcp_socket_c.h`、`coro_task_c.h`、`runtime_c.h` | kernel C 内部 |
| `<module>_c.h` | `http_c.h`、`redis_c.h`、`tracing_c.h`、`mysql_c.h` | 各模块公开 C 头 |
| `c_<name>_bridge.h` | `c_coro_tcp_bridge.h` | bridge 层 |

**问题**:三套命名混在同一 ABI 里,难一眼区分“公开 vs 内部 vs 桥接”,也不利于 C ABI 文档提出的**每模块伞形头**收敛。

**建议**(与 `docs/c-abi-encapsulation-optimization.md` 呼应):
- 公开 C 头统一:每模块一个伞形公开头 `galay/<module>/<module>_c.h`(路径已带命名空间,后缀 `_c` 明示 C ABI)。
- 内部/桥接头统一带 `_internal` / `_bridge` 标识,且不进安装导出。
- 现有细粒度头可保留,只需通过伞形头组织并明确“未纳入伞形头者视为内部”。

**落地**:非 kernel C 模块公开头与实现文件已从 `<module>.h` / `<module>.cc` 重命名为 `<module>_c.h` / `<module>_c.cc`;kernel 新增 `kernel_c.h` 伞形公开头。

---

## 第二部分:CMake 结构优化

### 2.1【由 1.1 直接解决】消除 10× 重复的 alias 解析

**现状(已核查)**:`if(TARGET galay::kernel) … elseif(galay-kernel) … elseif(kernel) …` 这段级联在 **10 个模块 `CMakeLists.txt` 里逐字复制**,且每个模块对 kernel、utils 各来一遍(见 `galay-rpc/CMakeLists.txt:1-27`)。

**根因**:目标是裸名(1.1),下游不得不枚举所有可能别名。

**建议**:
1. **治本**:落地 1.1,统一 `galay::<module>`,则各模块直接 `target_link_libraries(galay-rpc PRIVATE galay::kernel galay::utils)`,级联整段删除。
2. **兜底 helper**(过渡期仍需兼容多来源时):在 `cmake/` 加一个
   ```cmake
   galay_resolve_target(OUT_VAR CANONICAL galay::kernel LEGACY galay-kernel kernel)
   ```
   集中处理,替代每模块手写。

### 2.2 缺 `CMakePresets.json` → 构建画像

**现状(已核查)**:仓库无 `CMakePresets.json`;`option.cmake` 全模块 + tests + examples + benchmarks + shared 默认 `ON`,且 `GALAY_DISABLE_IOURING` 默认 `ON`(默认关掉高性能 Linux 后端,与定位略冲突)。

**建议**:新增 `CMakePresets.json`,把意图从“记一长串 `-D`”变成命名画像:
- `developer-full`:全模块 + tests + examples + benchmarks(核心开发/CI 全量)。
- `consumer-minimal`:仅 `utils + kernel` 默认 ON,协议模块显式开启,tests/examples/benchmarks OFF(库消费者)。
- `linux-perf`:显式尝试 io_uring,找不到 liburing 给清晰降级提示。

`option.cmake` 保持兼容(不改默认也可先加 preset),渐进迁移。

### 2.3 `file(GLOB)` 收集源码(28 处)的取舍

**现状(已核查)**:28 个模块 `CMakeLists.txt` 用 `file(GLOB ... CONFIGURE_DEPENDS ...)` 收集源码。

**评估**:用了 `CONFIGURE_DEPENDS` 缓解了“新增文件不重配”的经典问题,但仍有代价:每次 build 触发 glob 检查、跨平台/实验性/平台特定源码容易被误纳入。

**建议**(非强制,按团队偏好):
- 若追求可复现与显式依赖:平台特定源码(epoll/kqueue/uring、可选 TLS/io_uring)改为**显式列出**,普通源码可继续 GLOB。
- 至少对 Bazel 侧的 `glob(["**/*.cc"])`(架构报告已指出)做同样收敛,避免双构建行为漂移。

### 2.4 值得肯定、不要动的地方

核查发现 CMake 已有不错的纪律,应保留:
- **模块依赖校验链**:`option.cmake` 里 `KERNEL 需要 UTILS`、`WS 需要 HTTP`、`HTTP2 需要 HTTP` 等 `FATAL_ERROR` 前置校验,清晰且 fail-fast。
- **选项弃用处理**:`GALAY_TRACING_ENABLE_OTLP_HTTP` → `..._GALAY_HTTP_OTLP_TRANSPORT` 的 `DEPRECATION` + `FORCE` 迁移,是正确的 option 演进范式,可作为 1.1 目标重命名的兼容模板。
- **`GALAY_BUILD_C_API` 默认 OFF**:C ABI 默认不构建,对纯 C++ 用户是合理轻量默认。

### 2.5 Bazel 与 CMake 对等(引用)

架构报告已指出 Bazel(`glob(["**/*.cc"])`、缺 option/external deps/test 等价物)明显弱于 CMake。命名与目标重构时,应**同步决定 Bazel 支持级别**:要么补齐为一等支持并纳入 CI,要么在 README 明确标注实验性,避免“能 CMake 不能 Bazel”。详见 `architecture_defects_report.md` 第 5 节。

---

## 落地顺序

1. **零风险先做**:`.wroktree` 清理(1.2)、`.h/.hpp` 规则文档化并归位少数不符项(1.3)。
2. **核心一步(命名=CMake 双修)**:目标改 `galay-<module>` + `galay::<module>`,同步删除 10× alias 级联(1.1 + 2.1)。
3. **接入体验**:加 `CMakePresets.json`(2.2)。
4. **收敛细节**:C ABI 伞形头与文件命名(1.4)、GLOB 取舍(2.3)、Bazel 对等决策(2.5)。

**兼容红线**:第 2 步与 C ABI 公开头重命名都是 breaking change。本次按用户要求直接破坏性重构,不保留旧 target alias 或旧公开头 stub。

---

## 引用
- `architecture_review_report.md` / `architecture_defects_report.md` — CMake alias 分散、默认构建过重、Bazel 不对等
- `docs/c-abi-encapsulation-optimization.md` — C ABI 伞形头与文件命名收敛
- `docs/cpp-modules-optimization.md` — C++ 模块级优化

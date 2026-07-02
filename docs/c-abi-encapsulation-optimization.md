# galay C 接口封装优化建议

> 范围:针对 `src/c/**` 现有 C ABI 封装,给出可执行的优化建议。基于对 `galay-common-c`、`galay-kernel-c`(core/async/coro)、`galay-bridge-c`、`galay-http-c`、`galay-redis-c` 等模块头文件的实际核查。
> 目标读者:C ABI 维护者,以及后续做 Rust/其他语言 FFI 绑定的人(见姊妹文档 `rust-ffi-zero-overhead-guide.md`)。

---

## 一、先肯定:现状做得好的地方

这些是**已经正确、不要动**的约定,后续封装应继续遵守:

- **ABI 安全的基础类型**:用 `galay_bool_t` 枚举而非 C++ `bool`(`galay_c_defs.h`),避免跨编译器 bool 布局问题。核查确认头文件中**无 C++ `bool`、无位域、无 `#pragma pack`**——ABI 干净。
- **不透明句柄**:如 `galay_coro_task_t { void* task; }`,用 struct 包裹 `void*` 而非裸 `void*`,在 C 侧提供最小类型安全。
- **结果按值返回**:`C_IOResult{code, sys_errno, bytes, value, ptr}` 按值返回,无堆分配,寄存器/小结构体传递,FFI 友好。
- **显式错误传播**:符合 `CLAUDE.md` 第 5 条,不跨 ABI 抛异常;`galay_status_t` + 每模块 `*_get_error` 的约定覆盖面广(核查到 22 个 `*_get_error`)。
- **ABI 版本纪律**:`GALAY_C_VERSION_*` 宏 + 运行时 `galay_c_version_major/minor/patch`,头文件声明版本与链接库运行时版本可比对。
- **缓冲区按 (ptr,len) 传递**:IO 路径用 `(char* buffer, size_t length)`,不假设 NUL 结尾;所有权/生命周期在每个函数注释中说明。

**结论:C ABI 的底子是干净且规范的。下面的建议是“收敛不一致”和“把隐式约定显式化”,不是推倒重来。**

---

## 二、优化建议(按优先级)

### P0-1:统一(或明确桥接)两套并行的结果/错误惯用法

**现状**:C ABI 存在两套结果表达:

| 惯用法 | 形态 | 使用范围 |
| --- | --- | --- |
| `galay_status_t` | 8 值通用枚举 + 模块专用枚举 | 大部分对象/CRUD/配置类 API |
| `C_IOResult` | `{code, sys_errno, bytes, value, ptr}` 结构体 | coroutine / 直接 IO 路径 |

两者语义不同(一个是纯状态码,一个是带 payload 的结果),这本身合理。但问题是:
- 两套错误码**枚举空间独立**(`galay_status_t` vs `C_IOResultCode`),调用方要维护两张映射表。
- **缺口(已核查)**:`galay_status_t` 有 `galay_status_string`,但 **`C_IOResultCode` 似乎没有对应的 `*_get_error` / `*_string` 函数**。这违反 `CLAUDE.md` 第 5 条“每个公开 C 错误码枚举都必须提供错误字符串获取函数”。

**建议**:
1. **补齐 `C_IOResultCode` 的错误字符串函数**(如 `galay_coro_ioresult_string(C_IOResultCode)`),覆盖全部 6 个枚举值。这是硬性合规项,优先做。
2. 文档层明确两套惯用法的**选择规则**:“状态类 → `galay_status_t`;协程/IO 带字节数或 fd 的结果 → `C_IOResult`”,并在 `docs/c/README.md` 固化,避免新模块随意二选一。
3. 长期可选:提供一个 `galay_status_t galay_ioresult_to_status(C_IOResultCode)` 归一化函数,让上层只处理一套状态码。

### P0-2:把“所有权/生命周期”从注释约定升级为命名约定

**现状**:所有权规则写得很清楚,但**只在注释里**。例如 `galay_core_coro_tcp_accept` 的 `*out_socket 必须为 NULL`、`out_socket 必须在完成前保持有效`、`调用方最终必须销毁`。核查到每个对象都有 `_destroy`,但命名有漂移:`_destroy` / `_free`(`galay_redis_reply_free`)/ `_release`(`galay_redis_pool_release`)混用。

**问题**:FFI 绑定作者只能靠读注释推断所有权,机器无法校验,漏读就是 use-after-free / 泄漏。

**建议**:确立并统一一套**所有权命名规约**,让函数签名自解释:
- 输出拥有型句柄:统一用 `out_` 前缀(已部分采用)。
- 释放:统一动词。建议 `_destroy` 用于“对象句柄整体销毁”,`_free` 仅用于“释放库分配的裸缓冲区/字符串”,`_release` 仅用于“归还池化资源”。当前 redis 的 `reply_free` vs `reply_destroy` 若语义重叠应合并。
- 借用 vs 转移:对接收指针的参数,注释统一用固定措辞(`[borrowed]` / `[takes ownership]` / `[out, owned]`),便于绑定工具和人一眼识别,也便于 Rust 侧映射成 `&T` / `T` / `-> Owned<T>`。

### P1-1:桥接层的裸 `void*` 建议改为具名不透明指针

**现状**:`galay-bridge-c` 大量使用裸 `void*`,例如 `galay_core_coro_tcp_recv(void* socket, void* scheduler, ...)`。而 public 层(`coro_task_c.h`)已经用了具名 struct 句柄。

**问题**:
- 桥接层虽标注为“内部 C 风格桥接”,但它是 C ABI 的一部分,裸 `void*` 让 `socket` 和 `scheduler` 可被误传(编译器不报错)。
- FFI 绑定对 `void*` 无法生成类型区分,安全 wrapper 要额外人工约束。

**建议**:即使是内部桥接,也用具名不透明指针 typedef:
```c
typedef struct GalayCoreTcpSocket GalayCoreTcpSocket;   // 不完整类型
typedef struct GalayCoreIOScheduler GalayCoreIOScheduler;
GalayCoreCoroIOResult galay_core_coro_tcp_recv(
    GalayCoreTcpSocket* socket, GalayCoreIOScheduler* scheduler, ...);
```
零运行时成本,纯编译期类型安全,bindgen 会生成不同的不透明类型,天然防止参数错位。

### P1-2:回调表(`GalayCoreCoroWaitOps`)的契约需机器可校验

**现状**:`wait / complete_user_data / release_user_data` 三个回调“必须全部非 NULL”,靠注释约束。回调可能在 IO scheduler 完成路径被触发,线程安全由实现方保证。

**建议**:
- 入口函数对 `wait_ops` 及三个成员做**显式非空校验**,不满足直接返回 `Invalid`(而不是解引用崩溃)。这是 FFI 边界防御,外部语言更容易传错。
- 在头注释中用固定小节写明每个回调的**允许调用线程**和**是否可重入**,因为这是 Rust 侧决定 `Send`/`Sync` 边界的唯一依据。

### P1-3:每模块提供单一“伞形头文件”

**现状**:模块内头文件分散(如 kernel 的 `coro_task_c.h` / `coro_result_c.h` / `runtime_c.h` / `async-c/*`)。FFI 绑定通常希望一个入口头。

**建议**:每个 C 模块提供一个聚合头,如 `galay/c/galay-kernel-c/kernel_c.h`,`#include` 该模块所有公开 C 头。好处:
- bindgen/ffigen 只需一个入口,减少路径耦合。
- 明确“什么是公开 C ABI”(未被伞形头纳入的即视为内部)。
- 不改变现有细粒度头,纯增量。

**落地**:非 kernel C 模块公开头与实现文件统一为 `<module>_c.h` / `<module>_c.cc`;kernel 提供 `kernel_c.h` 作为公开伞形头。

### P2-1:结构体 ABI 稳定性显式化

**现状**:如 `C_IOResult`、`GalayCoreCoroHost`(含 `char address[46]` 内联数组)依赖自然布局。同平台/同编译器无问题,但跨版本演进时,在结构体中间加字段会破坏 ABI。

**建议**:
- 对**会返回给外部并按值拷贝**的结构体(`C_IOResult` 等),约定“只在尾部追加字段,不在中间插入”,并在注释标注。
- 或对配置类输入结构体引入 `size_t struct_size` 版本字段(caller 填 `sizeof`),让库能在 ABI 演进时兼容旧调用方。IO 热路径结果结构体不必加,避免开销。

### P2-2:平台类型(`struct iovec`)的暴露收敛

**现状**:`c_coro_tcp_bridge.h` 直接 `#include <sys/uio.h>` 暴露 `struct iovec`。这让 C ABI 头依赖 POSIX 系统头。

**建议**:可接受(iovec 是 POSIX 稳定 ABI),但建议在文档中标注“这些 API 依赖 POSIX `struct iovec` 布局”,便于非 POSIX 目标或纯 Windows 绑定评估。若要彻底可移植,可定义 `galay_iovec_t { void* base; size_t len; }` 并在实现内转换。

---

## 三、落地顺序建议

1. **P0-1 补 `C_IOResultCode` 错误字符串** + **P0-2 所有权命名规约**:合规 + 消除 FFI 最大风险源。
2. **P1-1 具名不透明指针** + **P1-3 伞形头**:直接降低绑定成本,零运行时代价。
3. **P1-2 回调非空校验** + **P2 结构体/平台约定**:边界健壮性与长期 ABI 稳定。

以上均为**增量、非破坏性**改动:补函数、加 typedef、加校验、加文档约定,不改变现有函数签名语义。这与项目“外科手术式修改”原则一致。

---

## 四、与其他文档的关系

- 这些封装约定直接决定 FFI 绑定质量,具体到 Rust 的零损耗映射见 `rust-ffi-zero-overhead-guide.md`。
- tracing 模块的 C ABI 还需补 OTLP exporter,同属“C ABI 覆盖不均”问题,后续由 tracing 集成方案单独跟踪。

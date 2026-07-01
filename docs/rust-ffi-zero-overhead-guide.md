# galay 近零损耗 FFI Rust 绑定方案

> 主题:如何在 galay 现有 C ABI 之上生成“几乎零损耗(near-zero-overhead)”的 Rust 绑定。
> 前置:请先读 `c-abi-encapsulation-optimization.md`——绑定质量由 C ABI 封装质量决定。
> 结论先行:galay 的 C 协程是**有栈纤程 + 同步外观的挂起式 IO**,这决定了**零损耗的正确姿势是“让 Rust 跑在 galay 协程里、直接同步调用 FFI”,而不是把它包成 Rust `Future` 交给 tokio**。后者不是零损耗,是双运行时。

---

## 一、关键前提:galay C 协程的本质

核查 `src/c/galay-kernel-c/coro-c` 与 `galay-bridge-c` 后确认:

- **有栈纤程**:`coro_context_x86_64.S` / `coro_context_aarch64.S` 是手写上下文切换汇编。协程有独立栈。
- **同步外观、纤程级挂起**:`galay_core_coro_tcp_recv(...)` 从调用方看是**同步返回** `C_IOResult`,但内部通过 `GalayCoreCoroWaitOps::wait` **挂起当前纤程**(不阻塞 IO scheduler 线程),完成后恢复。
- **入口即普通函数指针**:`galay_coro_spawn(runtime, entry_fn, arg, ...)`,`entry_fn` 是 `void(*)(void*)`,运行在 owner IO scheduler 线程。
- **`join` 有线程约束**:`galay_coro_join` 从 scheduler 线程或协程内调用返回 `Invalid`——只能在外部“主线程”join。

**这意味着**:Rust 只要在 `entry_fn` 里,调用 `recv/send/accept` 就是**普通的同步 FFI 调用**——没有 `poll`、没有 `Waker`、没有 `Future` 状态机、没有 executor。这正是零损耗的来源。

---

## 二、方案选型:两条路,只推荐一条

### 方案 A(推荐,零损耗):Rust-in-galay-coroutine,同步风格

Rust 代码作为 galay 协程体运行,IO 调用直接 FFI:

```rust
// 安全 wrapper 之上,业务代码长这样(同步外观,实际纤程挂起)
fn handle_conn(sock: &TcpSocket, sched: &Scheduler) -> Result<(), IoError> {
    let mut buf = [0u8; 4096];
    let n = sock.recv(sched, &mut buf, Timeout::Infinite)?;   // 直接 FFI,纤程挂起
    sock.send(sched, &buf[..n], Timeout::Infinite)?;
    Ok(())
}
```

- **成本模型**:每个 IO 操作 = 1 次 FFI 调用 + 1 次纤程上下文切换(本来就有)。**Rust 侧零额外分配、零 Future、零跨运行时唤醒。**
- 适用:用 galay 作为运行时、想用 Rust 写业务/协议逻辑的场景。这是 galay 的主场,也是“几乎零损耗”能成立的唯一姿势。

### 方案 B(不推荐当零损耗):映射成 Rust `Future` + tokio

用 Rust 实现 `wait_ops` 回调,把纤程等待桥接到 Rust task 的 park/wake。

- **成本**:引入 Waker、跨运行时唤醒、可能的线程切换与同步。这是**双运行时协作**,不是零损耗。
- **且语义拧巴**:galay 已经有自己的调度器和纤程,再叠一个 tokio 反应堆是重复。
- 仅在“已有庞大 tokio 生态、galay 只作为某个组件”时才考虑,并明确标注它不是零损耗路径。

> **立场**:用户要“几乎 0 损耗的 FFI Rust 代码”,答案就是方案 A。不要为了“Rust 要 async/await 才地道”而选 B——在 galay 里,同步外观的纤程调用本来就是异步的,再包一层 Future 是纯亏。

---

## 三、零损耗的具体技术要点

### 3.1 分层:`-sys` 原始绑定 + 安全 wrapper

```
galay-sys/          # bindgen 生成的原始 FFI,extern "C",repr(C),unsafe
  build.rs          # 链接已安装的 galay C 库(见 3.6)
  src/lib.rs        # include! 预生成 bindings 或 build 期生成
galay/              # 安全 wrapper:所有权/生命周期用 Rust 类型表达,零运行时成本
```

安全层的“安全”几乎全部是**编译期**的(类型、生命周期、Drop),不引入运行时开销。

### 3.2 结构体:`#[repr(C)]` 精确对齐,按值传递

```rust
#[repr(C)]
pub struct C_IOResult {
    pub code: C_IOResultCode,   // 见 3.3
    pub sys_errno: c_int,
    pub bytes: usize,
    pub value: i64,
    pub ptr: *mut c_void,
}
```
按值返回,匹配 C 布局,无 boxing。安全层把它 `match` 成 `Result<Payload, IoError>`——单分支跳转,单态化后零成本。

### 3.3 枚举:必须用 newtype,不能用 Rust `enum`(正确性 + 零成本)

C 枚举若被 Rust `#[repr(C)] enum` 接收,当 C 返回一个 Rust 不认识的判别值时是**未定义行为**。零损耗不能以 UB 为代价。

**做法**:bindgen 配置 `default_enum_style = "newtype"`(或 `moduleconsts`),生成:
```rust
#[repr(transparent)]
pub struct C_IOResultCode(pub c_int);
impl C_IOResultCode {
    pub const OK: Self = Self(0);
    pub const EOF: Self = Self(1);
    // ...
}
```
`#[repr(transparent)]`,零成本,且能安全承载任意 C 返回值。

### 3.4 缓冲区/切片:零拷贝直传

- recv:`&mut [u8]` → `(buf.as_mut_ptr() as *mut c_char, buf.len())`,库直接写入,**无中间拷贝**。
- send:`&[u8]` → `(as_ptr, len)`。
- iovec:Rust 侧构造 `libc::iovec` 数组切片直传 `galay_core_coro_tcp_writev`,零拷贝散布写。
- **wrapper 绝不在 IO 路径分配**;C ABI 已经是 caller-buffer 设计,Rust 必须保持这一点。

### 3.5 句柄:`repr(transparent)` newtype + `Drop` 表达所有权

把 C ABI 里“注释级”的所有权规则,变成 Rust 的**类型不变量**(零运行时成本):

```rust
#[repr(transparent)]
pub struct CoroTask(NonNull<c_void>);   // 拥有型

impl Drop for CoroTask {
    fn drop(&mut self) {
        // 对应 galay_coro_destroy;注意其“必须已终止”前置条件
        unsafe { sys::galay_coro_destroy(&mut raw(self)); }
    }
}

// 借用型:方法签名用 &TcpSocket,不 Drop,对应 C 的 [borrowed]
```
- 拥有型 → `impl Drop` 调 `_destroy`;借用型 → `&T`,不释放。C 文档的 `[borrowed]/[takes ownership]/[out,owned]`(见 C ABI 文档 P0-2)直接对应 Rust `&T / T / -> Owned<T>`。
- 这是 Rust 相对裸 C **净增的安全**,且**零运行时开销**。

### 3.6 回调边界:必须 `catch_unwind`(否则 UB,且违反项目异常准则)

`entry_fn`、`wait_ops` 的回调是 `extern "C" fn`。**Rust panic 跨 `extern "C"` 边界 unwind 进 C 是 UB**。这与 `CLAUDE.md` 第 5 条“不跨 ABI 传播异常”是对称要求(C++ 不抛、Rust 不 unwind)。

```rust
extern "C" fn coro_entry_trampoline(arg: *mut c_void) {
    let _ = std::panic::catch_unwind(AssertUnwindSafe(|| {
        // 调用用户 Rust 逻辑
    }));
    // panic 被吞在边界内;如需传播,转成错误码/日志,绝不 unwind 出去
}
```
- 所有 Rust→C 回调 trampoline **一律包 `catch_unwind`**。
- 建议 crate 级设置 `panic = "abort"` 作为兜底(可选),但边界 `catch_unwind` 是必须的。

### 3.7 线程与 `Send`/`Sync`:按 C ABI 的线程契约标注

- 协程体运行在 **owner IO scheduler 线程**;协程内**绝不能阻塞**该线程(禁止 `std::thread::sleep`、阻塞 syscall、`Mutex` 长持有)——要“等待”就用 galay 的异步 IO。文档必须显式警告。
- `galay_coro_join` 只能在**非 scheduler 线程**调用——Rust 的 `main`/驱动线程 join,协程内不 join。
- 句柄的 `Send`/`Sync` 实现要严格依据 C ABI 注释(3.4/P1-2 要求 C 侧写清回调的允许线程),不要凭感觉 `unsafe impl Send`。

### 3.8 构建集成:预生成 bindings + 稳定链接

- **依赖 C ABI 的 `find_package`/install 与 pkg-config**。`build.rs` 优先用 `pkg-config`/CMake 导出定位库。
- **预生成并提交 bindings.rs**(而非每次 build 跑 bindgen):可复现、无 bindgen 构建依赖、CI 快;bindgen 仅在 ABI 变更时手动重跑。用 C ABI 版本宏(`GALAY_C_VERSION_*`)在 `build.rs` 做兼容性断言。
- bindgen 建议项:`--use-core`、`--ctypes-prefix=libc`、blocklist 系统类型改用 `libc::iovec` 等、`--newtype-enum`(见 3.3)、为伞形头(C ABI 文档 P1-3)生成而非逐头拼接。

---

## 四、这套方案“损耗”到底在哪(诚实边界)

“几乎零损耗”指的是 **Rust 封装层不引入额外开销**,而非消灭所有成本:

| 成本项 | 是否由 Rust 引入 | 说明 |
| --- | --- | --- |
| FFI 调用本身 | 否 | 跨语言直接调用,无 marshaling(结构体/切片已 ABI 对齐) |
| 纤程上下文切换 | 否 | galay 本身机制,C 也一样 |
| `C_IOResult` → `Result` 转换 | 极小 | 单分支,单态化后基本消失 |
| `catch_unwind` | 极小 | landing pad,正常路径几乎零成本 |
| newtype enum / repr(transparent) | 零 | 纯类型层 |
| 安全层生命周期/Drop | 零运行时 | 编译期检查 |

**唯一需要坚决避免的“隐形损耗”**:方案 B 的跨运行时唤醒、IO 路径里的堆分配、以及用 Rust `enum` 承接 C 枚举(UB,不是开销)。

---

## 五、落地顺序建议

1. **前置**:先落地 C ABI 文档的 P0(补 `C_IOResultCode` 错误字符串、所有权命名规约)、P1-1(具名不透明指针)、P1-3(伞形头)——它们直接决定绑定能否零损耗且安全。
2. **`galay-sys`**:对 kernel(runtime + coro + bridge)先行,bindgen + 预生成 bindings + `build.rs` 链接。
3. **安全 wrapper(方案 A)**:`Runtime` / `CoroTask` / `TcpSocket` / `Scheduler`,同步风格方法,`Result` 错误,`Drop` 所有权,回调 `catch_unwind`。
4. **一个端到端 echo 示例**:Rust 业务体 + galay 协程,验证零 Future、零额外分配,并与 C 版基准对比确认无回退。
5. 其余协议模块(http/redis/…)按需增量,复用同一套约定。

---

## 六、一句话结论

galay 的有栈纤程 + 同步挂起式 IO 是 FFI 的“礼物”:**让 Rust 跑在协程里直接同步调用,就是零损耗**。绑定层的价值不在于套 async,而在于用 Rust 的类型系统把 C ABI 的所有权/生命周期/线程契约在编译期钉死——**安全是白拿的,损耗是可以逼近零的**。唯一红线:回调边界 `catch_unwind`、C 枚举用 newtype、IO 路径不分配。

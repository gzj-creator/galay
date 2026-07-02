# galay C++ 各模块优化建议

> 范围:对 `src/cpp/galay-*` 13 个 C++ 模块逐一做优化梳理。
> 方法:kernel / http / http2 / rpc 已在既有报告深入分析(见文末引用),本文引用其结论并只补新点;utils / ssl / ws / redis / mysql / mongo / etcd / mcp / tracing 由并行源码勘察得出,均带 `file:line`。
> **重要口径**:以下条目来自**源码阅读**,`file:line` 为勘察时定位,**动手前请 `git grep` 复核行号并用 profiling 确认热度**——避免为“看起来更快”做非必要改动(遵循 `CLAUDE.md` 外科手术式修改原则)。优先级 P0/P1/P2 指“该模块内相对影响”,非全项目统一刻度。

---

## 一、跨模块共性问题(最高杠杆,先看这里)

逐模块看下去会发现**同样几个反模式在几乎每个模块重复出现**。修一次模式,收益覆盖全仓——比单点优化更值得先做。

### 共性 1:解析即拷贝(copy-on-parse)——最普遍
几乎每个协议/序列化模块都在**解析时立即 `std::string` 拷贝**,而 `string_view` + 延迟到用户真正取用时才拷贝就能省掉大部分:

| 模块 | 位置 | 现象 |
| --- | --- | --- |
| redis | `redis_protocol.cc:279` | 每个 `+OK` 简单串都 `std::string value(...)`,百万 QPS 即百万次小分配 |
| mysql | `mysql_protocol.cc:451-475`,`:134` | 每行每列 `readLenEncString` 都拷贝;1000 行×10 列 = 1 万次分配 |
| mongo | `bson.cc:269` | 每个 CString decode 都分配拷贝 |
| etcd | `etcd_internal.h:393,399` | 每个 kv 对 base64 decode 两次(key+value) |
| mcp | `json_parser.cc:40,84` | 每次请求把 simdjson `string_view` 拷成 `std::string` |
| tracing | `span.cc:85-126` | 每个 attribute name/value 从 view 拷成 string |
| http | (既有报告)header slow-path `std::map` | rare header 走 map + 字符串规范化 |

**统一方向**:解析层内部结构一律用 `string_view`,只在跨越“所有权边界”(交给用户/入队/异步保存)时才 materialize 成 owned string。这是全仓最大的单一优化主题,预计各解析热路径 10~30% 收益。

### 共性 2:连接池实现三套且质量不一
- **redis**:lease-only,**不复用 socket**,acquire/release 每轮建连销连,waiter 用 `std::queue+std::mutex`(`conn_pool.cc:443-460` 释放时串行重试唤醒)。高并发下是明确瓶颈。
- **mysql**:**真复用**(`m_all_clients` 持有,`m_idle_clients` moodycamel 无锁队列回收),质量最好,但有 CAS 竞争与析构竞态(见下)。
- **rpc**:lease-only 租约限流,不复用 socket(既有报告 P1)。

**统一方向**:以 mysql 的“真复用 + 无锁空闲队列”为模板,收敛出一套 `ConnectionPool<T>` 基座,redis/rpc 复用之。这同时解决架构报告里“RPC 连接池名实不符”与 redis 高churn 两个问题。

### 共性 3:小集合用 `std::vector`/`std::map`,可换 small_vector / flat
- tracing:每个 Span 的 attributes/events/links 都 `std::vector`(`span.h:513-515`),但上限仅 32/64,几乎必然小——small_vector 可消除堆分配。
- http:header pair slow-path `std::map`(既有报告 P1)。

**统一方向**:引入一个内部 `small_vector<T, N>`(或复用 utils 里已有容器),用于“几乎总是很小、生命周期短”的集合。

### 共性 4:每次调用重建 parser / 缺 reserve / 重复 resize
- mcp:`mcp_json.cc:13-14` 每次 parse 都 `make_unique<simdjson::parser>`;应 thread_local + `reset()`(etcd 已这么做,可作范本)。
- ws:`ws_reader.h:809-817,834-836` 快路径 resize→memcpy→再 resize→memmove,单帧三次分配器往返。
- utils:base64 `+=` 逐字符(`base64.hpp:395`)、split/replace 缺 reserve。

---

## 二、逐模块清单(新点为主)

### galay-utils(基础,爆炸半径最大)
整体质量:结构清晰、模板化、move-friendly;但基础热路径有若干可修分配点。
- **P0** base64 encode 忽略已 reserve 的 buffer,逐字符 `ret +=` — `base64.hpp:395`(改批量写入)
- **P0** base64 decode/canDecode 为去空白整串拷贝且递归自调用 — `base64.hpp:167,262`(用 view 原地跳过)
- **P0** LRU `purgeExpired` 每次 get/put 可能扫全优先队列(tombstone 堆积)— `lru_cache.hpp:599-633`
- **P0** ByteQueueView::append 热路径用 `vector::insert(end,...)` 可能反复 realloc — `byte_queue_view.hpp:64`
- **P1** ConsistentHash::getNodes 用 `set find+insert` 去重,环大时偏贵 — `consistent_hash.hpp:367-399`
- **P1** StringUtils::split 全 substr 拷贝进 vector — `core/string.hpp:45-78`(读路径可返回 `vector<string_view>`)
- **P1** StringUtils::replace 逐段 append 无 reserve — `core/string.hpp:215-227`
- **P1**(语义)Bytes::assignOwned 分配 `length+1` 却把 capacity 报成 `length`,capacity() 少报 — `bytes.hpp:446-450`(核实是否影响调用方边界判断)

### galay-ssl(OpenSSL TLS 包装)
整体质量:异步架构好、buffer 管理不错;热路径有 OpenSSL 状态查询开销。
- **P0** 每次 read/write 都 `ERR_clear_error()` — `ssl_engine.cc:189,219`(移到握手期)
- **P0** 单轮 poll 多次 `SSL_pending()` — `ssl_await.cc:281,373,392,420,428,...`(每轮缓存一次)
- **P1** TLS session 复用未见落地/未度量 — `ssl_engine.cc:330,338`(补 session cache + 复用率指标)
- **P1** BIO feed 每次 `BIO_write/read` 无预分配 — `ssl_engine.cc:103-112`
- **P1** async 状态机 64 次内联转移硬上限 — `ssl_await.h:895`(可配置 + 命中告警)

### galay-ws(WebSocket)
整体质量:masking / UTF-8 SIMD 做得好;但重组路径 string 分配 churn、分片 UTF-8 重复校验。
- **P0** 分片写入时逐片校验 UTF-8,完成时又整体校验一遍 — `ws_reader.h:766,826,850`(延迟到 fin 再校验)
- **P0** 快路径 resize→copy→再 resize→memmove,三次分配器往返 — `ws_reader.h:809-836`(单趟写到最终 offset)
- **P0** `wsIsValidUtf8MaskedIovecs` 跨 iovec 逐字节 masked,散布到 2+ buffer 时近 O(n²);已有 SIMD 版未被复用 — `ws_reader.h:157-231` vs `ws_frame.cc:528`
- **P1** 多帧消息 append 无预留;首帧应按总长 reserve — `ws_reader.h:757`
- **P1** 出站帧头构造用临时 string + payload swap/memmove — `ws_frame.cc:373-403`(头直写 output 或 writev 头/体分离)

### galay-redis
整体质量:**lease-only,不复用 socket**,waiter 队列 mutex 竞争重;协议解析拷贝多。
- **P0** release 时 `while(!m_waiters.empty())` 串行重试唤醒,高并发级联延迟 — `conn_pool.cc:443-460`
- **P0** acquire 抽取连接窗口未持锁,与 release 竞态导致假性池空 — `conn_pool.cc:58-78`
- **P0** waiter 队列 `queue+mutex`,建议换 moodycamel(mysql 已用)— `conn_pool.h:675`
- **P1** RESP 简单串每次 `std::string` 拷贝 — `redis_protocol.cc:279`(见共性 1)
- **P1** recordAcquireStats 每次 acquire 双原子 CAS 竞争 — `conn_pool.cc:383-403`(peak 用 relaxed/接受陈旧)
- **P2** health/idle 清理反复 queue→temp_queue 整体搬移 — `conn_pool.cc:551-559` 等多处(改 vector + erase_if)

### galay-mysql
整体质量:**真复用 + 无锁空闲队列,池质量最好**;主要是逐行分配与建连 CAS/竞态。
- **P0** 逐行逐列 `readLenEncString` 拷贝 — `mysql_protocol.cc:451-475,:134`(见共性 1,预计解析提速约 2×)
- **P1** `MysqlPoolLease::dismiss()` 交出裸指针破坏 RAII,忘还即泄漏 — `conn_pool.cc:68-81`(改带自定义 deleter 的 unique_ptr)
- **P1** createClient 的 slot 分配 CAS 自旋无退避 — `conn_pool.cc:114-129`
- **P1** wakeOneWaiter 失败重入队可能丢唤醒 — `conn_pool.cc:136-170`(复核竞态)
- **P1** idle 计数 fetch_sub 与 dequeue 非原子,瞬时假性池空 — `conn_pool.cc:104-112`
- **P2** 缺 acquire 时连接健康检查(注释已承认租约连接可能失效)— `conn_pool.h:64-65`(异步校验,勿阻塞)

### galay-mongo
整体质量:序列化基础好,拷贝开销可修;TLS URI 是成熟度缺口。
- **P0** CRC32C 逐位循环(每字节 8 次迭代),比查表慢约 10× — `mongo_protocol.cc:30-41`(换 Castagnoli 查表 / SSE4.2 `crc32` 指令)
- **P0** BSON decode 每个 CString 分配拷贝 — `bson.cc:269`(见共性 1)
- **P1** ObjectId 每次 encode 重解析 24 位 hex — `bson.cc:354-370`(内部存 12 字节二进制)
- **P1**(成熟度)Mongo URI 全部 TLS 选项 `unsupported` — `mongo_uri.cc:160-161`(生产阻断项)

### galay-etcd
整体质量:JSON 解析用了 thread_local parser;base64 与 regex 有开销;HTTPS 未实现。
- **P0** 每 kv 对 base64 decode 两次 — `etcd_internal.h:393,399`
- **P1** endpoint 用 `std::regex`(编译期成本 + 惯用法脆)— `etcd_internal.h:260-261`(改 `from_chars` + 手写解析)
- **P1**(成熟度)`https` 解析后 `secure=true` 但无下游 TLS 处理 — `etcd_internal.h:275`(生产阻断项)
- **P1** thread_local simdjson parser 未见 `reset()`,复用可能残留状态 — `etcd_internal.h:325-328`(核实)

### galay-mcp
整体质量:JSON 处理扎实;parser 每次分配 + 热路径 string 拷贝。
- **P0** `JsonDocument::parse` 每次 `make_unique<simdjson::parser>` — `mcp_json.cc:13-14`(thread_local + reset,参考 etcd)
- **P1** 请求 method / id 每次 view→string 拷贝 — `json_parser.cc:40,84`
- **P2** stdio `getline` 时持 `m_inputMutex`,慢 IO 阻塞协程 — `stdio_transport.cc:335-347`(分离 IO 锁与 parse 锁)

### galay-tracing(内部数据路径;嵌入/解耦见另两份文档)
整体质量:架构隔离好;优化点集中在批处理/序列化 churn 与 attribute 分配。
- **P0** Span attribute 用 `std::vector`(上限 32)— `span.h:513`(small_vector 消除堆分配)
- **P0** attribute name/value 每次从 view 拷成 string — `span.cc:85-126`(延迟到序列化)
- **P0** OTLP body 每批新建 string 且遍历 spans 两次 — `otlp_http_exporter.cc:281-353`(复用 buffer/arena)
- **P1** `currentContext()` 每次拷贝整个 TraceContext + tracestate — `context_storage.cc:38-43`(热路径返回 const ref / 缓存)
- **P1** Sampler `traceIdHighBits` 每次移位 8 字节 — `sampler.cc:24-30`(在 TraceId 预算高 64 位)
- **P1** `injectTraceparent` 每次建 55 字节 string — `traceparent.cc:76-91`(用 `std::array<char,55>`)

### 已覆盖模块的引用(不重复展开)
- **galay-kernel**:IO scheduler 三后端重复、Chase-Lev stealing 语义、timer 注释/重复取时钟、TaskState 内存布局 → 见 `architecture_defects_report.md` / `architecture_review_report.md`。补充新点:上下文跨 `co_await` 传播需入帧,后续由 tracing 集成方案单独跟踪。
- **galay-http**:router fuzzy route 不可删除、header slow-path `std::map` → 既有报告;并入共性 1/3。
- **galay-http2**:HPACK encode 热点(reserve 粗糙 + 动态表线性查找 + Huffman 临时串)、out_sched 小帧 string、静态文件路径规范化 syscall → 既有报告。
- **galay-rpc**:连接池名实不符(lease 非 socket 复用)→ 既有报告;并入共性 2。

---

## 三、建议动手顺序

1. **先做共性 1(解析用 string_view)**:选 1~2 个高 QPS 模块(redis RESP、mysql 行解析)做样板,确立“内部 view、边界 materialize”规约,再推广。收益最大、模式可复制。
2. **收敛连接池(共性 2)**:以 mysql 无锁复用为模板抽 `ConnectionPool<T>`,先替 redis(churn 最重),同时兑现 rpc 改名/复用。
3. **各模块 P0 里的“低风险确定项”**:mongo CRC32C 查表、mcp/etcd parser 复用、ws 单趟写、ssl `ERR_clear_error` 移位、tracing small_vector——这些改动面小、收益明确。
4. **成熟度阻断项单独立项**:mongo TLS URI、etcd HTTPS——不是性能问题而是能力缺口,应进模块成熟度矩阵并排期。
5. **每一项改动都先 profiling 确认热度、改后跑 benchmark 对比**:接住前面性能报告要建的 p95/p99 门禁,避免盲改。

---

## 四、引用
- `performance_analysis_report.md` — 性能基线与热点方向
- `architecture_review_report.md` / `architecture_defects_report.md` — kernel/http/http2/rpc 架构与热路径
- 可观测性/tracing 集成方案 — 用来验证本文优化是否真有收益,后续单独跟踪
- `docs/c-abi-encapsulation-optimization.md` / `docs/rust-ffi-zero-overhead-guide.md` — C ABI 与 FFI

---

## 五、2026-07-02 落地记录

本轮按模块并行处理低风险优化项,每个模块均补充或复用测试与 benchmark 验证。连接池真复用、Mongo/etcd TLS 等跨行为成熟度项未强行并入本轮,保留为单独设计任务。

| 模块 | 已落地项 | 验证 |
| --- | --- | --- |
| utils | Base64 encode 预分配后索引写入;decode/canDecode 去空白路径避免整串递归拷贝 | `utils.algorithm_codecs_crypto`, `utils.resource_error_boundaries`, `benchmark_utils_resource_error_boundaries` |
| ssl | read/write 热路径仅在错误队列非空时清理;await poll 内缓存 pending encrypted output | `ssl.*`, `benchmark_ssl_tls_steady_state` |
| ws | 分片文本首帧组装避开多余 append/memmove;已校验单帧文本不重复整体验证 | `ws.ws_protocol_boundaries`, `benchmark_ws_protocol_boundaries` |
| redis | RESP simple/error/bulk string 直接 materialize 到 `RedisReply` owned string,减少解析层临时 string | `redis.t2.protocol`, `redis.t21.resp.boundaries`, `benchmark_redis_resp_parser_throughput` |
| mysql | 新增 borrowed `readLenEncStringView`/`parseTextRowView`,owned API 保持不变 | `mysql.t2.protocol`, `benchmark_mysql_lenenc_row_parse` |
| mongo | OP_MSG CRC32C 改为 Castagnoli 查表实现,新增 known-vector 覆盖 | `mongo.crc32c`, `benchmark_mongo_crc32c` |
| etcd | endpoint 解析从 `std::regex` 改为直接字符串解析和 `from_chars` | `etcd.t15.endpoint.parser`, `benchmark_etcd_b4_endpoint_parser` |
| mcp | `JsonDocument::parse` 复用 thread-local parser state,DOM document 由每个 `JsonDocument` 独立持有 | `mcp.json_document_lifetime_and_allocation`, `benchmark_mcp_json_document_parse_throughput` |
| tracing | `injectTraceparent` 使用固定 55 字节缓冲一次 materialize;sampler 高 64 位提取改为直接展开 | `tracing.traceparent`, `tracing.span_guard`, `benchmark_tracing_traceparent_throughput` |
| http | rare header 解析慢路径跳过重复 common-header 匹配与重复 map lookup | `http.parser`, `http.header`, `http.header_case`, `http.http_protocol_boundaries`, `benchmark_http_header_parser_throughput` |
| http2 | outbound bytes DATA 路径直接以当前 pending chunk 的 `string_view` 序列化,避免一层临时 payload string | `http2.h2core`, `http2.h2flow`, `http2.h2pressure`, `benchmark_http2_h2_kernel_pressure` |
| kernel | `ThreadSafeTimerManager` draining pending timers 时批内复用一次时钟快照 | `kernel.timer`, `kernel.wheel`, `benchmark_kernel_thread_safe_timer_manager` |
| rpc | pool bucket key 从 `host:port` string 改为 `RpcEndpoint` key,减少 endpoint switching 下的 key 构造 | `rpc.t40.connection.pool`, `rpc.t41.managed.client`, `rpc.t42.connection.pool.multi.endpoint`, `benchmark_rpc_connection_pool_endpoint_switching` |

未在本轮完成的高风险项:
- redis/rpc 真 socket 复用连接池:当前池语义是逻辑 lease,真复用需要重设 `RpcClient`/channel/socket 生命周期、健康检查、取消、失败和 shutdown 语义。
- Mongo TLS URI、etcd HTTPS 下游 TLS:属于能力成熟度项,不是局部性能优化。
- tracing small_vector/attribute 延迟 materialize:涉及 public storage/lifetime 语义,需单独设计。

# HTTP/2 Static Fast Path Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 为 h2c/h2 增加生产级静态响应与静态文件快路径，缩小当前 GET 空响应与 nghttpd 静态快路径的数量级差距，同时保持 HTTP/2 flow control、多路复用和现有 public API 兼容。

**Architecture:** 不把 HTTP/1.1 `sendfile` 逻辑直接搬到 HTTP/2；先在 HTTP/2 server/stream 层提供静态响应 fast path，绕过 active stream 扫描和通用 handler，再逐步复用 HTTP 静态文件 metadata、缓存、Range/ETag 语义。h2c 大文件 sendfile payload 是后置优化，因为 HTTP/2 DATA frame 需要 frame header、flow control 和公平调度；h2 TLS 不能直接使用 kernel sendfile。

**Tech Stack:** C++23, galay HTTP/2 `H2cServer`/`H2Server`, `Http2Stream`, `StreamManager`, HPACK, `Http2FrameBuilder::*Bytes`, CMake/CTest, `h2load`, optional `nghttpd`.

---

## 当前证据与约束

- 同一 `h2load`、h2c、4 client threads、server 4 workers/IO threads 下，POST 13B echo：
  - galay h2c echo: `117,490 req/s`, p95 `27.89ms`, p99 `29.18ms`, server avg CPU `~97%`, RSS `~14.4 MiB`。
  - nghttpd `--echo-upload`: `16,500 req/s`, p95 `125.86ms`, p99 `128.41ms`, server avg CPU `~317.5%`, RSS `~9.6 MiB`。
- GET 空响应 sanity check：
  - galay 应用层 echo handler: `~200,776 req/s`。
  - nghttpd 静态空文件: `~2,947,939 req/s`。
  - 该组不是公平业务对比，nghttpd 走静态文件极短路径，galay 走 `Http2ConnContext -> active streams -> stream event -> handler -> sendEncodedHeadersAndData`。
- macOS 没有 `taskset/cpuset`；本地“4 核”压测按 server 4 worker/IO threads + `h2load -t4` 限制记录，不声称硬绑核。
- 当前 `brew install nghttp2` 后，OpenSSL include 路径变化导致重新构建 `ssl` 目标可能报 `openssl/err.h` not found。新 agent 先修构建环境或使用已有 build 目录验证；不要把该环境问题误判为 HTTP/2 代码失败。
- 所有命令使用 `rtk` 前缀。
- C++ 行为改动必须先写 RED 测试，确认失败后再写生产代码。

## 非目标

- 不在第一阶段实现完整 HTTP/2 priority dependency tree。
- 不在第一阶段把 h2 TLS 做成 kernel sendfile，TLS 必须用户态加密。
- 不删除或破坏现有 `activeConnHandler`、`streamHandler`、`sendEncodedHeadersAndData` public API。
- 不把 HTTP/1.1 router 大段复制到 HTTP/2；只复用可独立的配置、metadata 和工具函数。

## [x] Task 1: 修复/确认基准构建环境

**Files:**
- Inspect: `CMakeLists.txt`
- Inspect: `src/galay-ssl/CMakeLists.txt`
- Optional Modify: CMake OpenSSL 查找逻辑（仅当本机仍无法构建时）

**Step 1: 复现构建状态**

Run:
```bash
rtk cmake --build build --target benchmark_http2_h2_multiplex_server_throughput benchmark_http2_h2_multiplex_client_throughput
```

Expected: PASS。若失败且报 `openssl/err.h` not found，先修 OpenSSL include/lib 查找，不改 HTTP/2 逻辑。

**Step 2: 验证 HTTP/2 回归**

Run:
```bash
rtk ctest --test-dir build -R '^http2\\.' --output-on-failure
```

Expected: PASS。若有失败，记录测试名、错误输出、是否与本计划相关。

**Step 3: 不提交业务改动**

本任务只建立可靠基线。若修了 CMake/OpenSSL，单独提交：
```bash
rtk git add <cmake files>
rtk git commit -m "fix: 修复 OpenSSL 构建探测"
```

## [x] Task 2: 建立 h2load 外部压测脚本与文档基线

**Files:**
- Create: `scripts/http2_h2load_compare.sh`
- Modify: `docs/modules/http2/05-性能测试.md`

**Step 1: 写脚本**

脚本职责：
- 启动 galay h2c benchmark server：`benchmark_http2_h2_multiplex_server_throughput <port> 4 1000 0`
- 启动 nghttpd h2c echo 对照：`nghttpd --no-tls -a 127.0.0.1 -n 4 -m 1000 --echo-upload`
- 用同一 `h2load` 参数压测：
  ```bash
  h2load -D10 --warm-up-time=2 -t4 -c20 -m100 -d payload http://127.0.0.1:<port>/echo
  ```
- 采样 server CPU/RSS：
  ```bash
  ps -o %cpu=,rss= -p <pid>
  ```
- 输出 req/s、p95、p99、失败数、server avg/max CPU、max RSS。

**Step 2: 跑脚本确认基线**

Run:
```bash
rtk scripts/http2_h2load_compare.sh
```

Expected:
- galay POST echo 约 `100k+ req/s`，失败为 0。
- nghttpd POST echo 可低于 galay，但只作为 echo-upload 对照。
- GET 空响应若纳入脚本，必须标记为 sanity check，不作为公平 echo 对比。

**Step 3: 写入文档**

在 `docs/modules/http2/05-性能测试.md` 追加“外部 h2load 对比”章节，注明：
- h2load 版本
- nghttpd 版本
- macOS 线程限制不是硬绑核
- POST echo 与 GET 静态空文件的语义差异

**Step 4: 提交**

```bash
rtk git add scripts/http2_h2load_compare.sh docs/modules/http2/05-性能测试.md
rtk git commit -m "test: 增加 HTTP2 h2load 外部对比基线"
```

## [x] Task 3: 设计 HTTP/2 静态响应 public surface

**Files:**
- Modify: `src/galay-http2/server/http2_server.h`
- Add or Modify: `test/http2/t87_h2static_surface.cc`
- Modify: `test/http2/CMakeLists.txt`（通常 glob 已覆盖，确认即可）

**Step 1: 写 RED 测试**

新增测试断言 builder 暴露静态响应入口，但当前不存在应编译失败：
```cpp
H2cServerBuilder builder;
builder.staticResponse("/echo", H2StaticResponse{
    .status = 200,
    .content_type = "text/plain",
    .body = ""
});
```

Run:
```bash
rtk cmake --build build --target t87_h2static_surface
```

Expected: FAIL，原因是 `H2StaticResponse` 或 `staticResponse()` 不存在。

**Step 2: 增加最小类型**

建议新增：
```cpp
struct H2StaticResponse {
    int status = 200;
    std::string content_type = "application/octet-stream";
    std::string body;
    bool allow_head = true;
};
```

在 `H2cServerConfig` 增加静态路由表：
```cpp
std::vector<H2StaticRoute> static_routes;
```

在 `H2cServerBuilder` 增加：
```cpp
H2cServerBuilder& staticResponse(std::string path, H2StaticResponse response);
```

**Step 3: GREEN**

Run:
```bash
rtk cmake --build build --target t87_h2static_surface
rtk ctest --test-dir build -R '^http2\\.h2static_surface$' --output-on-failure
```

Expected: PASS。

**Step 4: 提交**

```bash
rtk git add src/galay-http2/server/http2_server.h test/http2/t87_h2static_surface.cc
rtk git commit -m "feat: 增加 HTTP2 静态响应配置入口"
```

## [x] Task 4: 实现 HEADERS-only 静态空响应 fast path

**Files:**
- Modify: `src/galay-http2/server/http2_server.h`
- Modify: `src/galay-http2/kernel/stream_manager.h`
- Modify: `src/galay-http2/kernel/http2_stream.h`
- Add or Modify: `test/http2/t88_h2static_fastpath.cc`
- Add benchmark: `benchmark/http2/b15_h2_static_fast_path.cc`
- Modify: `benchmark/http2/CMakeLists.txt`

**Step 1: 写 RED 行为测试**

场景：
- 启动 `H2cServer`，注册 `staticResponse("/echo", body="")`。
- 用 h2c client GET `/echo`。
- 断言 status `200`、body 为空、成功结束 stream。
- 增加 HEAD `/echo`，断言 status `200`、body 为空。

Run:
```bash
rtk cmake --build build --target t88_h2static_fastpath
rtk ctest --test-dir build -R '^http2\\.h2static_fastpath$' --output-on-failure
```

Expected: FAIL，原因是路由未被 server fast path 使用。

**Step 2: 实现最小 fast path**

要求：
- 在 stream 完成 request headers 且 path 命中静态空响应时，直接 enqueue HEADERS frame bytes，设置 END_STREAM。
- 不进入 `activeConnHandler`。
- 不创建额外 DATA frame；空 body 用 `HEADERS END_STREAM`。
- 使用 `HpackEncoder::encodeStateless()` 或现有 stateless encoded headers helper，缓存 header block。
- 只处理 GET/HEAD + exact path。

**Step 3: 验证**

Run:
```bash
rtk cmake --build build --target t88_h2static_fastpath
rtk ctest --test-dir build -R '^http2\\.h2static_fastpath$' --output-on-failure
rtk ctest --test-dir build -R '^http2\\.' --output-on-failure
```

Expected: PASS。

**Step 4: benchmark**

新增 benchmark 或扩展脚本，跑：
```bash
rtk ./build/benchmark/http2/benchmark_http2_h2_static_fast_path 4 1000 0
rtk scripts/http2_h2load_compare.sh --galay-static-empty
```

目标：
- GET 空响应不再只有约 `200k req/s`。
- 若未达到 `500k req/s`，继续 profile/采样并记录瓶颈，不盲目打勾。

**Step 5: 提交**

```bash
rtk git add src/galay-http2/server/http2_server.h src/galay-http2/kernel/stream_manager.h src/galay-http2/kernel/http2_stream.h test/http2/t88_h2static_fastpath.cc benchmark/http2/b15_h2_static_fast_path.cc benchmark/http2/CMakeLists.txt
rtk git commit -m "feat: 实现 HTTP2 静态空响应快路径"
```

## [x] Task 5: 小 body 静态响应 bytes fast path

**Files:**
- Modify: `src/galay-http2/server/http2_server.h`
- Modify: `src/galay-http2/kernel/http2_stream.h`
- Modify: `src/galay-http2/kernel/stream_manager.h`
- Modify: `test/http2/t88_h2static_fastpath.cc`
- Modify: `benchmark/http2/b15_h2_static_fast_path.cc`

**Step 1: RED 测试**

覆盖：
- GET `/small` 返回 1KB body。
- HEAD `/small` 只返回 headers，不返回 DATA。
- `content-length` 正确。

Expected: 当前只支持空 body，测试失败。

**Step 2: 实现**

要求：
- 预编码 HEADERS block。
- 小 body 直接用 `Http2FrameBuilder::dataBytes(stream_id, body, true)`。
- HEADERS + DATA 进入同一 send batch/writev。
- 遵守 stream send window；body 大于当前窗口时回退普通分块路径。

**Step 3: 验证**

Run:
```bash
rtk cmake --build build --target t88_h2static_fastpath benchmark_http2_h2_static_fast_path
rtk ctest --test-dir build -R '^http2\\.h2static_fastpath$' --output-on-failure
rtk ./build/benchmark/http2/benchmark_http2_h2_static_fast_path
```

Expected: PASS，并输出 0B/1KB 两组吞吐。

**Step 4: 提交**

```bash
rtk git add <modified files>
rtk git commit -m "feat: 支持 HTTP2 小响应 bytes 快路径"
```

## [ ] Task 6: HTTP/2 静态文件 metadata 与小文件缓存

**Files:**
- Add: `src/galay-http2/server/h2_static_file.h`
- Add: `src/galay-http2/server/h2_static_file.cc`
- Modify: `src/galay-http2/CMakeLists.txt`
- Add: `test/http2/t89_h2static_file.cc`

**Step 1: RED 测试**

覆盖：
- path normalization 防止 `..` 逃逸。
- 文件不存在返回 404。
- 小文件命中返回 `content-length`、`content-type`。
- `If-None-Match` 或 `If-Modified-Since` 至少选一个先覆盖 304。

**Step 2: 实现最小 metadata/cache**

要求：
- 复用 HTTP/1.1 的 `StaticFileSetting` 思路，但不要依赖 HTTP/1.1 `HttpRouter`。
- metadata 包含 size、mtime、etag、mime、是否缓存 body。
- 小文件阈值默认 64KB。
- 热路径不每次做 `filesystem exists/stat/file_size`；通过 cache TTL 或启动时预加载。

**Step 3: 验证**

Run:
```bash
rtk cmake --build build --target t89_h2static_file
rtk ctest --test-dir build -R '^http2\\.h2static_file$' --output-on-failure
```

Expected: PASS。

**Step 4: 提交**

```bash
rtk git add src/galay-http2/server/h2_static_file.* src/galay-http2/CMakeLists.txt test/http2/t89_h2static_file.cc
rtk git commit -m "feat: 增加 HTTP2 静态文件元数据缓存"
```

## [ ] Task 7: 静态文件 GET/HEAD/Range 集成

**Files:**
- Modify: `src/galay-http2/server/http2_server.h`
- Modify: `src/galay-http2/server/h2_static_file.h`
- Modify: `src/galay-http2/server/h2_static_file.cc`
- Modify: `test/http2/t89_h2static_file.cc`
- Modify: `benchmark/http2/b15_h2_static_fast_path.cc`

**Step 1: RED 测试**

覆盖：
- GET 1KB/16KB 文件。
- HEAD 文件。
- Range 单段：`Range: bytes=0-99` 返回 206 和 `content-range`。
- Range 不合法返回 416。

**Step 2: 实现**

要求：
- 小文件缓存直接 DATA bytes。
- 中文件 read chunk + DATA frame bytes。
- chunk 大小受 `max_frame_size` 限制。
- 先不要做 h2c sendfile；避免破坏 flow control 和多路复用公平性。

**Step 3: 验证**

Run:
```bash
rtk cmake --build build --target t89_h2static_file benchmark_http2_h2_static_fast_path
rtk ctest --test-dir build -R '^http2\\.h2static_file$' --output-on-failure
rtk ctest --test-dir build -R '^http2\\.' --output-on-failure
```

Expected: PASS。

**Step 4: h2load 压测**

Run:
```bash
rtk scripts/http2_h2load_compare.sh --static-files
```

记录：
- 0B、1KB、16KB、128KB、1MB
- req/s、p95、p99、CPU、RSS、失败率

**Step 5: 提交**

```bash
rtk git add <modified files>
rtk git commit -m "feat: 支持 HTTP2 静态文件 GET HEAD Range"
```

## [ ] Task 8: 写路径批处理与 ready queue 优化

**Files:**
- Modify: `src/galay-http2/kernel/stream_manager.h`
- Modify: `src/galay-http2/kernel/http2_stream.h`
- Add or Modify: `test/http2/t90_h2_ready_queue.cc`
- Modify: `benchmark/http2/b15_h2_static_fast_path.cc`

**Step 1: RED 测试**

验证：
- 多个静态响应 stream 完成时，不需要扫描所有 active stream 才能发出响应。
- ready stream 被消费后不会重复触发。

**Step 2: 实现 ready queue**

要求：
- 请求完成时将 stream id 入队。
- active handler 只拉 ready queue，不扫全量 map/vector。
- 静态 fast path 可完全绕过 active handler。

**Step 3: 验证与 benchmark**

Run:
```bash
rtk cmake --build build --target t90_h2_ready_queue benchmark_http2_h2_static_fast_path
rtk ctest --test-dir build -R '^http2\\.(h2_ready_queue|h2static_fastpath|h2static_file)$' --output-on-failure
rtk scripts/http2_h2load_compare.sh --static-empty
```

Expected:
- PASS。
- GET 空响应吞吐继续提升，若退化则回退 ready queue 或限定使用范围。

**Step 4: 提交**

```bash
rtk git add <modified files>
rtk git commit -m "perf: 优化 HTTP2 ready stream 调度"
```

## [ ] Task 9: 评估 h2c sendfile payload 特化

**Files:**
- Modify only if benchmark proves useful: `src/galay-http2/kernel/stream_manager.h`
- Modify only if benchmark proves useful: `src/galay-http2/server/h2_static_file.cc`
- Modify: `docs/modules/http2/05-性能测试.md`

**Step 1: 先写评估，不写实现**

记录设计限制：
- 只允许 h2c 明文。
- 每个 DATA frame 仍要先写 9 字节 header。
- payload 可用 socket sendfile，但必须处理 partial sendfile、flow control 暂停、多 stream 插队。
- h2 TLS 禁用。

**Step 2: 做 prototype benchmark 分支或临时代码**

仅在 1MB/10MB 静态文件场景有明显瓶颈时实施。

**Step 3: 决策**

若收益低于 10% 或代码显著复杂，不合入，只记录“不做”的证据。

## [ ] Task 10: 最终回归、对比报告与计划勾选

**Files:**
- Modify: `docs/modules/http2/05-性能测试.md`
- Modify: `docs/plans/2026-06-20-http2-static-fast-path-plan.md`

**Step 1: 全量回归**

Run:
```bash
rtk cmake --build build --target t88_h2static_fastpath t89_h2static_file benchmark_http2_h2_static_fast_path
rtk ctest --test-dir build -R '^http2\\.' --output-on-failure
```

Expected: PASS。

**Step 2: 外部对比压测**

Run:
```bash
rtk scripts/http2_h2load_compare.sh --all
```

Expected:
- 每个场景失败率为 0。
- 表格包含 galay 与 nghttpd 的 req/s、p95、p99、CPU、RSS。

**Step 3: 更新文档**

在 `docs/modules/http2/05-性能测试.md` 写入：
- 环境
- 命令
- 结果表
- 和 nghttpd 对比解释
- 剩余瓶颈

**Step 4: 勾选计划**

每个 Task 只有在对应验证通过后才把 `[ ]` 改为 `[x]`。不得因为代码写完就打勾。

**Step 5: 提交**

```bash
rtk git add docs/modules/http2/05-性能测试.md docs/plans/2026-06-20-http2-static-fast-path-plan.md
rtk git commit -m "docs: 记录 HTTP2 静态快路径压测结果"
```

## 验收标准

- GET/HEAD 静态空响应不进入通用 active stream handler。
- 小 body 静态响应使用预编码 HEADERS + DATA bytes batch。
- 静态文件支持 GET、HEAD、404、304、Range 单段、416。
- h2c/h2 都支持静态文件语义；h2 TLS 不使用 sendfile。
- `h2load` 外部对比脚本可重复运行。
- 全量 `http2.*` CTest 通过。
- 文档明确区分 POST echo、GET 静态空文件、静态文件不同大小，不做误导性横向结论。

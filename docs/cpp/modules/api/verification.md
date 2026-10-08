# API 验证记录

本页前半部分保留最初外置目录版本和策略增补的历史证据；涉及资源目录或
`--assets` 的旧命令不是当前接口。默认内嵌版本的实际结果见末尾
“默认内嵌 UI”一节，包含部署无资源、安装搬迁、构建校验和浏览器验收。

本页记录 HTTP/1 首版 P1–P4 与 `ApiServer<Policy>` 增补的实际执行结果。
基线与逐轮红色测试、GO / NO-GO 见
`docs/plans/2026-10-06-api-documentation-plan.md`。实现验收阶段未执行
commit/tag/push；用户后续授权的 v6.1.0 发布与复验单独记录在末节及
`docs/release_note.md`，不修改历史验证范围。
Serde 已发布 v0.4.0，submodule 保持
`e93269257f04166db056da4ef9ee893d8cc096bf`，没有改 tag 或回退 gitlink。

以下“P3 初轮”保留初次实现的历史记录，最终状态以本页末尾的集成验收为准。

## P3 初轮

1. 阅读 AGENTS.md、2026-10-07 冻结契约和 HTTP/router/serde 公共源码。
2. 先落 `test/cpp/api/t5_docs.cc`，再实现最小端到端 docs 服务并运行真实
   红色测试；修复后以 `api.docs` 绿灯为完成标准。
3. 固定导入官方 5.17.14 资源，核对 npm integrity 和所有文件 SHA256。
4. 完成 typed 示例和 `--export` 提前退出，交由主 agent 串行 `-j1`
   构建、安装消费、OpenAPI validator 和断外网 Playwright 验收。

### 实际执行

资源从官方 npm registry 下载：

```bash
curl --fail --silent --show-error --location \
  https://registry.npmjs.org/swagger-ui-dist/5.17.14
curl --fail --silent --show-error --location \
  https://registry.npmjs.org/swagger-ui-dist/-/swagger-ui-dist-5.17.14.tgz \
  -o /tmp/galay-swagger-ui-5.17.14/swagger-ui-dist-5.17.14.tgz
sha256sum --check SHA256SUMS
```

下载成功，npm `dist.integrity` 的 SHA512 通过 Node `crypto` 重新计算核对。
逐文件校验在 `assets/swagger-ui` 执行，CSS/bundle/preset/两个 favicon/
LICENSE/NOTICE 共 7 项 `OK`。归档和每个文件的 SHA256 见该目录记录。

测试源码先落盘；首轮主 agent 编译真实失败于
`docs.cc:180: Task does not name a type`。已改为 `kernel::Task<void>`，
跨模块包含改用 canonical include。编译失败不计作行为红色证据。

真实行为红色：

```bash
ctest --test-dir build/api-docs -R '^api.docs$' --output-on-failure
```

返回 exit 8，`api.docs` 1/1 Failed，具体输出为
`api.docs: failed install must remain retryable`。根因是初始化配置只登记
`reflect_fields(type_identity<UiConfiguration>)`，未提供 serde 实际编码所需
的 `reflect_fields(const UiConfiguration&)`，导致加载补全后配置序列化
仍报 unsupported type。主 agent 的 GDB 同时确认 `retry.error().code` 为
`encoding_error`，message 为
`Swagger UI initialization configuration: unsupported type in JSON serializer`。
已补值反射入口，且统一增加所有方法的 router
查找及参数化 endpoint 匹配预检，避免原 router 的静默覆盖和部分注册。

修复后的同一命令已由 P3 实际复跑：exit 0，`api.docs` 1/1 Passed，
测试 0.13 s、总计 0.14 s。主 agent 的第四轮 API 行为验收为 5/5 Passed。
测试包含每个资产缺失、失败后可重试、空 / 非普通文件、恶意路径、生成
路径超长、精确 / fuzzy / 非 GET router / endpoint 冲突、重复安装、默认 /
自定义 / 根 UI 路径，以及复制 handler、销毁 PreparedApi 和删除磁盘资产
后的真实 HTTP loopback，校验页面、初始化 JSON、OpenAPI 和全部资产字节。

修复包括值反射入口、完整路径预检和发送路径的 checked header / close。
生产代码没有新增 `throw`、`try`、`catch` 或阻塞同步锁；未改公共契约、
HTTP/binding/schema、CMake/BUILD、计划或现有 serde 改动。
`assets/swagger-ui/BUILD.bazel` 由主 agent 管理，P3 未覆盖。

上述初轮完成时，浏览器、export、validator 和最终安装仍待主 agent 验证；
没有将初轮代理结论用作 P3 整体验收。后续结果如下。

## 策略增补

在更新主计划、冻结接口和分工后，三个 agent 分别提交文档不漂移测试及
安装消费、HTTP/1 策略测试、docs 策略及示例文档。主 agent 实现
`ApiServer<Policy = NoSwagger>`、错误映射、构建接线并进行串行验收。
没有为底层 HTTP 引入 serde / OpenAPI / Swagger 依赖，也没有新增虚函数。

首轮真实行为测试：

```bash
ctest --test-dir build/api-docs \
  -R '^api\.(policy|policy_document)$' --output-on-failure
```

返回 exit 8，0/2 通过：

- `api.policy`：NoSwagger 未注册 docs 路由，但原 HTTP server 无 handler
  分支硬编码 200，body 为 `404 Not Found`。只将该分支状态改为 404，保留
  严格状态断言；没有重构 HTTP 模板或请求分发。
- `api.policy_document`：测试错误地把 JSON optional nullable 套用到 query。
  文本 query 没有 null token，已有 binding 正确输出非 required、非 nullable
  schema；修正测试假设，保留数值约束及 JSON body/output nullable 断言。

修复后重构建 API、示例和受 HTTP 头影响的六个回归目标，60/60 步成功。
两个 agent 分别实际复跑各自 policy / policy_document 测试通过。

`api.policy` 覆盖 concept 正负例、move-only 非默认构造策略、无虚函数、
不可复制/移动、延迟 Runtime、失败不消费 router、缺资产后重试、默认无 docs、
自定义文档路径、占用端口、重复 start、stop 后禁用 restart、真实 GET/POST/
spec/UI，以及 handler、policy 和 ApiContext 跨协程挂起后的生命周期。
Linux 测试直接比较启动前的 fd/thread 数；policy 释放时检查监听已经关闭。

`api.policy_document` 覆盖八个 typed 操作、框架/业务响应、HEAD/204、输入/
输出 required、optional nullable、uint64 精度、字段/容器/enum 约束及同一份
不可变文档。原始 router 与 policy 新增的非 typed 路由不被伪造为 OpenAPI
操作。三种策略及 builder/router 销毁前后，文档指针和字节保持一致。

## 最终集成

环境：GCC 14.2.0、CMake 3.28.3、Ninja、C++23 include、Linux epoll、Debug。
源码构建目录 `build/api-docs`；ASan/UBSan 为独立 `build/api-docs-asan`。
构建均 `-j1`，没有并发进行多个编译或安装任务。

```bash
ctest --test-dir build/api-docs -L api --output-on-failure
```

**10/10 通过**，总 27.17 秒，含八个 API 行为测试、配置矩阵和安装消费。
配置矩阵验证 API=OFF 且无 serde 源码时不引入 API/serde/UI 构建依赖；
API=ON 的 HTTP/serde 开关或 serde 源码缺失均明确配置失败。

随后直接构建缺 serde 的 OFF fixture：

```bash
cmake --build build/api-docs/api-configure/api_off --target galay-http -j1
```

**80/80 步成功**，没有 API / serde / Swagger 依赖。GCC14 优化构建出现既有
kernel TaskResumeState atomic 的 `-Wstringop-overflow` 警告；相关 kernel
文件没有改动，不将“成功构建”夸大为全库无警告。

```bash
ctest --test-dir build/api-docs \
  -R '^(http\.(router|router_check|parser|http_limits|http_protocol_boundaries|router_match_source|server_nodelay_config|keepalive_lifecycle|accept_hook|blacklist)|serde\.(struct_formats|contract|reflect|json|errors|serialize|roundtrip|parse_fixture|toml_edges|parser_boundaries|wide_fields))$' \
  --output-on-failure
```

**21/21 通过**，总 3.61 秒：HTTP 10 项、serde 11 项。HTTP 404 修复后的
相关 server header 依赖已重建，不复用修复前的二进制作证据。

安装前缀 `build/api-docs/api-install/prefix`。实际执行 install、独立 configure、
`--parallel 1` build 和 CTest；消费工程仅 `find_package(galay CONFIG REQUIRED)`
及 `galay::api`，包括安装后的 `api_server.h`、三种策略和本地 UI 资源。
**installed.api / installed.export 2/2 通过**，缺失 UI 的 export 仍成功。
日志为 `build/api-docs/api-install/{install,configure,build,ctest}.log`。

原 serde 消费集成也保留并复验：
`ctest --test-dir build/api-docs -R '^serde.install_consumer$' --output-on-failure`，
**1/1 通过**，总 0.47 秒，含 t14 字段契约；此次安装/消费流程实际重新运行，
consumer 编译目标已 up-to-date，没有将增量无编译工作声称为全量重建。

## 外部验收

```bash
build/api-docs/test/cpp/api/api_t1_schema \
  --export-schemas build/api-docs/document-schema.json
strace -f \
  -e trace=%network,clone,clone3,openat,epoll_create,epoll_create1,eventfd,eventfd2,io_uring_setup \
  -o build/api-docs/export-policy.strace \
  build/api-docs/examples/cpp/api/example_api_e1_users \
  --export build/api-docs/openapi-policy.json --assets /deliberately-missing-ui
```

两条命令均返回 0。trace 无 socket/bind/listen/connect、线程创建、epoll/
eventfd/io_uring 创建或 UI 文件访问，export 不创建 Runtime、不监听、不加载 UI。

```bash
PLAYWRIGHT_BROWSERS_PATH=$PWD/build/api-docs-tools/browsers \
PLAYWRIGHT_MODULE=$PWD/build/api-docs-tools/node_modules/playwright \
node test/cpp/api/browser_acceptance.cjs \
  build/api-docs/examples/cpp/api/example_api_e1_users \
  assets/swagger-ui build/api-docs/acceptance-policy

PYTHONPATH=$PWD/build/api-docs-tools/python \
python3 test/cpp/api/validate_openapi.py build/api-docs/openapi-policy.json \
  --served build/api-docs/acceptance-policy/served-openapi.json \
  --schemas build/api-docs/document-schema.json
```

Chromium 141.0.7390.37，desktop 1440x1000 和 mobile 390x844：0 外部请求、
0 浏览器/资源错误、0 水平溢出；官方 Swagger UI 实际 GET 200 / POST 201。
主 agent 目视检查 desktop-executed/mobile 截图；脚本停止服务并正常退出 0。
截图、browser.json 和 server.log 位于 `build/api-docs/acceptance-policy`。

`openapi-spec-validator 0.7.2` 通过 OpenAPI 3.1 两操作文档；
Draft 2020-12 JSON Schema 校验 **45 个边界正负例通过**。
`cmp` served spec 与 export 返回 0，字节完全一致。
官方固定 `swagger-ui-dist@5.17.14`，本地资源 hash **7/7 OK**；
Apache-2.0 许可证及来源、npm integrity 均保留，没有 CDN fallback。

HTTP 404 修复及全部相关 header 重建后，上述 export / strace / 浏览器 /
validator / cmp / 资产 hash 再执行一遍，结果保持通过。最终证据路径为
`build/api-docs/openapi-policy-final.json`、`export-policy-final.strace` 及
`acceptance-policy-final`；最后两张 desktop/mobile 截图也已目视检查。

## Sanitizer

```bash
cmake --build build/api-docs-asan \
  --target api_t1_schema api_t2_openapi api_t3_binding api_t4_http \
  api_t5_docs api_t6_contract api_t7_policy api_t8_policy_document -j1
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build/api-docs-asan \
  -R '^api\.(schema|openapi|binding|http|docs|contract|policy|policy_document)$' \
  --output-on-failure
```

最终增量构建 **40/40 步成功**，最新八项 API 测试 **8/8 通过**，总 1.60 秒。
包含最后的 schema/default-object/null-member 修改、HTTP 404 修复和新增策略
测试；address / undefined / leak 检查未报告错误，不使用旧六项绿灯代替。

## 限制

- 首版验收只覆盖明文 HTTP/1、C++23 include、GCC14、epoll；Bazel、mcpp、
  native module、Clang、io_uring 等 API 组合未实际运行，不能把接线视为验收。
- HTTP/2、HTTPS typed 路由、WS/AsyncAPI、multipart、多值 query 和递归 DTO
  不在本轮实施范围。文本 query 继承现有单值 map / URI parser 限制。
- 请求热路径不生成 schema、不读取 UI 文件、不调用 policy.install 或改注册表；
  本轮未进行 typed API 吞吐/延迟对比，不宣称“零开销”或性能提升。
- 自定义 policy 的失败原子性与借用责任由其实现者保证；concept 不替代语义验证。
  ApiServer 一旦进入底层启动即为单次初始化，监听失败或 stop 后需新实例及新 API。

## 默认内嵌 UI（2026-10-07）

增补范围只涉及资源来源、文档策略、构建和部署消费；没有重写 HTTP 热路径、
serde codec 或 schema。`DocsConfig` 仅保留 spec/UI 路径。`HttpSwagger{}`
及 `install_docs` 使用构建期生成的字节；自定义策略显式调用
`install_docs_from_directory`，文件错误不会被内嵌资源或 CDN 掩盖。

构建期固定 `swagger-ui-dist@5.17.14`，使用 CMake 的 HEX 读取生成私有
translation unit，不修改官方 JS/CSS/PNG/LICENSE/NOTICE。七个上游文件
逐项 SHA256 校验；九个服务资源还包括版本、来源和校验清单。许可证与
来源元数据另安装到 `share/galay/swagger-ui`，但默认服务和 package config
不依赖它。安装消费工程不再设置资源路径宏。

### 行为和回归

- 先新增 `api.embedded` 并实际执行：旧实现返回空目录 `resource_error`，
  CTest exit8。内嵌实现后相同测试通过，确认空部署目录可安装默认资源。
- `cmake --build build/api-docs --target galay-api api_t9_embedded -j1`：
  13/13 步完成。示例/其余核心目标随后18/18、docs/policy目标12/12步完成。
  README 改动实际触发重生成；随后 API 构建显示 `ninja: no work to do`。
- `api.embedded_generator` 实测确定性、损坏/缺失/空资源失败、非法/重复
  SHA256 清单失败、README 变动进入生成结果。九文件和生成器均登记为
  Ninja 显式输入；原上游 `sha256sum --check SHA256SUMS` 为7/7 OK。
- docs/policy 测试覆盖九资源 HTTP 字节、PNG NUL、Content-Type、全部默认/
  自定义/根文档路径冲突；文件策略逐项缺失、空、非普通文件失败且不 fallback。
  自定义内容与内嵌不同，删除目录/销毁 prepared/复制 handler 后仍提供原字节。
- `ctest --test-dir build/api-docs -L api --output-on-failure`：最终12/12通过，26.40秒，
  含九个行为测试、生成器负例、配置矩阵及安装消费。配置矩阵增加了
  API=ON、有serde但缺构建期UI资源的明确配置失败；API=OFF fixture没有
  serde及资源目录，仍独立配置。其 HTTP 构建复验是 `no work to do`，
  不是冒称本轮重新全量编译。
- HTTP10 + serde11 的原21项定向回归：21/21通过，3.93秒。
- ASan/UBSan增量串行构建30/30步成功；九个行为测试9/9通过，2.31秒，
  `detect_leaks=1:halt_on_error=1` 和 UBSan halt/stacktrace 开启，无报告。

### 安装和部署

`api.install_consumer` 实际安装后删除其
`share/galay/swagger-ui`，将整包移到 `relocated-prefix`，再用独立工程的
`find_package(galay)` + `galay::api` 配置/构建。首次新增loopback测试的
const响应调用既有非const `header()` 导致编译失败；仅修正测试局部const后
通过，无HTTP接口扩张。独立消费3/3通过：类型契约、真实HTTP GET/docs/
九资源、无Runtime的export。`ldd`确认加载移后前缀的三个Galay库。

示例从空 `/tmp` 工作目录执行 `--help` 返回0，帮助无资源参数；`--export`
返回0，3525字节且工作目录仍空。移除的 `--assets` 明确返回2，不保留兼容
参数。正常可执行文件和链接库部署要求仍然存在，内嵌不等于静态链接全依赖。

### 浏览器和导出

```bash
env STRACE_OUTPUT=$PWD/build/api-docs/serve-embedded.strace \
  PLAYWRIGHT_BROWSERS_PATH=$PWD/build/api-docs-tools/browsers \
  PLAYWRIGHT_MODULE=$PWD/build/api-docs-tools/node_modules/playwright \
  node test/cpp/api/browser_acceptance.cjs \
    build/api-docs/examples/cpp/api/example_api_e1_users assets/swagger-ui \
    build/api-docs/acceptance-embedded-traced

env PYTHONPATH=$PWD/build/api-docs-tools/python \
  python3 test/cpp/api/validate_openapi.py build/api-docs/openapi-embedded.json \
    --served build/api-docs/acceptance-embedded-traced/served-openapi.json \
    --schemas build/api-docs/document-schema-embedded.json
```

Playwright Chromium141.0.7390.37在空部署目录启动示例，九资源逐字节相等；
desktop1440x1000/mobile390x844均0外部请求、0资源/console错误、0横向
溢出，UI真实GET200/POST201。截图已目视检查。`strace -f -s4096 -e trace=%file`
覆盖启动、浏览器请求及退出，无UI文件/资源目录访问；也不向cwd抽取文件。
服务SIGTERM正常退出0，无遗留测试进程。

`--export`独立strace（从`/tmp`运行）跟踪file/network/clone/epoll/eventfd/
io_uring创建：无UI文件访问、无网络或Runtime创建。OpenAPI校验器0.7.2
通过3.1两操作文档，Draft202012的45个边界通过，served/export `cmp`相等。
证据在 `openapi-embedded.json`、`export-embedded.strace`、
`serve-embedded.strace`、`acceptance-embedded-traced/` 以及
`api-install/{install,configure,build,ctest}.log`。

本增补结论：**GO**。完整命令、首次失败/修复和文件归属同时追加在
`docs/plans/2026-10-06-api-documentation-plan.md` §5.6与执行记录；本地
计划继续被忽略，没有强制加入版本控制。最终schema边界fixture由本轮
`api_t1_schema --export-schemas`重新生成，不依赖旧输出冒充新验收。

本增补验收覆盖 GCC14、C++23 include、Linux epoll及共享库。Bazel、mcpp/
native module、Clang、io_uring、静态库与其他平台组合未实际运行，不作GO。
Bazel已接线同一生成器；mcpp需先按manifest注释预生成资源source，尚未验收。
没有性能测量，不宣称零开销或吞吐提升。内嵌增补验收时尚未执行
commit/tag/push，serde v0.4.0 submodule及原集成改动保留；后续发布见末节。

## 交付状态

P1–P4（GCC14 / C++23 include / epoll / 明文 HTTP/1）及文档策略增补：**GO**。
其余上面列出的组合保持未验证或范围外，不扩大验收结论。
最终 `git diff --check`、生产异常/锁/RTTI/serde-detail 扫描通过；
计划目录仍被 ignore，未调整规则或 force-add。serde 发布提交/tag 未动，
submodule 工作区干净，没有遗留示例服务或测试进程。

## v6.1.0 发布复验

2026-10-07 用户授权父仓库提交并打新中版本 tag。版本从 v6.0.0 升至
v6.1.0，CMake / Bazel / mcpp 元数据同步；serde v0.4.0 不重新发布。
发布审查发现新增 `ApiServer::isRunning` 未遵守已确认的命名规则，先增加
要求 `is_running()` 返回 bool/noexcept、拒绝旧名的编译契约，实际构建在
两个 static_assert 失败。随后仅迁移新 API、测试、安装消费者、示例及
文档；原 `HttpServer::isRunning` 保持不变，没有旧名兼容包装。

发布前实际复跑：

```bash
cmake --build build/api-docs --target api_t1_schema api_t2_openapi api_t3_binding api_t4_http api_t5_docs api_t6_contract api_t7_policy api_t8_policy_document api_t9_embedded example_api_e1_users t14_contract -j1
ctest --test-dir build/api-docs -L api --output-on-failure
ctest --test-dir build/api-docs -R '^serde.install_consumer$' --output-on-failure
cmake --build build/api-docs-asan --target api_t1_schema api_t2_openapi api_t3_binding api_t4_http api_t5_docs api_t6_contract api_t7_policy api_t8_policy_document api_t9_embedded -j1
env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build/api-docs-asan -R '^api\.(schema|openapi|binding|http|docs|contract|policy|policy_document|embedded)$' --output-on-failure
```

主构建增量8/8步成功，API **12/12** 通过（65.02秒），内含生成器、
API OFF/缺依赖配置及安装搬迁。HTTP/serde沿用本页21项定向正则再次
**21/21** 通过（3.80秒）。ASan增量4/4步，九项行为测试 **9/9** 通过
（2.38秒），无sanitizer报告。独立安装消费 **3/3** 通过，包含新的
`is_running` 编译契约及真实HTTP；安装包版本6.1.0、serde精确依赖0.4.0，
`ldd`确认三个Galay库来自移后前缀。serde安装消费 **1/1** 通过（0.31秒）。
API OFF HTTP增量构建为no work to do，不冒称全量重编译。

重新生成 `document-schema-v6.1.0.json` 和 `openapi-v6.1.0.json`；以
`validate_openapi.py --served .../acceptance-v6.1.0/served-openapi.json --schemas
.../document-schema-v6.1.0.json` 验证OpenAPI3.1、45个schema边界通过，
`cmp`确认served/export逐字节相同。从 `/tmp` 执行export strace无UI文件、
网络或Runtime创建。带 `STRACE_OUTPUT=.../serve-v6.1.0.strace` 的浏览器
验收在空部署目录运行，Chromium141 desktop1440x1000/mobile390x844均
0外部请求、0错误、0横向溢出；九资源字节一致，真实GET200/POST201，
截图已检查，启动/请求trace无UI文件访问。七项上游SHA256均OK。

发布复验 **GO**；仍只覆盖GCC14/include/Linux epoll/共享库，未扩大原有
未验证平台、模块组合或性能结论。计划和原始证据仍被忽略，不强制加入提交。

## 四传输增补（2026-10-07）

本轮基于实际代码继续实现 HTTP/1 明文、HTTPS（HTTP/1 over TLS）、h2c
prior knowledge 和 H2 over TLS。没有升级 OpenAPI 3.1.0 或 Swagger UI
5.17.14，没有新增 Upgrade、AsyncAPI、WS 消息契约、OAuth 服务端或 C ABI。
基线工作区干净；原有用户演示 PID 503632 / 18080 不停止、不占用。
所有新增 loopback 和浏览器 fixture 从操作系统申请私有端口，生成自己的
本地证书，清理自己的进程，不依赖现有演示。

### 设计与回归

- `ApiContext` 保留借用语义请求；H2 adapter 在协程 frame 内持有规范化的
  `HttpRequest`。`PreparedApi` 保留原生 `HttpRouter`，typed/doc handler 改为
  返回自持响应，四种传输复用同一条绑定/校验/JSON/错误映射链路。
- `ApiServerConfig` 选择已启用的原生引擎。预检/策略失败可重试；进入原生
  初始化后仍保持单次使用；启动失败/stop 立即释放 Runtime、TLS 和 listener，
  共享 document 不清除。原生 bind/listen 失败同步保留原因。
- HTTP/2 使用真实 stream、HEADERS/DATA 和流量控制；完整请求之后才绑定。
  reset/peer close 唤醒接收或发送等待，挂起业务返回后检查关闭状态，不向
  已取消的 stream 发响应。reset 不抢占任意业务协程。

实际红色证据与修复（失败未降级为成功）：

1. `api.startup` 首次失败于 h2c 把 bind 失败报告为 running；监听改为同步
   初始化。扩展后四传输启动、策略重试、冲突、证书/私钥/CA 失败及
   `/proc/self/fd`、线程回收检查通过，见 `api-startup-expanded.log`。
2. Node/nghttp2 的连续资源请求暴露 HPACK 动态表扩容颠倒索引顺序。
   `http2.hpack` 先以独立 newest-index 期望复现，随后修复扩容复制顺序；
   红色证据 `api-hpack-red.log`，四传输复验 `api-transports-expanded.log`。
3. trailing HEADERS 曾再次 spawn handler；`http2.protocol_correctness` 的
   请求完成/调用次数回归先失败，随后按 initial-headers 标记只登记一次，
   见 `api-trailers-red.log` 和 `api-trailers-green.log`。
4. 浏览器 H2 首次 `page.goto(/docs, networkidle)` 真实超时 30 秒。
   Chromium netlog 显示 ALPN `h2`、peer INITIAL_WINDOW_SIZE=6291456，但三项
   大资源均只发送 65535 字节。根因是 SETTINGS 更新了已有 stream，未更新
   后续新建/pool stream 的窗口。`api-new-window-red.log` 复现 0/4096/6MiB/
   最大窗口初始化失败，修复在原生 `create_stream` 初始化双向窗口。
   未更改浏览器超时、协议断言或改用 HTTP/1 fallback。
5. 缓存的 deprecated shared_ptr 原子自由函数替换为单次发布的标量原子
   状态：Empty/Publishing/Ready，release/acquire 之后只读普通 immutable
   shared_ptr。读者不自旋、不阻塞；发布期间允许 cache miss，发送仍持有
   自己的 body。`http2.h2_body_cache` 覆盖 16 writers / 8 readers / 20 rounds、
   唯一发布、null、空 body 和 slot 销毁后的 snapshot 生命周期。
6. GCC 的 `TaskResumeQueue` 原子访问 overflow warning 不是靠关闭诊断处理：
   `release_state` 可能因 borrowed view 返回 null。移动 borrowed view 的
   `kernel.ringfb` 真实 SEGFAULT 见 `api-resume-borrowed-red.log`；随后检查
   转移结果，绿灯见 `api-resume-borrowed-green.log`。

扩展 validator 已实际运行：

```bash
PYTHONPATH=build/api-docs-tools/python python3 test/cpp/api/validate_openapi.py \
  build/naming-release/test/cpp/api/transport-evidence/openapi.json \
  --served build/naming-release/test/cpp/api/transport-evidence/http-openapi.json \
  --served build/naming-release/test/cpp/api/transport-evidence/https-openapi.json \
  --served build/naming-release/test/cpp/api/transport-evidence/h2c-openapi.json \
  --served build/naming-release/test/cpp/api/transport-evidence/h2-openapi.json \
  --observations build/naming-release/test/cpp/api/transport-evidence/observations.json
```

`api-validator-initial.log`：OpenAPIV31SpecValidator 通过 11 operations；四份
served 与 export 逐字节一致，96 条实际 request/response 观察（每传输 24）
与参数类型/来源、body schema、成功状态、错误响应和无 body 契约一致。

### 浏览器与警告复验

浏览器的窗口修复后，实际执行（不是 HTTP/1 降级结果）：

```bash
PLAYWRIGHT_MODULE=$PWD/build/api-docs-tools/node_modules/playwright \
PLAYWRIGHT_BROWSERS_PATH=$PWD/build/api-docs-tools/browsers \
STRACE_OUTPUT=$PWD/build/naming-release/api-browser-final-files.strace \
node test/cpp/api/browser_acceptance.cjs \
  build/naming-release/examples/cpp/api/example_api_e1_users assets/swagger-ui \
  build/naming-release/api-browser-final-transports
```

`api-browser-final.log` 返回 0。Chromium 141.0.7390.37 在 desktop 1440x1000 和
mobile 390x844 上，HTTP、HTTPS、H2 的页面、初始化脚本、文档、资产及真实
GET 200 / POST 201 Try it out 全部通过；CDP 对每个响应断言 HTTP/HTTPS 为
`http/1.1`、H2 为 `h2`，零外部请求、零 JS/资源错误、无水平溢出。已查看
H2 desktop/mobile 的执行后截图。Node 同时以真实 h2c prior knowledge 请求
文档及九资产；Chromium 明文导航返回原生 HTTP/1 fallback 404，在证据中标明
浏览器限制，不将其计为 h2c UI/Try it out 通过。HTTPS/H2 浏览器显式使用本地
自签名证书例外；证书信任和主机名验证由独立 Node TLS 客户端严格检查。
每种模式在空工作目录运行，`strace` 确认启动和请求不打开 UI 资源文件，也
没有解压/落盘文件。复跑 trace 时先删除本 fixture 的旧 trace，避免把旧 PID
当作当前子进程；首次复跑的清理 `ESRCH` 是测试程序错误，修复后重新运行。

全量构建将 C/C++ warning 作为错误，不使用诊断屏蔽：

```bash
cmake --build build/naming-release --clean-first --parallel 1
```

首轮真实失败在 3935/3936 的 `benchmark_utils_move_clone_contracts`，日志
`api-full-build.log` 保留 `Bytes::assign_owned` 的 `-Werror=stringop-overflow`
证据。`length + 1` 可能先溢出再分配，修复将隐藏 terminator 长度交给原有
`malloc_bytes(length, spare_bytes)`，在加法前验证 `ptrdiff_t` 可表示的分配
上限。保留既有分配失败 `std::bad_alloc` 行为，没有新增生产 throw/try/catch；
本轮不迁移 Bytes 的既有分配接口。新增最大 size_t/ptrdiff_t 边界测试，目标
重编译 `api-bytes-warning-fixed.log` 与 clean 重编译
`api-bytes-warning-clean.log` 均返回 0，未 suppress warning。

受影响模块第一次 CTest 还发现 `http.server_nodelay_config` 失败：既有原生
HTTPS accept-plugin 探测故意不配置证书并在 TLS 握手前拒绝连接。证书完整性
检查收窄到 HTTPS 路由模式，保留既有低层 handler/plugin 能力；typed HTTPS
仍必须提供证书和私钥。`api-affected-ctest.log` 保留初次失败，不将其改为 skip。

### 全量构建和 CTest 最终结果

2026-10-07，GCC/G++ 14、Linux epoll、Release、C++23 头文件接口；所有
C++ 模块（含 API/SSL/HTTP2/serde）、C ABI、测试、示例和 benchmark 均启用。
`build/naming-release/CMakeCache.txt` 的 C/C++ flags 均为 `-Werror`，未增加
warning suppression。Boost.Asio 对照 benchmark 因本机缺少 Boost 头文件
按原有配置规则不生成，不将它算作已编译目标。原生命名模块构建未启用。

```bash
cmake --build build/naming-release --parallel 1 \
  > build/naming-release/api-full-build-final.log 2>&1
ctest --test-dir build/naming-release --output-on-failure --parallel 1 \
  --output-junit api-full-ctest-final.xml \
  > build/naming-release/api-full-ctest-final.log 2>&1
ctest --test-dir build/naming-release \
  -R '^(http|http2|ssl)\.|^kernel.ringfb$|^utils.(resource_error_boundaries|buffer_queue_ring|move_clone_contracts)$' \
  --output-on-failure --parallel 1 \
  > build/naming-release/api-protocol-regressions-green.log 2>&1
```

最终全目标构建 3928/3928 完成、退出 0，完整日志没有 `warning:`、`error:`
或 `FAILED:`。此前 clean benchmark 验证清理了整个 Ninja 工程，因此本次
不是只编译 API 的增量检查；边界测试和 benchmark 的提前重编译证据另见
`api-bytes-warning-clean.log` / `api-bytes-warning-fixed.log`。
扫描 `src/cpp`、`test/cpp`、`examples/cpp`、`benchmark/cpp`，没有原子
shared_ptr 或其 atomic free functions。C ABI 的标量原子不在替换范围内。

完整 CTest **645 项登记：604 Passed、36 原有 Skipped、5 原有 Disabled、
0 Failed**，336.38 秒、退出 0。没有传入排除、failover 或 skip 参数，没有
修改外部 fixture 的注册规则。36 项未运行涉及 C Postgres 1、kernel
io_uring/AIO 3、Redis 6、RPC/etcd 1、MySQL 10、Postgres 6、Mongo 1、etcd 8；
这些后端/外部服务能力不因本轮验收变成通过。5 个 disabled HTTP 客户端
原本依赖外部 8080 fixture。逐项名称、状态、输出在
`api-full-ctest-final.xml` 和完整日志中，不把 CTest 的 “640 tests” 简写成
640 项实际通过。API 的 14 项全部通过；受影响协议复验 94 Passed、5 个
相同的 Disabled、0 Failed，9.43 秒。

### 迁移安装和浏览器最终结果

完整 CTest 中的 `api.install_consumer` 115.07 秒通过。实际安装后删除
独立 UI metadata 目录，将 prefix 迁到 `api-install/relocated-prefix`，外部
工程仅依赖 `find_package(galay)` / `galay::api`，以 `-Werror` 构建。
消费工程 5/5 Passed，包含四传输真实 loopback、启动失败/清理、独立导出；
`ldd api-install/build/users` 的五个 Galay 共享库均来自迁移后的 prefix。

安装消费者还独立执行浏览器验证：

```bash
PLAYWRIGHT_MODULE=$PWD/build/api-docs-tools/node_modules/playwright \
PLAYWRIGHT_BROWSERS_PATH=$PWD/build/api-docs-tools/browsers \
STRACE_OUTPUT=$PWD/build/naming-release/api-browser-installed-files.strace \
node test/cpp/api/browser_acceptance.cjs \
  build/naming-release/api-install/build/users assets/swagger-ui \
  build/naming-release/api-browser-installed-transports \
  > build/naming-release/api-browser-installed-final.log 2>&1
```

退出 0。HTTP/HTTPS/H2 在两个 viewport 上的离线 UI 和真实 GET 200 / POST
201 均通过，CDP 对 H2 全链路断言 `h2`，零外部请求/错误/水平溢出。
已目视检查安装后 H2 的 desktop/mobile 执行截图。四传输九个资源逐字节
相同，空工作目录没有文件抽取，strace 无 UI 文件访问；h2c 的 Chromium
404/连接限制单独记录，不计为浏览器成功。

完整 CTest 产生的新实际请求证据再次由已有验证器检查：
`api-validator-final.log` 为 11 operations、96 request/response observations
（每传输 24）、45 个 schema 正负边界全部通过，四种 served/export 一致。

## 最终排空与无警告复验（2026-10-08）

以下结果覆盖上一节之后的连接排空、stream pool 生命周期及更严格的停止
断言；上一节日志仍保留，不能代替修改后的复验。

### Sanitizer 发现与修复

第一轮完整受影响 sanitizer 集合真实为 27 Passed / 2 Failed，红色日志保留
在 `build/naming-asan/api-sanitizers-ctest.log`：

- `api.transports` 在活跃 HTTP/1 handler 停止时泄漏 36644 bytes / 31 allocations。
  直接停止 Runtime 会截断挂起的嵌套任务及定时器。原生服务器现在在连接的
  IO owner 上登记生命周期，停止 listener 后非阻塞 shutdown 连接，异步等待
  连接/stream handler 完成，再停止 Runtime；同步 join 仅在控制线程执行。
  不引入协程阻塞锁、原子 shared_ptr 或抢占任意业务 handler。
- `http2.protocol_correctness` 泄漏 5880 bytes / 7 allocations。缓存 stream 的
  `enable_shared_from_this` weak control block 与 deleter 强持有 pool state
  形成循环。deleter 改持 weak state；pool 仍活着时归还 stream，pool 已销毁
  时删除 stream。`http2.h2pool` 增加 pool state 释放、缓存后重用及 stream
  晚于 pool 销毁的断言。

扩大 loopback 停止测试后，h2c 的原生 HTTP/1 默认 fallback keepalive 曾使
停止超时，见 `api-idle-fallback-before.log/xml`。将同一个连接 Scope 保留到
默认 fallback 完成并在 close 前解除 descriptor 登记；修复后原测试通过，
见 `api-idle-fallback-after.log/xml`，未增加超时或改成 skip。

最终 fixture 同时保留 idle socket、未完成 TLS handshake、未完成 H2 preface、
原生默认 HTTP/1 fallback keepalive、部分 JSON 请求、挂起业务和流控阻塞的
Swagger 大资源。两次 `stop()` 后必须 `active == 0` 且 `completed == started`；
reset 和停止只中断 IO，不抢占业务。连接 handler/自定义 fallback 可接管
socket，其原有生命周期责任未迁移到 typed API。

```bash
cmake --build build/naming-asan --parallel 1 --target test/cpp/api/all \
  t3_hpack t16_h2pool t27_protocol_correctness t28_h2_body_cache t97_ringfb \
  t14_resource_error_boundaries t17_move_clone_contracts t6_buffer_queue_ring \
  t6_router t7_router_check t33_http_protocol_boundaries t34_server_nodelay_config \
  t85_keepalive_lifecycle t26_h2static_tls t24_h2static_file t92_http2_nodelay_config \
  t2_loopback t12_handshake t14_security_lifecycle \
  > build/naming-asan/api-sanitizers-final-build.log 2>&1
env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build/naming-asan --parallel 1 --output-on-failure \
  --output-junit api-sanitizers-final.xml \
  -R '^api\.(startup|schema|openapi|binding|http|docs|contract|policy|policy_document|embedded|transports)$|^http2\.(hpack|h2pool|protocol_correctness|h2_body_cache|h2static_tls|h2static_file|http2_nodelay_config)$|^http\.(router|router_check|http_protocol_boundaries|server_nodelay_config|keepalive_lifecycle)$|^ssl\.t(2\.loopback|12\.handshake|14\.security\.lifecycle)$|^kernel\.ringfb$|^utils\.(resource_error_boundaries|buffer_queue_ring|move_clone_contracts)$' \
  > build/naming-asan/api-sanitizers-final.log 2>&1
```

最终串行构建 57/57 步、退出 0；ASan+UBSan **30/30 Passed**，16.24 秒，
包括四传输真实 loopback、启动失败清理、stream pool 和缓存并发测试。泄漏
检测始终开启，零 sanitizer 报告、零 skipped/disabled；这是受影响集合，
不是把完整仓库 CTest 都声称为 sanitizer 覆盖。

### 全目标构建和完整 CTest

使用上一节相同的全模块 Release/GCC14/Linux epoll 配置，C/C++ `-Werror`、
C ABI/测试/示例/benchmark 开启，未加 `-Wno-*` 或 pragma 屏蔽。本轮是在
此前 3928/3928 clean 全量构建基础上，对最终生命周期修改执行全目标增量：

```bash
cmake --build build/naming-release --parallel 1 \
  > build/naming-release/api-full-build-delivery.log 2>&1
ctest --test-dir build/naming-release --parallel 1 --output-on-failure \
  --output-junit api-full-ctest-delivery.xml \
  > build/naming-release/api-full-ctest-delivery.log 2>&1
```

最终全目标增量 **302/302 步**、退出 0；日志无 `warning:` / `error:` / `FAILED:`。
HTTP、HTTP2、MCP 的传递依赖、示例及 benchmark 均在受影响重编译范围内，
没有只构建 API 后声称全仓通过。源码扫描确认 `src/cpp`、`test/cpp`、
`examples/cpp`、`benchmark/cpp` 无原子 shared_ptr 或其 atomic free functions。
普通只读 shared_ptr 和标量/原始指针原子仍按其实际所有权和同步契约使用。

完整 CTest **645 项登记：604 Passed、36 原有 Skipped、5 原有 Disabled、
0 Failed**，310.08 秒、退出 0。未使用排除参数，未修改外部服务测试的 gating；
未运行项目及原因与上一节相同，逐项在 delivery XML/log 保留。
API **14/14 Passed**；`api.install_consumer` 118.35 秒通过，其外部消费工程
**5/5 Passed**，4.89 秒，四传输和启动清理使用最终源码。

### 离线 UI、安装迁移与实际契约

分别使用最终源码示例与重新安装迁移后的 consumer 执行浏览器验收：

```bash
env PLAYWRIGHT_MODULE=$PWD/build/api-docs-tools/node_modules/playwright \
  PLAYWRIGHT_BROWSERS_PATH=$PWD/build/api-docs-tools/browsers \
  STRACE_OUTPUT=$PWD/build/naming-release/api-browser-source-files-delivery.strace \
  node test/cpp/api/browser_acceptance.cjs \
  build/naming-release/examples/cpp/api/example_api_e1_users assets/swagger-ui \
  build/naming-release/api-browser-source-delivery \
  > build/naming-release/api-browser-source-delivery.log 2>&1
env PLAYWRIGHT_MODULE=$PWD/build/api-docs-tools/node_modules/playwright \
  PLAYWRIGHT_BROWSERS_PATH=$PWD/build/api-docs-tools/browsers \
  STRACE_OUTPUT=$PWD/build/naming-release/api-browser-installed-files-delivery.strace \
  node test/cpp/api/browser_acceptance.cjs \
  build/naming-release/api-install/build/users assets/swagger-ui \
  build/naming-release/api-browser-installed-delivery \
  > build/naming-release/api-browser-installed-delivery.log 2>&1
```

两个命令均退出 0。Chromium 141.0.7390.37 的 desktop 1440x1000 / mobile
390x844 上，HTTP/HTTPS/H2 均真实 Try it out GET 200 / POST 201。H2 所有
页面、资产、文档及业务响应由 CDP 断言 `h2`，HTTP/HTTPS 为 `http/1.1`。
零外部请求、零浏览器错误、无横向溢出；已目视检查源码及迁移消费者 H2
desktop/mobile 执行截图。h2c prior knowledge 文档/九资产由 Node 验证，
Chromium 的 HTTP/1 fallback 404 只记录连接能力限制，不算 h2c Try it out。
两套 fixture 在空目录运行，无 UI 文件访问或资源抽取；`ldd` 确认消费者
五个 Galay 库均从 `api-install/relocated-prefix` 加载，该 prefix 无 Swagger
share 目录。TLS 浏览器自签名证书例外不代替严格 CA/hostname 客户端验收。

```bash
build/naming-release/test/cpp/api/api_t1_schema --export-schemas \
  build/naming-release/api-transport-schemas-delivery.json
env PYTHONPATH=$PWD/build/api-docs-tools/python \
  python3 test/cpp/api/validate_openapi.py \
  build/naming-release/test/cpp/api/transport-evidence/openapi.json \
  --served build/naming-release/test/cpp/api/transport-evidence/http-openapi.json \
  --served build/naming-release/test/cpp/api/transport-evidence/https-openapi.json \
  --served build/naming-release/test/cpp/api/transport-evidence/h2c-openapi.json \
  --served build/naming-release/test/cpp/api/transport-evidence/h2-openapi.json \
  --schemas build/naming-release/api-transport-schemas-delivery.json \
  --observations build/naming-release/test/cpp/api/transport-evidence/observations.json \
  > build/naming-release/api-validator-delivery.log 2>&1
```

schema 导出及 validator 均退出 0：11 operations、96 observations（每传输24）
及45个 schema 正负边界通过，served/export 字节一致。把上述 evidence 前缀
替换为 `build/naming-release/api-install/build/transport-evidence` 后再次运行
同一验证器，`api-validator-installed-delivery.log` 同样通过96个实际观察。
H2/h2c evidence 各记录24并发 stream（peak24）、trailers、长度不符 reset、
未完成 body reset、业务挂起 reset、流控阻塞资源 reset 及3次 abrupt peer close。
成功状态、绑定来源、JSON/DTO 失败与业务错误均与文档一致。

### Feature 矩阵和未覆盖范围

```bash
cmake -DGALAY_SOURCE_DIR=$PWD \
  -DGALAY_BINARY_DIR=/tmp/galay-api-transport-validation \
  -DGALAY_CXX_COMPILER=/usr/bin/g++-14 -DGALAY_C_COMPILER=/usr/bin/gcc-14 \
  -P test/cpp/api/transport_matrix.cmake \
  > build/naming-release/api-transport-matrix-delivery.log 2>&1
python3 scripts/common/106_gen_module_prelude.py --check
git diff --check
```

矩阵退出 0。SSL/HTTP2 的 OFF/OFF、OFF/ON、ON/OFF、ON/ON 各执行
`-Werror` API 构建及 **14/14 Passed**（包括移后安装消费），分别96.74、126.86、
105.60、149.65秒；各构建及消费者 build.log 无 warning/error。细节在
`/tmp/galay-api-transport-validation/api-transport-matrix/ssl-*-{configure,build,ctest}.log`。
API=OFF configure 检查现在匹配 Ninja target rules，避免绝对 fixture 路径中
`galay-api` 字样造成错误依赖判断；缺 HTTP/serde/构建期 UI 的失败仍明确。
14个模块 prelude 全部与生成器一致，diff whitespace 检查通过。

本机验证限定 GCC14、C++23 头文件接口、Linux epoll、共享库；未验证 Bazel、
mcpp/原生命名模块、其他编译器/平台、生产 CA 配置或性能提升。Boost.Asio
对照 benchmark 因本机无 Boost 头文件未生成，未把它算作全目标已编译。
sanitizer 仅覆盖上述30个受影响行为测试；全仓的36项原有 skips 和5项
disabled 仍是未覆盖项。h2c 使用现有 prior knowledge，不新增 Upgrade；
浏览器不能直连 h2c。自定义连接 handler 的任务/升级生命周期由原生调用方
负责；typed handler 必须最终返回，停止等待但不会任意抢占。

以上验收完成时尚未 commit/tag/push；`v6.2.0` annotated ref
`4d46496e0c93f4cc0b4f8948814af2ab7bcec288` / peeled commit
`178c8b391f43e50b2f6967e78ffb0e14f30f9db0` 不变。原有用户服务PID503632 / 18080
保持运行，验收 fixture 使用独立私有端口并已清理；serde submodule 无改动。

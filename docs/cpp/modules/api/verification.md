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

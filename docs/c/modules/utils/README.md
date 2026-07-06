# utils C 文档

## 当前基线

- 源码位置：`src/c/galay-utils-c`
- CMake target：`galay-c-utils`，alias 为 `galay::c-utils`
- 依赖：`galay::c-common` 与 `galay::utils`
- 主要职责：提供 utils 模块的 C ABI，包括字节数组、单线程环形缓冲区和常用编码/摘要函数。

## 公开头文件

- `src/c/galay-utils-c/utils_c.h`

## 核心 API

- `galay_utils_get_error()`：把 utils/common 状态码转换为静态只读错误字符串。
- `galay_utils_bytes_create()` / `galay_utils_bytes_destroy()`：创建与销毁不可变字节数组。
- `galay_utils_bytes_data()` / `galay_utils_bytes_size()` / `galay_utils_bytes_capacity()`：读取字节数组内部只读视图和容量信息。
- `galay_utils_ring_buffer_create()` / `galay_utils_ring_buffer_destroy()`：创建与销毁固定容量环形缓冲区。
- `galay_utils_ring_buffer_write()` / `galay_utils_ring_buffer_read()`：完整写入或完整读取环形缓冲区。
- `galay_utils_base64_encode()` / `galay_utils_base64_decode()`：Base64 编解码。

## 使用约束

- 除 create 返回的 opaque handle 外，输入和输出缓冲区均由调用方拥有。
- 环形缓冲区不提供内部同步；多个线程并发访问时调用方必须自行串行化。
- read/write 不做部分成功：容量不足或可读数据不足时返回错误，`actual` 保持 0。
- Base64 API 不追加字符串终止符；需要 C 字符串时调用方需额外预留并自行写入 `\0`。


# common C 文档

## 当前基线

- 源码位置：`src/c/galay-common-c`
- CMake target：`galay-c-common`，alias 为 `galay::c-common`
- 主要职责：提供 C ABI 共享类型、版本号、通用错误码和 scatter/gather buffer 描述。

## 公开头文件

- `src/c/galay-common-c/common/galay_c_defs.h`
- `src/c/galay-common-c/common/galay_c_error.h`
- `src/c/galay-common-c/common/galay_c_iovec.h`

## 核心 API

- `GALAY_C_API`：编译期可用性标记。
- `GALAY_C_VERSION_MAJOR` / `GALAY_C_VERSION_MINOR` / `GALAY_C_VERSION_PATCH`：C ABI 头文件版本号。
- `galay_c_version_major()` / `galay_c_version_minor()` / `galay_c_version_patch()`：当前链接库 C ABI 版本。
- `galay_status_t`：通用状态码，覆盖参数错误、未找到、内存不足、协议错误、I/O 错误、EOF、超时与取消。
- `galay_status_string()` / `galay_c_common_get_error()`：把状态码转换为静态只读错误字符串。
- `galay_iovec_t`：跨 C/C++ ABI 的 scatter/gather buffer 描述。

## 使用约束

- C API 通过显式状态码或结果结构返回错误，不让 C++ 异常穿过 ABI 边界。
- 错误字符串由库拥有，调用方不得释放或写入。
- `galay_iovec_t::base` 是调用方借用缓冲区，对应 API 返回前必须保持有效。


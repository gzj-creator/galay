/**
 * @file macro.h
 * @brief galay-utils C ABI 编译期状态宏
 */

#ifndef GALAY_C_UTILS_MACRO_H
#define GALAY_C_UTILS_MACRO_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

/** @brief utils 操作成功状态别名。 */
#ifndef GALAY_UTILS_OK
#define GALAY_UTILS_OK GALAY_OK
#endif
/** @brief utils 参数非法状态别名。 */
#ifndef GALAY_UTILS_INVALID_ARGUMENT
#define GALAY_UTILS_INVALID_ARGUMENT GALAY_INVALID_ARGUMENT
#endif
/** @brief utils 输出缓冲区不足状态别名。 */
#ifndef GALAY_UTILS_BUFFER_TOO_SMALL
#define GALAY_UTILS_BUFFER_TOO_SMALL GALAY_OUT_OF_MEMORY
#endif

#endif  /* GALAY_C_UTILS_MACRO_H */

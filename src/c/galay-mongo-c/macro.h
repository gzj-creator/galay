/**
 * @file macro.h
 * @brief galay-mongo C ABI 编译期边界宏
 */

#ifndef GALAY_C_MONGO_MACRO_H
#define GALAY_C_MONGO_MACRO_H

/** @brief C ABI 接受的 BSON key 最大字节数。 */
#ifndef GALAY_MONGO_MAX_KEY_LENGTH
#define GALAY_MONGO_MAX_KEY_LENGTH 255u
#endif
/** @brief C ABI 接受的 BSON 字符串最大字节数。 */
#ifndef GALAY_MONGO_MAX_STRING_LENGTH
#define GALAY_MONGO_MAX_STRING_LENGTH 4096u
#endif

#endif  /* GALAY_C_MONGO_MACRO_H */

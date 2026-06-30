#ifndef GALAY_C_MONGO_MONGO_H
#define GALAY_C_MONGO_MONGO_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GALAY_MONGO_MAX_KEY_LENGTH 255u      ///< C ABI 接受的 BSON key 最大字节数。
#define GALAY_MONGO_MAX_STRING_LENGTH 4096u  ///< C ABI 接受的字符串最大字节数。

/**
 * @brief Mongo BSON document opaque handle。
 * @details document 拥有 BSON 字段和值；append document/array 时按值复制输入内容。
 *          encode 结果缓存于 document 内部，下一次修改、encode 或 destroy 后失效。
 * @note 非线程安全；同一 document 不应被多个线程或 C coroutine 并发修改/读取。
 */
typedef struct galay_mongo_document_t galay_mongo_document_t;

/**
 * @brief Mongo BSON array opaque handle。
 * @details array 拥有 BSON 元素；append document/array 时按值复制输入内容。
 * @note get_document/get_array 返回新建 handle，调用方负责销毁。
 */
typedef struct galay_mongo_array_t galay_mongo_array_t;

/**
 * @brief Mongo URI opaque handle。
 * @details 当前 C ABI 解析 `mongodb://host[:port]/database` 形式，并保存 host、port、database。
 * @note host/database getter 返回借用指针，生命周期到 uri destroy 为止。
 */
typedef struct galay_mongo_uri_t galay_mongo_uri_t;

/**
 * @brief Mongo client opaque handle。
 * @details client 独占 TCP socket、默认 endpoint/database 和 receive buffer；async API 必须在
 *          galay C coroutine/runtime 中串行调用。
 * @note 非线程安全；close/destroy 前调用方必须保证没有挂起的 connect/command/hello/close。
 */
typedef struct galay_mongo_client_t galay_mongo_client_t;

/**
 * @brief 将 Mongo C ABI 状态码转换为静态错误字符串。
 * @param status `galay_status_t` 状态码。
 * @return 指向静态字符串的指针，调用方不得释放。
 * @note async API 的 `C_IOResult.value` 可能保存 `galay_status_t`，可用本函数解释。
 */
const char* galay_mongo_get_error(galay_status_t status);

/**
 * @brief 创建空 BSON document。
 * @param out 成功时返回 document 所有权，调用方用 `galay_mongo_document_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数非法或分配失败通过 `galay_status_t` 返回。
 */
galay_status_t galay_mongo_document_create(galay_mongo_document_t** out);

/**
 * @brief 销毁 BSON document。
 * @param document 可为 NULL。
 * @note 销毁后 encode/get_string/get_binary/get_object_id 返回的借用指针失效。
 */
void galay_mongo_document_destroy(galay_mongo_document_t* document);

/**
 * @brief 获取 document 字段数量。
 * @param document BSON document；NULL 返回 0。
 * @return 字段数量。
 */
size_t galay_mongo_document_size(const galay_mongo_document_t* document);

/**
 * @brief 向 document 追加 int32 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用，必须非空且不超过 `GALAY_MONGO_MAX_KEY_LENGTH`。
 * @param value int32 值。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_append_int32(galay_mongo_document_t* document, const char* key, int32_t value);

/**
 * @brief 向 document 追加 int64 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value int64 值。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_append_int64(galay_mongo_document_t* document, const char* key, int64_t value);

/**
 * @brief 向 document 追加 double 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value double 值。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_append_double(galay_mongo_document_t* document, const char* key, double value);

/**
 * @brief 向 document 追加 bool 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value `GALAY_TRUE` 写入 true，其它值写入 false。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_append_bool(galay_mongo_document_t* document, const char* key, galay_bool_t value);

/**
 * @brief 向 document 追加 string 字段。
 * @details 字符串按 `value_len` 复制，可包含 NUL；`value == NULL && value_len == 0` 表示空串。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value 字符串字节，调用期间借用。
 * @param value_len 字符串字节数，必须不超过 `GALAY_MONGO_MAX_STRING_LENGTH`。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_append_string(galay_mongo_document_t* document, const char* key, const char* value, size_t value_len);

/**
 * @brief 向 document 追加 null 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_append_null(galay_mongo_document_t* document, const char* key);

/**
 * @brief 向 document 追加嵌套 document 字段。
 * @details `value` 内容会按值复制，调用返回后原 document 可销毁。
 * @param document 目标 BSON document。
 * @param key 字段名，调用期间借用。
 * @param value 源 document。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_append_document(galay_mongo_document_t* document, const char* key, const galay_mongo_document_t* value);

/**
 * @brief 向 document 追加嵌套 array 字段。
 * @details `value` 内容会按值复制，调用返回后原 array 可销毁。
 * @param document 目标 BSON document。
 * @param key 字段名，调用期间借用。
 * @param value 源 array。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_append_array(galay_mongo_document_t* document, const char* key, const galay_mongo_array_t* value);

/**
 * @brief 向 document 追加 binary 字段。
 * @details binary 按值复制；`value == NULL && value_len == 0` 表示空 binary。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value binary 字节，调用期间借用。
 * @param value_len binary 字节数。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_append_binary(galay_mongo_document_t* document, const char* key, const uint8_t* value, size_t value_len);

/**
 * @brief 向 document 追加 ObjectId 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param object_id_hex 24 字节 hex ObjectId 字符串。
 * @return 成功返回 `GALAY_OK`；参数非法或 ObjectId 格式错误返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_append_object_id(galay_mongo_document_t* document, const char* key, const char* object_id_hex);

/**
 * @brief 向 document 追加 BSON DateTime 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param millis Unix epoch 毫秒。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_append_date_time(galay_mongo_document_t* document, const char* key, int64_t millis);

/**
 * @brief 向 document 追加 BSON Timestamp 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param timestamp BSON timestamp 原始 64 位值。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_append_timestamp(galay_mongo_document_t* document, const char* key, uint64_t timestamp);

/**
 * @brief 将 document 编码为 BSON buffer。
 * @param document BSON document。
 * @param bson 成功时返回借用 BSON 字节指针。
 * @param bson_len 成功时返回 BSON 字节数。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`，编码失败返回
 *         `GALAY_PROTOCOL_ERROR`。
 * @note 返回 buffer 缓存在 document 内，下一次修改、encode 或 destroy 后失效。
 */
galay_status_t galay_mongo_document_encode(galay_mongo_document_t* document, const uint8_t** bson, size_t* bson_len);

/**
 * @brief 从 BSON buffer 解码 document。
 * @param bson BSON 字节，调用期间借用。
 * @param bson_len BSON 字节数。
 * @param out 成功时返回新 document 所有权，调用方负责 destroy。
 * @return 成功返回 `GALAY_OK`；参数非法、协议错误或分配失败通过 `galay_status_t` 返回。
 */
galay_status_t galay_mongo_document_decode(const uint8_t* bson, size_t bson_len, galay_mongo_document_t** out);
/**
 * @brief 获取 document 中的 int32 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value 成功时返回 int32 值。
 * @return 成功返回 `GALAY_OK`；字段不存在返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回
 *         `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_get_int32(const galay_mongo_document_t* document, const char* key, int32_t* value);

/**
 * @brief 获取 document 中的 int64 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value 成功时返回 int64 值。
 * @return 成功返回 `GALAY_OK`；字段不存在返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回错误。
 */
galay_status_t galay_mongo_document_get_int64(const galay_mongo_document_t* document, const char* key, int64_t* value);

/**
 * @brief 获取 document 中的 double 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value 成功时返回 double 值。
 * @return 成功返回 `GALAY_OK`；字段不存在返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回错误。
 */
galay_status_t galay_mongo_document_get_double(const galay_mongo_document_t* document, const char* key, double* value);

/**
 * @brief 获取 document 中的 bool 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value 成功时返回 `GALAY_TRUE` 或 `GALAY_FALSE`。
 * @return 成功返回 `GALAY_OK`；字段不存在返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回错误。
 */
galay_status_t galay_mongo_document_get_bool(const galay_mongo_document_t* document, const char* key, galay_bool_t* value);

/**
 * @brief 获取 document 中的 string 字段借用视图。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value 成功时返回借用字符串指针。
 * @param value_len 成功时返回字符串字节数。
 * @return 成功返回 `GALAY_OK`；字段不存在返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回错误。
 * @note 字符串不保证 NUL 结尾，生命周期到 document 修改或 destroy 为止。
 */
galay_status_t galay_mongo_document_get_string(const galay_mongo_document_t* document, const char* key, const char** value, size_t* value_len);

/**
 * @brief 获取 document 中的嵌套 document。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param out 成功时返回新 document 所有权，调用方负责 destroy。
 * @return 成功返回 `GALAY_OK`；字段不存在返回 `GALAY_NOT_FOUND`；类型不匹配、参数非法或分配失败返回错误。
 * @note 返回的是按值复制的新 handle，不借用父 document。
 */
galay_status_t galay_mongo_document_get_document(const galay_mongo_document_t* document, const char* key, galay_mongo_document_t** out);

/**
 * @brief 获取 document 中的嵌套 array。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param out 成功时返回新 array 所有权，调用方负责 destroy。
 * @return 成功返回 `GALAY_OK`；字段不存在返回 `GALAY_NOT_FOUND`；类型不匹配、参数非法或分配失败返回错误。
 * @note 返回的是按值复制的新 handle，不借用父 document。
 */
galay_status_t galay_mongo_document_get_array(const galay_mongo_document_t* document, const char* key, galay_mongo_array_t** out);

/**
 * @brief 获取 document 中的 binary 字段借用视图。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value 成功时返回借用 binary 指针。
 * @param value_len 成功时返回 binary 字节数。
 * @return 成功返回 `GALAY_OK`；字段不存在返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回错误。
 * @note 指针生命周期到 document 修改或 destroy 为止。
 */
galay_status_t galay_mongo_document_get_binary(const galay_mongo_document_t* document, const char* key, const uint8_t** value, size_t* value_len);

/**
 * @brief 获取 document 中的 ObjectId 字符串借用视图。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value 成功时返回借用 ObjectId 文本指针。
 * @param value_len 成功时返回文本字节数。
 * @return 成功返回 `GALAY_OK`；字段不存在返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回错误。
 * @note 指针生命周期到 document 修改或 destroy 为止。
 */
galay_status_t galay_mongo_document_get_object_id(const galay_mongo_document_t* document, const char* key, const char** value, size_t* value_len);

/**
 * @brief 获取 document 中的 DateTime 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value 成功时返回 Unix epoch 毫秒。
 * @return 成功返回 `GALAY_OK`；字段不存在返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回错误。
 */
galay_status_t galay_mongo_document_get_date_time(const galay_mongo_document_t* document, const char* key, int64_t* value);

/**
 * @brief 获取 document 中的 Timestamp 字段。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @param value 成功时返回 BSON timestamp 原始 64 位值。
 * @return 成功返回 `GALAY_OK`；字段不存在返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回错误。
 */
galay_status_t galay_mongo_document_get_timestamp(const galay_mongo_document_t* document, const char* key, uint64_t* value);

/**
 * @brief 判断 document 中指定字段是否为 null。
 * @param document BSON document。
 * @param key 字段名，调用期间借用。
 * @return 字段存在且为 null 返回 `GALAY_OK`；字段不存在返回 `GALAY_NOT_FOUND`；字段非 null 返回
 *         `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_document_is_null(const galay_mongo_document_t* document, const char* key);

/**
 * @brief 创建空 BSON array。
 * @param out 成功时返回 array 所有权，调用方用 `galay_mongo_array_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数非法或分配失败通过 `galay_status_t` 返回。
 */
galay_status_t galay_mongo_array_create(galay_mongo_array_t** out);

/**
 * @brief 销毁 BSON array。
 * @param array 可为 NULL。
 * @note 销毁后 array getter 返回的借用字符串失效。
 */
void galay_mongo_array_destroy(galay_mongo_array_t* array);

/**
 * @brief 获取 array 元素数量。
 * @param array BSON array；NULL 返回 0。
 * @return 元素数量。
 */
size_t galay_mongo_array_size(const galay_mongo_array_t* array);

/**
 * @brief 向 array 追加 int32 元素。
 * @param array BSON array。
 * @param value int32 值。
 * @return 成功返回 `GALAY_OK`；array 为 NULL 返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_array_append_int32(galay_mongo_array_t* array, int32_t value);

/**
 * @brief 向 array 追加 int64 元素。
 * @param array BSON array。
 * @param value int64 值。
 * @return 成功返回 `GALAY_OK`；array 为 NULL 返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_array_append_int64(galay_mongo_array_t* array, int64_t value);

/**
 * @brief 向 array 追加 double 元素。
 * @param array BSON array。
 * @param value double 值。
 * @return 成功返回 `GALAY_OK`；array 为 NULL 返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_array_append_double(galay_mongo_array_t* array, double value);

/**
 * @brief 向 array 追加 bool 元素。
 * @param array BSON array。
 * @param value `GALAY_TRUE` 写入 true，其它值写入 false。
 * @return 成功返回 `GALAY_OK`；array 为 NULL 返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_array_append_bool(galay_mongo_array_t* array, galay_bool_t value);

/**
 * @brief 向 array 追加 string 元素。
 * @param array BSON array。
 * @param value 字符串字节，调用期间借用。
 * @param value_len 字符串字节数，必须不超过 `GALAY_MONGO_MAX_STRING_LENGTH`。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note 字符串按值复制，可包含 NUL。
 */
galay_status_t galay_mongo_array_append_string(galay_mongo_array_t* array, const char* value, size_t value_len);

/**
 * @brief 向 array 追加 null 元素。
 * @param array BSON array。
 * @return 成功返回 `GALAY_OK`；array 为 NULL 返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_array_append_null(galay_mongo_array_t* array);

/**
 * @brief 向 array 追加 document 元素。
 * @param array 目标 BSON array。
 * @param value 源 document。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note `value` 会按值复制，调用返回后可销毁。
 */
galay_status_t galay_mongo_array_append_document(galay_mongo_array_t* array, const galay_mongo_document_t* value);

/**
 * @brief 向 array 追加嵌套 array 元素。
 * @param array 目标 BSON array。
 * @param value 源 array。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note `value` 会按值复制，调用返回后可销毁。
 */
galay_status_t galay_mongo_array_append_array(galay_mongo_array_t* array, const galay_mongo_array_t* value);

/**
 * @brief 向 array 追加 binary 元素。
 * @param array BSON array。
 * @param value binary 字节，调用期间借用。
 * @param value_len binary 字节数。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note binary 按值复制。
 */
galay_status_t galay_mongo_array_append_binary(galay_mongo_array_t* array, const uint8_t* value, size_t value_len);

/**
 * @brief 向 array 追加 ObjectId 元素。
 * @param array BSON array。
 * @param object_id_hex 24 字节 hex ObjectId 字符串。
 * @return 成功返回 `GALAY_OK`；参数非法或 ObjectId 格式错误返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_array_append_object_id(galay_mongo_array_t* array, const char* object_id_hex);

/**
 * @brief 向 array 追加 DateTime 元素。
 * @param array BSON array。
 * @param millis Unix epoch 毫秒。
 * @return 成功返回 `GALAY_OK`；array 为 NULL 返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_array_append_date_time(galay_mongo_array_t* array, int64_t millis);

/**
 * @brief 向 array 追加 Timestamp 元素。
 * @param array BSON array。
 * @param timestamp BSON timestamp 原始 64 位值。
 * @return 成功返回 `GALAY_OK`；array 为 NULL 返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_array_append_timestamp(galay_mongo_array_t* array, uint64_t timestamp);
/**
 * @brief 获取 array 中的 int32 元素。
 * @param array BSON array。
 * @param index 元素索引。
 * @param value 成功时返回 int32 值。
 * @return 成功返回 `GALAY_OK`；越界返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回错误。
 */
galay_status_t galay_mongo_array_get_int32(const galay_mongo_array_t* array, size_t index, int32_t* value);

/**
 * @brief 获取 array 中的 int64 元素。
 * @param array BSON array。
 * @param index 元素索引。
 * @param value 成功时返回 int64 值。
 * @return 成功返回 `GALAY_OK`；越界返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回错误。
 */
galay_status_t galay_mongo_array_get_int64(const galay_mongo_array_t* array, size_t index, int64_t* value);

/**
 * @brief 获取 array 中的 double 元素。
 * @param array BSON array。
 * @param index 元素索引。
 * @param value 成功时返回 double 值。
 * @return 成功返回 `GALAY_OK`；越界返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回错误。
 */
galay_status_t galay_mongo_array_get_double(const galay_mongo_array_t* array, size_t index, double* value);

/**
 * @brief 获取 array 中的 bool 元素。
 * @param array BSON array。
 * @param index 元素索引。
 * @param value 成功时返回 `GALAY_TRUE` 或 `GALAY_FALSE`。
 * @return 成功返回 `GALAY_OK`；越界返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回错误。
 */
galay_status_t galay_mongo_array_get_bool(const galay_mongo_array_t* array, size_t index, galay_bool_t* value);

/**
 * @brief 获取 array 中的 string 元素借用视图。
 * @param array BSON array。
 * @param index 元素索引。
 * @param value 成功时返回借用字符串指针。
 * @param value_len 成功时返回字符串字节数。
 * @return 成功返回 `GALAY_OK`；越界返回 `GALAY_NOT_FOUND`；类型不匹配或参数非法返回错误。
 * @note 字符串不保证 NUL 结尾，生命周期到 array 修改或 destroy 为止。
 */
galay_status_t galay_mongo_array_get_string(const galay_mongo_array_t* array, size_t index, const char** value, size_t* value_len);

/**
 * @brief 获取 array 中的 document 元素。
 * @param array BSON array。
 * @param index 元素索引。
 * @param out 成功时返回新 document 所有权，调用方负责 destroy。
 * @return 成功返回 `GALAY_OK`；越界返回 `GALAY_NOT_FOUND`；类型不匹配、参数非法或分配失败返回错误。
 * @note 返回的是按值复制的新 handle。
 */
galay_status_t galay_mongo_array_get_document(const galay_mongo_array_t* array, size_t index, galay_mongo_document_t** out);

/**
 * @brief 获取 array 中的嵌套 array 元素。
 * @param array BSON array。
 * @param index 元素索引。
 * @param out 成功时返回新 array 所有权，调用方负责 destroy。
 * @return 成功返回 `GALAY_OK`；越界返回 `GALAY_NOT_FOUND`；类型不匹配、参数非法或分配失败返回错误。
 * @note 返回的是按值复制的新 handle。
 */
galay_status_t galay_mongo_array_get_array(const galay_mongo_array_t* array, size_t index, galay_mongo_array_t** out);

/**
 * @brief 解析 Mongo URI。
 * @details 当前支持 `mongodb://host[:port]/database`，忽略 query string。
 * @param uri_text URI 文本。
 * @param out 成功时返回 URI handle 所有权，调用方用 `galay_mongo_uri_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；格式非法返回 `GALAY_INVALID_ARGUMENT`，分配失败返回
 *         `GALAY_OUT_OF_MEMORY`。
 */
galay_status_t galay_mongo_uri_parse(const char* uri_text, galay_mongo_uri_t** out);

/**
 * @brief 销毁 Mongo URI handle。
 * @param uri 可为 NULL。
 * @note 销毁后 host/database getter 返回的借用指针失效。
 */
void galay_mongo_uri_destroy(galay_mongo_uri_t* uri);

/**
 * @brief 获取 URI host。
 * @param uri Mongo URI handle。
 * @param host 成功时返回借用 host 指针。
 * @param host_len 成功时返回 host 字节数。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note 指针生命周期到 uri destroy 为止，不保证 NUL 结尾。
 */
galay_status_t galay_mongo_uri_host(const galay_mongo_uri_t* uri, const char** host, size_t* host_len);

/**
 * @brief 获取 URI database。
 * @param uri Mongo URI handle。
 * @param database 成功时返回借用 database 指针。
 * @param database_len 成功时返回 database 字节数。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note 指针生命周期到 uri destroy 为止，不保证 NUL 结尾。
 */
galay_status_t galay_mongo_uri_database(const galay_mongo_uri_t* uri, const char** database, size_t* database_len);

/**
 * @brief 获取 URI port。
 * @param uri Mongo URI handle。
 * @param port 成功时返回 port。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mongo_uri_port(const galay_mongo_uri_t* uri, uint16_t* port);

/**
 * @brief 构造 Mongo findOne 命令 document。
 * @details 生成包含 `find`、`filter`、可选 `projection`、`limit: 1`、`singleBatch: true`
 *          和 `$db` 的命令 document。
 * @param database database 名称，调用期间借用。
 * @param collection collection 名称，不能为空。
 * @param filter 查询过滤 document。
 * @param projection 可选投影 document，可为 NULL。
 * @param out 成功时返回命令 document 所有权，调用方负责 destroy。
 * @return 成功返回 `GALAY_OK`；参数非法或分配失败通过 `galay_status_t` 返回。
 * @note 输入 document 会按值复制到命令中。
 */
galay_status_t galay_mongo_command_find_one(const char* database, const char* collection,
                                            const galay_mongo_document_t* filter,
                                            const galay_mongo_document_t* projection,
                                            galay_mongo_document_t** out);

/**
 * @brief 构造 Mongo insertOne 命令 document。
 * @param database database 名称，调用期间借用。
 * @param collection collection 名称，不能为空。
 * @param document 待插入 document，会按值复制。
 * @param out 成功时返回命令 document 所有权，调用方负责 destroy。
 * @return 成功返回 `GALAY_OK`；参数非法或分配失败通过 `galay_status_t` 返回。
 */
galay_status_t galay_mongo_command_insert_one(const char* database, const char* collection,
                                              const galay_mongo_document_t* document,
                                              galay_mongo_document_t** out);

/**
 * @brief 构造 Mongo updateOne 命令 document。
 * @param database database 名称，调用期间借用。
 * @param collection collection 名称，不能为空。
 * @param filter update filter document，会按值复制。
 * @param update update document，会按值复制。
 * @param upsert `GALAY_TRUE` 时设置 upsert。
 * @param out 成功时返回命令 document 所有权，调用方负责 destroy。
 * @return 成功返回 `GALAY_OK`；参数非法或分配失败通过 `galay_status_t` 返回。
 */
galay_status_t galay_mongo_command_update_one(const char* database, const char* collection,
                                              const galay_mongo_document_t* filter,
                                              const galay_mongo_document_t* update,
                                              galay_bool_t upsert,
                                              galay_mongo_document_t** out);

/**
 * @brief 构造 Mongo deleteOne 命令 document。
 * @param database database 名称，调用期间借用。
 * @param collection collection 名称，不能为空。
 * @param filter delete filter document，会按值复制。
 * @param out 成功时返回命令 document 所有权，调用方负责 destroy。
 * @return 成功返回 `GALAY_OK`；参数非法或分配失败通过 `galay_status_t` 返回。
 */
galay_status_t galay_mongo_command_delete_one(const char* database, const char* collection,
                                              const galay_mongo_document_t* filter,
                                              galay_mongo_document_t** out);

/**
 * @brief 创建 Mongo client handle。
 * @param out 成功时返回 client 所有权，调用方用 `galay_mongo_client_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数非法或分配失败通过 `galay_status_t` 返回。
 * @note 默认 endpoint 为 127.0.0.1:27017/admin；create 不连接网络。
 */
galay_status_t galay_mongo_client_create(galay_mongo_client_t** out);

/**
 * @brief 销毁 Mongo client handle。
 * @param client 可为 NULL。
 * @note 若 socket 仍存在会释放 socket；调用方必须保证没有挂起 async 操作。
 */
void galay_mongo_client_destroy(galay_mongo_client_t* client);

/**
 * @brief 同步释放 Mongo client 内部 socket。
 * @param client Mongo client，可为 NULL。
 * @note 本地资源关闭，不发送 MongoDB command，也不挂起 coroutine。
 */
void galay_mongo_client_close(galay_mongo_client_t* client);

/**
 * @brief 查询 Mongo client 是否处于 connected 状态。
 * @param client Mongo client，可为 NULL。
 * @return 已连接返回 `GALAY_TRUE`，否则返回 `GALAY_FALSE`。
 */
galay_bool_t galay_mongo_client_is_connected(const galay_mongo_client_t* client);

/**
 * @brief 同步 ping 接口占位。
 * @param client 已连接的 Mongo client。
 * @param database database 名称，调用期间借用。
 * @return 当前对有效参数返回 `GALAY_UNSUPPORTED`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note 使用 `galay_mongo_client_hello_async` 或 `galay_mongo_client_command_async` 执行真实 I/O。
 */
galay_status_t galay_mongo_client_ping(galay_mongo_client_t* client, const char* database);

/**
 * @brief 设置 Mongo client endpoint 和默认 database。
 * @param client Mongo client。
 * @param host host 字符串，调用期间借用并复制到 client。
 * @param port 非 0 port。
 * @param database 默认 database；NULL 或空字符串使用 `admin`。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note 应在 connect 前调用；当前函数不关闭已有连接。
 */
galay_status_t galay_mongo_client_set_endpoint(galay_mongo_client_t* client, const char* host, uint16_t port, const char* database);

/**
 * @brief 在当前 C coroutine 内连接 Mongo endpoint。
 * @details 创建并连接底层 TCP socket；成功后 `C_IOResult.ptr` 指向 client。
 * @param client Mongo client。
 * @param timeout_ms 负数无限等待，0 直接超时，正数为毫秒超时。
 * @return 成功返回 `C_IOResultOk`；参数、socket 或超时错误通过 `C_IOResult` 返回。
 * @note 会挂起当前 C coroutine，不阻塞线程；无独立 cancel API，可通过 timeout 或关闭 runtime/socket 终止等待。
 */
C_IOResult galay_mongo_client_connect_async(galay_mongo_client_t* client, int64_t timeout_ms);

/**
 * @brief 发送 Mongo hello command 并读取 reply。
 * @param client 已连接的 Mongo client。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param reply 成功时返回 reply document 所有权，调用方负责 destroy。
 * @return 成功返回 `C_IOResultOk`；网络、协议、server ok=false 或分配错误通过 `C_IOResult` 返回。
 * @note 使用 client 默认 database；会挂起当前 C coroutine。
 */
C_IOResult galay_mongo_client_hello_async(galay_mongo_client_t* client, int64_t timeout_ms, galay_mongo_document_t** reply);

/**
 * @brief 发送任意 Mongo command 并读取 reply。
 * @details command 会编码为 OP_MSG；reply 的 `response_to` 必须匹配本次 request id。
 * @param client 已连接的 Mongo client。
 * @param database database 名称；NULL 或空字符串时使用 client 默认 database。
 * @param command 命令 document，调用期间借用。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param reply 成功时返回 reply document 所有权，调用方负责 destroy。
 * @return 成功返回 `C_IOResultOk`；网络、协议、server ok=false 或分配错误通过 `C_IOResult` 返回。
 * @note 同一 client 不得并发发送多个 command；函数会挂起当前 C coroutine。
 */
C_IOResult galay_mongo_client_command_async(galay_mongo_client_t* client, const char* database,
                                            const galay_mongo_document_t* command,
                                            int64_t timeout_ms,
                                            galay_mongo_document_t** reply);

/**
 * @brief 在当前 C coroutine 内关闭 Mongo TCP 连接并释放 socket。
 * @details 调用 kernel TCP close awaitable 后销毁 socket，并清空 receive buffer。
 * @param client 已连接或持有 socket 的 Mongo client。
 * @param timeout_ms 关闭操作超时，语义同 kernel TCP close。
 * @return `C_IOResultOk` 表示关闭并清理成功；失败通过 `C_IOResult` 返回。
 * @note close 会挂起当前 C coroutine；成功或失败后 client 都会标记为未连接。
 */
C_IOResult galay_mongo_client_close_async(galay_mongo_client_t* client, int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif

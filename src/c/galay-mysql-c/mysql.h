#ifndef GALAY_C_MYSQL_MYSQL_H
#define GALAY_C_MYSQL_MYSQL_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MySQL client 配置 opaque handle。
 * @details 配置对象拥有 host、username、password、database、charset 等字符串副本；
 *          create 后默认 host 为 127.0.0.1，port 为 3306，username 为 root，
 *          charset 为 utf8mb4，connect timeout 为 3000ms。
 * @note 配置对象非线程安全；setter 与 getter 不应和连接流程并发调用。
 */
typedef struct galay_mysql_config_t galay_mysql_config_t;

/**
 * @brief MySQL 字节 buffer opaque handle。
 * @details 用于认证响应、COM_QUERY packet 和 query 返回 packet；buffer 拥有其字节存储。
 * @note `galay_mysql_buffer_data` 返回借用指针，生命周期到 buffer destroy 为止。
 */
typedef struct galay_mysql_buffer_t galay_mysql_buffer_t;

/**
 * @brief MySQL client opaque handle。
 * @details client 独占底层 TCP socket 和最近一次 handshake packet；async API 必须在
 *          galay C coroutine/runtime 中串行调用。
 * @note 非线程安全；close/destroy 前调用方必须保证没有挂起的 connect/auth/query/stmt/pool 操作。
 */
typedef struct galay_mysql_client_t galay_mysql_client_t;

/**
 * @brief MySQL result-set opaque handle。
 * @details 由 result decode、query_result、transaction、stmt execute 或 pipeline result 返回；
 *          对外暴露的 field/value view 均借用 result-set 内部存储。
 * @note 调用方拥有独立 result-set 时必须用 `galay_mysql_result_set_destroy` 释放；pipeline
 *       result 中的 item 由 pipeline result 拥有，不得单独释放。
 */
typedef struct galay_mysql_result_set_t galay_mysql_result_set_t;

/**
 * @brief MySQL prepared statement metadata opaque handle。
 * @details prepare 成功后保存 statement id、参数数量和列数量。
 * @note 当前 C ABI 没有单独 COM_STMT_CLOSE；destroy 只释放本地 metadata。
 */
typedef struct galay_mysql_stmt_t galay_mysql_stmt_t;

/**
 * @brief MySQL pipeline 查询缓存 opaque handle。
 * @details pipeline 拥有追加的 SQL 字符串副本；提交后按追加顺序发送并读取结果。
 * @note pipeline 非线程安全，不拥有提交返回的 result。
 */
typedef struct galay_mysql_pipeline_t galay_mysql_pipeline_t;

/**
 * @brief MySQL pipeline 结果集合 opaque handle。
 * @details 拥有每条 pipeline query 解码出的 result-set。
 * @note item accessor 返回借用 result-set 指针，生命周期到 pipeline result destroy 为止。
 */
typedef struct galay_mysql_pipeline_result_t galay_mysql_pipeline_result_t;

/**
 * @brief MySQL 连接池 opaque handle。
 * @details pool 拥有空闲 client；acquire 返回 lease 后 client 借给调用方，release 后归还空闲池。
 * @note 当前实现不提供锁保护和等待队列；不支持跨线程并发 acquire/release。
 */
typedef struct galay_mysql_pool_t galay_mysql_pool_t;

/**
 * @brief MySQL pool lease opaque handle。
 * @details lease 拥有一次借出的 client 引用，必须通过 `galay_mysql_pool_lease_release` 归还。
 * @note release 会销毁 lease handle；release 后不得继续访问 lease 或借用 client。
 */
typedef struct galay_mysql_pool_lease_t galay_mysql_pool_lease_t;

/**
 * @brief MySQL packet header view。
 * @details 对应 wire protocol 中 3 字节 payload length 与 1 字节 sequence id。
 */
typedef struct galay_mysql_packet_header_t {
    uint32_t payload_length; ///< payload 字节数，不含 4 字节 packet header。
    uint8_t sequence_id;     ///< MySQL packet sequence id。
} galay_mysql_packet_header_t;

/**
 * @brief MySQL packet 借用视图。
 * @details `payload` 指向输入 packet buffer 的 payload 起始位置；结构体不拥有数据。
 * @note 调用方必须保证原始 buffer 在使用 view 期间仍然存活。
 */
typedef struct galay_mysql_packet_view_t {
    const unsigned char* payload; ///< 借用 payload 指针。
    size_t payload_len;           ///< payload 字节数。
    uint8_t sequence_id;          ///< packet sequence id。
    size_t consumed;              ///< header + payload 总消费字节数。
} galay_mysql_packet_view_t;

/**
 * @brief MySQL column definition 借用视图。
 * @details 字符串字段指向 result-set 内部存储，不保证可跨 result-set 销毁后使用。
 * @note 该视图不拥有任何字符串；调用方需要长期保存时必须自行复制。
 */
typedef struct galay_mysql_field_view_t {
    const char* catalog;      ///< 借用 catalog 字符串。
    const char* schema;       ///< 借用 schema 字符串。
    const char* table;        ///< 借用 table 字符串。
    const char* org_table;    ///< 借用 original table 字符串。
    const char* name;         ///< 借用 column name 字符串。
    const char* org_name;     ///< 借用 original column name 字符串。
    uint16_t character_set;   ///< MySQL character set id。
    uint32_t column_length;   ///< column length。
    uint8_t column_type;      ///< MySQL column type。
    uint16_t flags;           ///< column flags。
    uint8_t decimals;         ///< decimals。
} galay_mysql_field_view_t;

/**
 * @brief MySQL row value 借用视图。
 * @details 非 NULL value 的 `data` 指向 result-set 内部 row 存储，可能包含 NUL 字节。
 * @note `is_null == GALAY_TRUE` 时 `data == NULL` 且 `data_len == 0`。
 */
typedef struct galay_mysql_value_view_t {
    const unsigned char* data; ///< 借用 value 字节指针。
    size_t data_len;           ///< value 字节数。
    galay_bool_t is_null;      ///< 是否为 SQL NULL。
} galay_mysql_value_view_t;

/**
 * @brief MySQL prepared statement 参数绑定。
 * @details execute 调用期间借用 `data`；非 NULL 参数按 length-encoded string 写入。
 * @note `bind_count` 必须等于 statement param count；`is_null == GALAY_TRUE` 时忽略 data。
 */
typedef struct galay_mysql_stmt_bind_t {
    const unsigned char* data; ///< 参数字节，调用期间借用。
    size_t data_len;           ///< 参数字节数。
    galay_bool_t is_null;      ///< 是否绑定 SQL NULL。
    uint8_t column_type;       ///< MySQL column type。
} galay_mysql_stmt_bind_t;

/**
 * @brief 将 MySQL C ABI 状态码转换为静态错误字符串。
 * @param status `galay_status_t` 状态码。
 * @return 指向静态字符串的指针，调用方不得释放。
 * @note async API 的 `C_IOResult.value` 可能保存 `galay_status_t`，可用本函数解释。
 */
const char* galay_mysql_get_error(galay_status_t status);

/**
 * @brief 创建 MySQL 配置对象。
 * @param out 成功时返回 config 所有权，调用方用 `galay_mysql_config_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数非法或分配失败通过 `galay_status_t` 返回。
 */
galay_status_t galay_mysql_config_create(galay_mysql_config_t** out);

/**
 * @brief 销毁 MySQL 配置对象。
 * @param config 可为 NULL。
 * @note 销毁后 getter 返回的所有借用字符串失效。
 */
void galay_mysql_config_destroy(galay_mysql_config_t* config);

/**
 * @brief 获取配置中的 host。
 * @param config MySQL 配置对象。
 * @param host 成功时返回借用 host 字符串。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note 指针生命周期到 config destroy 或下一次 set_host 为止。
 */
galay_status_t galay_mysql_config_host(const galay_mysql_config_t* config, const char** host);

/**
 * @brief 获取配置中的 port。
 * @param config MySQL 配置对象。
 * @param port 成功时返回 port。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_config_port(const galay_mysql_config_t* config, uint16_t* port);

/**
 * @brief 设置 MySQL host。
 * @param config MySQL 配置对象。
 * @param host 非空 host 字符串，调用期间借用并复制到 config。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_config_set_host(galay_mysql_config_t* config, const char* host);

/**
 * @brief 设置 MySQL port。
 * @param config MySQL 配置对象。
 * @param port 非 0 port。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_config_set_port(galay_mysql_config_t* config, uint16_t port);

/**
 * @brief 设置 MySQL username。
 * @param config MySQL 配置对象。
 * @param username 用户名，调用期间借用并复制到 config。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_config_set_username(galay_mysql_config_t* config, const char* username);

/**
 * @brief 设置 MySQL password。
 * @param config MySQL 配置对象。
 * @param password 密码，调用期间借用并复制到 config。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_config_set_password(galay_mysql_config_t* config, const char* password);

/**
 * @brief 设置默认 database。
 * @param config MySQL 配置对象。
 * @param database database 名称，调用期间借用并复制到 config。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_config_set_database(galay_mysql_config_t* config, const char* database);

/**
 * @brief 设置连接字符集名称。
 * @param config MySQL 配置对象。
 * @param charset 非空 charset 名称，调用期间借用并复制到 config。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_config_set_charset(galay_mysql_config_t* config, const char* charset);

/**
 * @brief 设置默认连接超时。
 * @param config MySQL 配置对象。
 * @param timeout_ms 正数毫秒超时。
 * @return 成功返回 `GALAY_OK`；0 或参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note async connect 传入负数 timeout 时使用该配置值。
 */
galay_status_t galay_mysql_config_set_connect_timeout_ms(galay_mysql_config_t* config, uint32_t timeout_ms);

/**
 * @brief 校验 MySQL 配置是否可用于连接。
 * @param config MySQL 配置对象。
 * @return host 非空且 port 非 0 时返回 `GALAY_OK`，否则返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_config_validate(const galay_mysql_config_t* config);

/**
 * @brief 生成 MySQL 认证插件响应 payload。
 * @details 支持 `mysql_native_password` 与 `caching_sha2_password` 的 fast auth 响应。
 * @param plugin 认证插件名。
 * @param password 密码字符串。
 * @param salt server handshake salt，调用期间借用。
 * @param salt_len salt 字节数，必须大于 0。
 * @param out 成功时返回 buffer 所有权，调用方用 `galay_mysql_buffer_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；插件不支持返回 `GALAY_UNSUPPORTED`；参数非法或分配失败返回错误。
 * @note 返回 buffer 只包含认证响应 payload，不包含 MySQL packet header。
 */
galay_status_t galay_mysql_auth_response_for_plugin(const char* plugin, const char* password,
                                                    const unsigned char* salt, size_t salt_len,
                                                    galay_mysql_buffer_t** out);

/**
 * @brief 销毁 MySQL buffer。
 * @param buffer 可为 NULL。
 * @note 销毁后 `galay_mysql_buffer_data` 返回的借用指针失效。
 */
void galay_mysql_buffer_destroy(galay_mysql_buffer_t* buffer);

/**
 * @brief 获取 MySQL buffer 的字节视图。
 * @param buffer MySQL buffer。
 * @param data 成功时返回借用字节指针。
 * @param data_len 成功时返回字节数。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note 指针生命周期到 buffer destroy 为止；调用方不得写入或释放该指针。
 */
galay_status_t galay_mysql_buffer_data(const galay_mysql_buffer_t* buffer,
                                       const unsigned char** data, size_t* data_len);

/**
 * @brief 解析 MySQL packet header。
 * @param data 至少 4 字节的 packet buffer，调用期间借用。
 * @param data_len buffer 字节数。
 * @param header 输出 packet header。
 * @return 成功返回 `GALAY_OK`；buffer 不足返回 `GALAY_PROTOCOL_ERROR`，参数非法返回
 *         `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_parse_packet_header(const unsigned char* data, size_t data_len,
                                               galay_mysql_packet_header_t* header);

/**
 * @brief 从连续 buffer 中提取一个完整 MySQL packet view。
 * @param data packet buffer，调用期间借用。
 * @param data_len buffer 字节数。
 * @param view 成功时返回 payload 借用视图和消费长度。
 * @return 成功返回 `GALAY_OK`；截断 packet 返回 `GALAY_PROTOCOL_ERROR`，参数非法返回
 *         `GALAY_INVALID_ARGUMENT`。
 * @note `view->payload` 指向 `data` 内部，不拥有内存。
 */
galay_status_t galay_mysql_extract_packet(const unsigned char* data, size_t data_len,
                                          galay_mysql_packet_view_t* view);

/**
 * @brief 编码 COM_QUERY packet。
 * @param query SQL 文本，不能为 NULL。
 * @param sequence_id MySQL packet sequence id。
 * @param out 成功时返回 packet buffer 所有权，调用方用 `galay_mysql_buffer_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数非法或分配失败通过 `galay_status_t` 返回。
 * @note buffer 包含 4 字节 MySQL packet header 和 COM_QUERY payload。
 */
galay_status_t galay_mysql_encode_query_packet(const char* query, uint8_t sequence_id,
                                               galay_mysql_buffer_t** out);

/**
 * @brief 解码一段连续 MySQL response packet 为 C result-set 对象。
 * @details 支持 OK packet 和文本结果集 packet 序列；ERR packet 或不完整 packet 返回协议错误。
 * @param data 连续完整 packet buffer，每个 packet 包含 4 字节 MySQL packet header。
 * @param data_len buffer 字节数。
 * @param out 成功时获得 result-set 所有权，调用方必须用 `galay_mysql_result_set_destroy` 释放。
 * @return `GALAY_OK` 表示解码成功；截断、非法长度或不支持的响应返回 `GALAY_PROTOCOL_ERROR`。
 * @note 返回的 field/value view 均借用 result-set 内部存储，仅在 result-set 销毁前有效。
 */
galay_status_t galay_mysql_result_set_decode(const unsigned char* data, size_t data_len,
                                             galay_mysql_result_set_t** out);

/**
 * @brief 销毁 MySQL result-set。
 * @param result 可为 NULL。
 * @note 销毁后所有 field/value view 失效；pipeline result 中借用的 item 不得传入本函数。
 */
void galay_mysql_result_set_destroy(galay_mysql_result_set_t* result);

/**
 * @brief 获取 result-set 字段数量。
 * @param result MySQL result-set。
 * @param count 成功时返回字段数。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_result_set_field_count(const galay_mysql_result_set_t* result,
                                                  size_t* count);

/**
 * @brief 获取 result-set 行数量。
 * @param result MySQL result-set。
 * @param count 成功时返回行数。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_result_set_row_count(const galay_mysql_result_set_t* result,
                                                size_t* count);

/**
 * @brief 获取指定字段的 metadata view。
 * @param result MySQL result-set。
 * @param index 字段索引。
 * @param field 成功时写入字段借用视图。
 * @return 成功返回 `GALAY_OK`；越界返回 `GALAY_NOT_FOUND`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note `field` 内字符串借用 result-set 内部存储。
 */
galay_status_t galay_mysql_result_set_field(const galay_mysql_result_set_t* result,
                                            size_t index,
                                            galay_mysql_field_view_t* field);

/**
 * @brief 按字段名查找字段索引。
 * @param result MySQL result-set。
 * @param name 字段名，调用期间借用。
 * @param index 成功时返回字段索引。
 * @return 找到返回 `GALAY_OK`；未找到返回 `GALAY_NOT_FOUND`；参数非法返回
 *         `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_result_set_find_field(const galay_mysql_result_set_t* result,
                                                 const char* name,
                                                 size_t* index);

/**
 * @brief 获取指定单元格值。
 * @param result MySQL result-set。
 * @param row 行索引。
 * @param column 列索引。
 * @param value 成功时写入值借用视图。
 * @return 成功返回 `GALAY_OK`；行列越界返回 `GALAY_NOT_FOUND`；参数非法返回
 *         `GALAY_INVALID_ARGUMENT`。
 * @note 非 NULL value 的 data 可包含 NUL 字节，不保证 NUL 结尾。
 */
galay_status_t galay_mysql_result_set_value(const galay_mysql_result_set_t* result,
                                            size_t row,
                                            size_t column,
                                            galay_mysql_value_view_t* value);

/**
 * @brief 获取 OK packet/result-set 的 affected rows。
 * @param result MySQL result-set。
 * @param affected_rows 成功时返回 affected rows。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_result_set_affected_rows(const galay_mysql_result_set_t* result,
                                                    uint64_t* affected_rows);

/**
 * @brief 获取 OK packet/result-set 的 last insert id。
 * @param result MySQL result-set。
 * @param last_insert_id 成功时返回 last insert id。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_result_set_last_insert_id(const galay_mysql_result_set_t* result,
                                                     uint64_t* last_insert_id);

/**
 * @brief 获取 result-set status flags。
 * @param result MySQL result-set。
 * @param status_flags 成功时返回 MySQL status flags。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_result_set_status_flags(const galay_mysql_result_set_t* result,
                                                   uint16_t* status_flags);

/**
 * @brief 获取 result-set warnings 数量。
 * @param result MySQL result-set。
 * @param warnings 成功时返回 warning count。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_result_set_warnings(const galay_mysql_result_set_t* result,
                                               uint16_t* warnings);

/**
 * @brief 创建 MySQL client handle。
 * @param out 成功时返回 client 所有权，调用方用 `galay_mysql_client_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数非法或分配失败通过 `galay_status_t` 返回。
 * @note create 不连接网络；client 非线程安全。
 */
galay_status_t galay_mysql_client_create(galay_mysql_client_t** out);

/**
 * @brief 销毁 MySQL client handle。
 * @details 若内部 socket 仍存在，会先释放 socket 资源。
 * @param client 可为 NULL。
 * @note 不会等待挂起 I/O；销毁前必须确保没有 active async 操作。
 */
void galay_mysql_client_destroy(galay_mysql_client_t* client);

/**
 * @brief 同步释放 client 内部 socket 并清理握手状态。
 * @param client MySQL client，可为 NULL。
 * @note 这是本地资源关闭，不发送 MySQL quit packet，也不挂起 coroutine。
 */
void galay_mysql_client_close(galay_mysql_client_t* client);

/**
 * @brief 查询 client 是否处于 connected 状态。
 * @param client MySQL client。
 * @param connected 成功时返回 `GALAY_TRUE` 或 `GALAY_FALSE`。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_client_is_connected(const galay_mysql_client_t* client,
                                               galay_bool_t* connected);

/**
 * @brief 同步连接接口占位。
 * @details 当前实现不提供同步 MySQL 连接。
 * @param client MySQL client。
 * @param config MySQL 配置。
 * @return 有效参数返回 `GALAY_UNSUPPORTED`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 * @note 使用 `galay_mysql_client_connect_async` 执行真实连接。
 */
galay_status_t galay_mysql_client_connect(galay_mysql_client_t* client,
                                          const galay_mysql_config_t* config);

/**
 * @brief 在当前 C coroutine 内异步连接 MySQL mock/standalone endpoint 并读取 server handshake packet。
 * @details 创建 TCP socket、连接 endpoint，并读取一包 server handshake 保存到 client。
 * @param client 由 `galay_mysql_client_create` 创建的 client。
 * @param config 连接配置；host/port 必须有效。
 * @param timeout_ms 负数时使用 config 的 connect timeout，0 直接超时，正数为毫秒超时。
 * @return `C_IOResultOk` 表示 TCP 连接成功且读取到一包 handshake；错误通过返回值传播。
 * @note 该函数挂起当前 C coroutine，不阻塞线程；认证需继续调用 authenticate 或 connect_auth。
 */
C_IOResult galay_mysql_client_connect_async(galay_mysql_client_t* client,
                                            const galay_mysql_config_t* config,
                                            int64_t timeout_ms);

/**
 * @brief 使用最近一次 handshake packet 发送 MySQL 认证响应并读取认证结果。
 * @details 支持 `mysql_native_password` 和 `caching_sha2_password` fast auth；在启用 SSL/RSA
 *          支持时可处理 RSA public-key full auth。
 * @param client 已通过 `galay_mysql_client_connect_async` 建立 TCP 并读取 handshake 的 client。
 * @param config 用户名、密码、database 等认证配置。
 * @param timeout_ms socket I/O 超时。
 * @return `C_IOResultOk` 表示认证 OK packet 已收到；不支持的插件、ERR packet 或协议错误返回错误。
 * @note C ABI 支持 `mysql_native_password` 和 `caching_sha2_password`，后者覆盖 fast auth
 *       与 RSA public-key full auth；full auth 需要构建时启用 SSL/RSA 支持。
 */
C_IOResult galay_mysql_client_authenticate_async(galay_mysql_client_t* client,
                                                 const galay_mysql_config_t* config,
                                                 int64_t timeout_ms);

/**
 * @brief 在当前 C coroutine 内完成连接并认证。
 * @details 先调用 connect_async，再调用 authenticate_async；认证失败时会尝试 close_async 清理 socket。
 * @param client MySQL client。
 * @param config MySQL 配置。
 * @param timeout_ms 每次连接/认证/关闭 I/O 的毫秒超时；负数连接阶段使用 config 默认超时。
 * @return 成功返回 `C_IOResultOk`；连接、认证或失败清理错误通过 `C_IOResult` 返回。
 * @note 会挂起当前 C coroutine；若认证失败且 close 也失败，返回 close 的错误。
 */
C_IOResult galay_mysql_client_connect_auth_async(galay_mysql_client_t* client,
                                                 const galay_mysql_config_t* config,
                                                 int64_t timeout_ms);

/**
 * @brief 在当前 C coroutine 内发送 COM_QUERY 并读取一个 result/error packet。
 * @details 返回原始单包 MySQL buffer，不解码多包 result-set。
 * @param client 已通过 `galay_mysql_client_connect_async` 连接的 client。
 * @param query SQL 文本，不能为 NULL。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param result_packet 成功时返回完整 MySQL packet buffer，调用方用
 *        `galay_mysql_buffer_destroy` 释放。
 * @return `C_IOResultOk` 表示 query packet 写入且读取到一包结果；错误通过返回值传播。
 * @note 会挂起当前 C coroutine；需要解码完整结果集时使用 `galay_mysql_client_query_result_async`。
 */
C_IOResult galay_mysql_client_query_async(galay_mysql_client_t* client,
                                          const char* query,
                                          int64_t timeout_ms,
                                          galay_mysql_buffer_t** result_packet);

/**
 * @brief 在当前 C coroutine 内发送 COM_QUERY 并读取/解码完整 result-set。
 * @param client 已连接的 MySQL client。
 * @param query SQL 文本，不能为 NULL。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param result 成功时返回 result-set 所有权，调用方用 `galay_mysql_result_set_destroy` 释放。
 * @return 成功返回 `C_IOResultOk`；网络、协议、参数和分配错误通过 `C_IOResult` 返回。
 * @note 同一 client 不得并发发起多个 query；函数会挂起当前 C coroutine。
 */
C_IOResult galay_mysql_client_query_result_async(galay_mysql_client_t* client,
                                                 const char* query,
                                                 int64_t timeout_ms,
                                                 galay_mysql_result_set_t** result);

/**
 * @brief 发送 `START TRANSACTION` 并解码结果。
 * @param client 已连接的 MySQL client。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param result 成功时返回 result-set 所有权，调用方负责 destroy。
 * @return 成功返回 `C_IOResultOk`；错误通过 `C_IOResult` 返回。
 */
C_IOResult galay_mysql_client_begin_transaction_async(galay_mysql_client_t* client,
                                                      int64_t timeout_ms,
                                                      galay_mysql_result_set_t** result);

/**
 * @brief 发送 `COMMIT` 并解码结果。
 * @param client 已连接的 MySQL client。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param result 成功时返回 result-set 所有权，调用方负责 destroy。
 * @return 成功返回 `C_IOResultOk`；错误通过 `C_IOResult` 返回。
 */
C_IOResult galay_mysql_client_commit_async(galay_mysql_client_t* client,
                                           int64_t timeout_ms,
                                           galay_mysql_result_set_t** result);

/**
 * @brief 发送 `ROLLBACK` 并解码结果。
 * @param client 已连接的 MySQL client。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param result 成功时返回 result-set 所有权，调用方负责 destroy。
 * @return 成功返回 `C_IOResultOk`；错误通过 `C_IOResult` 返回。
 */
C_IOResult galay_mysql_client_rollback_async(galay_mysql_client_t* client,
                                             int64_t timeout_ms,
                                             galay_mysql_result_set_t** result);

/**
 * @brief 在当前 C coroutine 内发送 COM_STMT_PREPARE 并读取 statement metadata。
 * @param client 已连接的 MySQL client。
 * @param sql SQL prepare 文本，不能为 NULL。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param stmt 成功时返回 statement metadata 所有权，调用方用 `galay_mysql_stmt_destroy` 释放。
 * @return 成功返回 `C_IOResultOk`；参数、网络、ERR packet 或协议错误通过 `C_IOResult` 返回。
 * @note 函数会继续读取参数/列 metadata 包；返回的 stmt 只表示本地 metadata。
 */
C_IOResult galay_mysql_client_stmt_prepare_async(galay_mysql_client_t* client,
                                                 const char* sql,
                                                 int64_t timeout_ms,
                                                 galay_mysql_stmt_t** stmt);

/**
 * @brief 销毁 prepared statement metadata。
 * @param stmt 可为 NULL。
 * @note 当前不会向 server 发送 COM_STMT_CLOSE；若需要 server-side close，应由调用方另行处理。
 */
void galay_mysql_stmt_destroy(galay_mysql_stmt_t* stmt);

/**
 * @brief 获取 statement id。
 * @param stmt prepared statement metadata。
 * @param statement_id 成功时返回 server 分配的 statement id。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_stmt_id(const galay_mysql_stmt_t* stmt, uint32_t* statement_id);

/**
 * @brief 获取 prepared statement 参数数量。
 * @param stmt prepared statement metadata。
 * @param count 成功时返回参数数量。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_stmt_param_count(const galay_mysql_stmt_t* stmt, size_t* count);

/**
 * @brief 获取 prepared statement 结果列数量。
 * @param stmt prepared statement metadata。
 * @param count 成功时返回列数量。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_stmt_column_count(const galay_mysql_stmt_t* stmt, size_t* count);

/**
 * @brief 在当前 C coroutine 内执行 prepared statement 并解码 result-set。
 * @details `bind_count` 必须等于 statement param count；每个 bind 的 data 只在调用期间借用。
 * @param client 已连接的 MySQL client。
 * @param stmt 由 `galay_mysql_client_stmt_prepare_async` 返回的 statement metadata。
 * @param binds 参数绑定数组；参数数量为 0 时可为 NULL。
 * @param bind_count 参数数量。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param result 成功时返回 result-set 所有权，调用方负责 destroy。
 * @return 成功返回 `C_IOResultOk`；参数、网络、协议或分配错误通过 `C_IOResult` 返回。
 * @note 会挂起当前 C coroutine；同一 client 不得并发执行多个 statement/query。
 */
C_IOResult galay_mysql_client_stmt_execute_async(galay_mysql_client_t* client,
                                                 const galay_mysql_stmt_t* stmt,
                                                 const galay_mysql_stmt_bind_t* binds,
                                                 size_t bind_count,
                                                 int64_t timeout_ms,
                                                 galay_mysql_result_set_t** result);

/**
 * @brief 创建 MySQL pipeline 查询缓存。
 * @param out 成功时返回 pipeline 所有权，调用方用 `galay_mysql_pipeline_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数非法或分配失败通过 `galay_status_t` 返回。
 * @note pipeline 只保存 SQL 副本，不访问网络。
 */
galay_status_t galay_mysql_pipeline_create(galay_mysql_pipeline_t** out);

/**
 * @brief 销毁 MySQL pipeline。
 * @param pipeline 可为 NULL。
 * @note 不会释放已返回的 pipeline result；result 必须单独 destroy。
 */
void galay_mysql_pipeline_destroy(galay_mysql_pipeline_t* pipeline);

/**
 * @brief 向 pipeline 追加一条 COM_QUERY SQL。
 * @param pipeline MySQL pipeline。
 * @param query SQL 文本，调用期间借用并复制到 pipeline。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_pipeline_append_query(galay_mysql_pipeline_t* pipeline,
                                                 const char* query);

/**
 * @brief 在当前 C coroutine 内提交 pipeline 并读取每条 query 的 result-set。
 * @details 按追加顺序发送全部 query，再按顺序读取并解码结果。
 * @param client 已连接的 MySQL client。
 * @param pipeline 非空 pipeline。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param result 成功时返回 pipeline result 所有权，调用方负责 destroy。
 * @return 成功返回 `C_IOResultOk`；pipeline 为空、网络、协议或分配错误通过 `C_IOResult` 返回。
 * @note 会挂起当前 C coroutine；失败时内部释放临时结果。
 */
C_IOResult galay_mysql_client_pipeline_async(galay_mysql_client_t* client,
                                             const galay_mysql_pipeline_t* pipeline,
                                             int64_t timeout_ms,
                                             galay_mysql_pipeline_result_t** result);

/**
 * @brief 销毁 MySQL pipeline result。
 * @param result 可为 NULL。
 * @note 销毁后通过 `galay_mysql_pipeline_result_at` 取得的 item 指针失效。
 */
void galay_mysql_pipeline_result_destroy(galay_mysql_pipeline_result_t* result);

/**
 * @brief 获取 pipeline result 中的 result-set 数量。
 * @param result MySQL pipeline result。
 * @param count 成功时返回数量。
 * @return 成功返回 `GALAY_OK`；参数非法返回 `GALAY_INVALID_ARGUMENT`。
 */
galay_status_t galay_mysql_pipeline_result_count(const galay_mysql_pipeline_result_t* result,
                                                 size_t* count);

/**
 * @brief 获取指定 pipeline result item。
 * @param result MySQL pipeline result。
 * @param index item 索引。
 * @param item 成功时返回借用 result-set 指针。
 * @return 成功返回 `GALAY_OK`；越界返回 `GALAY_NOT_FOUND`；参数非法返回
 *         `GALAY_INVALID_ARGUMENT`。
 * @note item 由 pipeline result 拥有，不得调用 `galay_mysql_result_set_destroy`。
 */
galay_status_t galay_mysql_pipeline_result_at(const galay_mysql_pipeline_result_t* result,
                                              size_t index,
                                              const galay_mysql_result_set_t** item);

/**
 * @brief 创建 MySQL 连接池。
 * @details pool 会复制 config；连接在 acquire 时创建并完成 connect_auth。
 * @param config 已 validate 的 MySQL 配置。
 * @param max_connections 最大连接数，必须大于 0。
 * @param out 成功时返回 pool 所有权，调用方用 `galay_mysql_pool_destroy` 释放。
 * @return 成功返回 `GALAY_OK`；参数非法或分配失败通过 `galay_status_t` 返回。
 * @note pool 不提供跨线程同步；销毁前必须先 release 所有 lease。
 */
galay_status_t galay_mysql_pool_create(const galay_mysql_config_t* config,
                                       size_t max_connections,
                                       galay_mysql_pool_t** out);

/**
 * @brief 销毁 MySQL 连接池及其空闲连接。
 * @param pool 可为 NULL。
 * @note 调用方必须先归还所有 lease；未归还 lease 中的 client 不在 idle 列表中，继续使用会产生悬空引用。
 */
void galay_mysql_pool_destroy(galay_mysql_pool_t* pool);

/**
 * @brief 在当前 C coroutine 内获取一个 MySQL pool lease。
 * @details 优先复用空闲连接；无空闲且未达到上限时创建 client 并执行 connect_auth。
 * @param pool MySQL pool。
 * @param timeout_ms connect/auth I/O 的毫秒超时。
 * @param lease 成功时返回 lease 所有权，调用方必须 release。
 * @return 成功返回 `C_IOResultOk`；达到连接上限当前返回 `C_IOResultError` 且 value 为
 *         `GALAY_UNSUPPORTED`；其他错误通过 `C_IOResult` 返回。
 * @note 不提供等待队列；会挂起当前 C coroutine。
 */
C_IOResult galay_mysql_pool_acquire_async(galay_mysql_pool_t* pool,
                                          int64_t timeout_ms,
                                          galay_mysql_pool_lease_t** lease);

/**
 * @brief 从 lease 获取借用 client。
 * @param lease MySQL pool lease。
 * @param client 成功时返回借用 client 指针。
 * @return 成功返回 `GALAY_OK`；lease 无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note client 生命周期到 lease release 为止；调用方不得 destroy 或 close 该 client。
 */
galay_status_t galay_mysql_pool_lease_client(galay_mysql_pool_lease_t* lease,
                                             galay_mysql_client_t** client);

/**
 * @brief 归还 MySQL pool lease。
 * @param lease 由 `galay_mysql_pool_acquire_async` 返回的 lease。
 * @return 成功返回 `GALAY_OK`；lease 无效返回 `GALAY_INVALID_ARGUMENT`。
 * @note 成功后 lease handle 被销毁，借用 client 回到 pool idle 列表。
 */
galay_status_t galay_mysql_pool_lease_release(galay_mysql_pool_lease_t* lease);

/**
 * @brief 在当前 C coroutine 内关闭 MySQL TCP 连接并释放内部 socket。
 * @details 调用 kernel TCP close awaitable 后销毁 socket，并清理 handshake/auth 状态。
 * @param client 已连接或持有 socket 的 client。
 * @param timeout_ms 关闭操作超时，语义同 kernel TCP close。
 * @return `C_IOResultOk` 表示关闭并清理成功；失败通过返回值传播。
 * @note close 会挂起当前 C coroutine；成功或失败后 client 都会标记为未连接/未认证。
 */
C_IOResult galay_mysql_client_close_async(galay_mysql_client_t* client, int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif

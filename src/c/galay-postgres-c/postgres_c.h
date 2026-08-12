#ifndef GALAY_C_POSTGRES_POSTGRES_C_H
#define GALAY_C_POSTGRES_POSTGRES_C_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-common-c/common/galay_c_defs.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#include <stddef.h>
#include <stdint.h>

/**
 * @file postgres_c.h
 * @brief PostgreSQL wire-protocol v3 client C ABI.
 *
 * @details Recoverable failures are returned as `galay_status_t` or
 * `C_IOResult`; no C++ exception crosses this ABI. APIs suffixed `_async`
 * suspend the current galay C coroutine and must be called serially per handle.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PostgreSQL connection configuration handle.
 * @details Owns copies of host, credentials, database and application name.
 * Defaults are host `127.0.0.1`, port 5432, 5000 ms connect timeout and
 * TCP_NODELAY enabled; username must be set before validation succeeds.
 * @note Not thread-safe; do not mutate it while another call is borrowing it.
 */
typedef struct galay_postgres_config_t galay_postgres_config_t;

/**
 * @brief Owning wire buffer returned by protocol encoders.
 * @note Data returned by `galay_postgres_buffer_data` is borrowed until destroy.
 */
typedef struct galay_postgres_buffer_t galay_postgres_buffer_t;

/**
 * @brief PostgreSQL client handle.
 * @details Owns one kernel TCP socket and protocol receive storage.
 * @note Not thread-safe; only one connect/query/close operation may be active,
 * and destroy requires every suspended operation to have completed.
 */
typedef struct galay_postgres_client_t galay_postgres_client_t;

/**
 * @brief Owning result-set handle.
 * @details Stores RowDescription metadata, DataRow values, command tag and the
 * ReadyForQuery transaction status.
 * @note Field and value views borrow storage until reset, query reuse, or
 * destroy. Items borrowed from a pipeline result must not be destroyed separately.
 */
typedef struct galay_postgres_result_set_t galay_postgres_result_set_t;

/**
 * @brief Prepared statement metadata handle.
 * @details Owns the statement name, parameter OIDs and result-column metadata.
 * @note Destroy only releases local metadata; it does not send Close to server.
 */
typedef struct galay_postgres_stmt_t galay_postgres_stmt_t;

/**
 * @brief Owning pipeline command buffer.
 * @note Not thread-safe; appended SQL is copied into the handle.
 */
typedef struct galay_postgres_pipeline_t galay_postgres_pipeline_t;

/**
 * @brief Owns result sets returned by one pipeline execution.
 * @note Items returned by `galay_postgres_pipeline_result_at` are borrowed.
 */
typedef struct galay_postgres_pipeline_result_t galay_postgres_pipeline_result_t;

/**
 * @brief PostgreSQL connection pool handle.
 * @details Owns its copied configuration and all idle client handles.
 * @note Not safe for concurrent access; all leases must be released before destroy.
 */
typedef struct galay_postgres_pool_t galay_postgres_pool_t;

/**
 * @brief Lease for one client borrowed from a PostgreSQL pool.
 * @note Release destroys the lease handle and invalidates its borrowed client pointer.
 */
typedef struct galay_postgres_pool_lease_t galay_postgres_pool_lease_t;

/** Borrowed PostgreSQL v3 message header. `length` includes its four bytes. */
typedef struct galay_postgres_message_header_t {
    char type;       ///< PostgreSQL message type byte.
    uint32_t length; ///< Length including this four-byte field, excluding type.
} galay_postgres_message_header_t;

/** Borrowed PostgreSQL v3 message view. */
typedef struct galay_postgres_message_view_t {
    char type;                    ///< PostgreSQL message type byte.
    const unsigned char* payload; ///< Borrowed payload pointer into input data.
    size_t payload_len;           ///< Payload byte count.
    size_t consumed;              ///< Total type + length + payload bytes consumed.
} galay_postgres_message_view_t;

/** Borrowed RowDescription field metadata. */
typedef struct galay_postgres_field_view_t {
    const char* name;       ///< Borrowed NUL-terminated column name.
    uint32_t table_oid;     ///< Source table OID, or 0 when not applicable.
    int16_t column_index;   ///< Source table column attribute number, or 0.
    uint32_t type_oid;      ///< PostgreSQL data type OID.
    int16_t type_size;      ///< Type size, or -1 for variable-width values.
    int32_t type_modifier;  ///< Type modifier supplied by the server.
    int16_t format;         ///< 0 for text, 1 for binary.
} galay_postgres_field_view_t;

/** Borrowed DataRow value. `data` is NULL only for SQL NULL. */
typedef struct galay_postgres_value_view_t {
    const unsigned char* data; ///< Borrowed value bytes; NULL for SQL NULL.
    size_t data_len;           ///< Value byte count; may be zero for non-NULL.
    galay_bool_t is_null;      ///< `GALAY_TRUE` only for SQL NULL.
} galay_postgres_value_view_t;

/** Text-format prepared statement parameter binding. */
typedef struct galay_postgres_stmt_bind_t {
    const unsigned char* data; ///< Borrowed text-format bytes during execute.
    size_t data_len;           ///< Parameter byte count.
    galay_bool_t is_null;      ///< When true, data and data_len are ignored.
} galay_postgres_stmt_bind_t;

/**
 * @brief Convert a common status code to a stable PostgreSQL error string.
 * @param status Any `galay_status_t`, including unknown numeric values.
 * @return Static read-only storage; unknown values return `"unknown"`.
 * @note `C_IOResult.value` carries one of these statuses for module errors.
 */
const char* galay_postgres_get_error(galay_status_t status);

/* Configuration --------------------------------------------------------- */
/**
 * @brief Create a PostgreSQL configuration.
 * @param out Receives ownership; destroy with `galay_postgres_config_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_config_create(galay_postgres_config_t** out);

/**
 * @brief Destroy a PostgreSQL configuration.
 * @param config May be NULL.
 * @note All strings borrowed from this configuration become invalid.
 */
void galay_postgres_config_destroy(galay_postgres_config_t* config);

/**
 * @brief Get the configured host.
 * @param config Configuration handle.
 * @param host Receives a borrowed NUL-terminated string.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 * @note The pointer remains valid until the next host setter or destroy.
 */
galay_status_t galay_postgres_config_host(const galay_postgres_config_t* config,
                                          const char** host);

/**
 * @brief Get the configured TCP port.
 * @param config Configuration handle.
 * @param port Receives the non-zero TCP port.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT` for a NULL argument.
 */
galay_status_t galay_postgres_config_port(const galay_postgres_config_t* config,
                                          uint16_t* port);

/**
 * @brief Get the configured username.
 * @param config Configuration handle.
 * @param username Receives a borrowed string valid until mutation or destroy.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_config_username(const galay_postgres_config_t* config,
                                              const char** username);

/**
 * @brief Get the configured password.
 * @param config Configuration handle.
 * @param password Receives a borrowed string valid until mutation or destroy.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_config_password(const galay_postgres_config_t* config,
                                              const char** password);

/**
 * @brief Get the configured database.
 * @param config Configuration handle.
 * @param database Receives a borrowed string valid until mutation or destroy.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_config_database(const galay_postgres_config_t* config,
                                              const char** database);

/**
 * @brief Get the configured application name.
 * @param config Configuration handle.
 * @param application_name Receives a borrowed string valid until mutation or destroy.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_config_application_name(const galay_postgres_config_t* config,
                                                      const char** application_name);

/**
 * @brief Get the fallback connect timeout.
 * @param config Configuration handle.
 * @param timeout_ms Receives the positive timeout in milliseconds.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT` for a NULL argument.
 */
galay_status_t galay_postgres_config_connect_timeout_ms(const galay_postgres_config_t* config,
                                                        uint32_t* timeout_ms);

/**
 * @brief Get whether TCP_NODELAY is requested for new connections.
 * @param config Configuration handle.
 * @param enabled Receives `GALAY_TRUE` when the option is enabled.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT` for a NULL argument.
 */
galay_status_t galay_postgres_config_tcp_no_delay(const galay_postgres_config_t* config,
                                                  galay_bool_t* enabled);

/**
 * @brief Set the numeric IPv4 or IPv6 host used by the C socket ABI.
 * @param config Configuration handle.
 * @param host Non-empty string copied by the configuration.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_config_set_host(galay_postgres_config_t* config,
                                              const char* host);

/**
 * @brief Set a non-zero TCP port.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT` for a NULL config or zero port.
 */
galay_status_t galay_postgres_config_set_port(galay_postgres_config_t* config,
                                              uint16_t port);

/**
 * @brief Set a non-empty username, copying the supplied string.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_config_set_username(galay_postgres_config_t* config,
                                                  const char* username);

/**
 * @brief Set the password, copying the supplied string.
 * @details An empty password is allowed and is copied as an empty string.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_config_set_password(galay_postgres_config_t* config,
                                                  const char* password);

/**
 * @brief Set the startup database, copying the supplied string.
 * @details An empty database omits the startup parameter.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_config_set_database(galay_postgres_config_t* config,
                                                  const char* database);

/**
 * @brief Set the startup application name, copying the supplied string.
 * @details An empty name omits the startup parameter.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_config_set_application_name(galay_postgres_config_t* config,
                                                          const char* application_name);

/**
 * @brief Set the positive fallback connect timeout in milliseconds.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT` for zero or a NULL config.
 */
galay_status_t galay_postgres_config_set_connect_timeout_ms(galay_postgres_config_t* config,
                                                            uint32_t timeout_ms);

/**
 * @brief Set the TCP_NODELAY request for newly created sockets.
 * @param enabled Must be `GALAY_FALSE` or `GALAY_TRUE`.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_config_set_tcp_no_delay(galay_postgres_config_t* config,
                                                      galay_bool_t enabled);

/**
 * @brief Validate that a configuration can start a connection.
 * @return `GALAY_OK` when host, port, username and timeout are valid; otherwise
 * `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_config_validate(const galay_postgres_config_t* config);

/* Buffer and wire protocol --------------------------------------------- */
/**
 * @brief Destroy an owning wire buffer.
 * @param buffer May be NULL.
 * @note Any pointer returned by `galay_postgres_buffer_data` becomes invalid.
 */
void galay_postgres_buffer_destroy(galay_postgres_buffer_t* buffer);

/**
 * @brief Borrow bytes from an owning wire buffer.
 * @param buffer Buffer handle.
 * @param data Receives a read-only borrowed pointer.
 * @param data_len Receives the byte count.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_buffer_data(const galay_postgres_buffer_t* buffer,
                                          const unsigned char** data,
                                          size_t* data_len);

/**
 * @brief Parse a PostgreSQL v3 typed-message header.
 * @param data Input containing at least five bytes.
 * @param data_len Available byte count.
 * @param header Receives the copied header values.
 * @return Truncated or invalid lengths return `GALAY_PROTOCOL_ERROR`.
 */
galay_status_t galay_postgres_parse_message_header(const unsigned char* data,
                                                   size_t data_len,
                                                   galay_postgres_message_header_t* header);

/**
 * @brief Extract one complete typed message from contiguous input.
 * @param data Input bytes borrowed by the returned view.
 * @param data_len Available byte count; trailing bytes are allowed.
 * @param view Receives payload pointer, size and consumed byte count.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_PROTOCOL_ERROR`.
 */
galay_status_t galay_postgres_extract_message(const unsigned char* data,
                                              size_t data_len,
                                              galay_postgres_message_view_t* view);

/**
 * @brief Encode an untyped StartupMessage from a validated configuration.
 * @param config Username plus optional database/application name are copied.
 * @param out Receives buffer ownership; destroy it after use.
 * @return Explicit argument, allocation, or protocol-size status.
 */
galay_status_t galay_postgres_encode_startup_message(const galay_postgres_config_t* config,
                                                     galay_postgres_buffer_t** out);

/**
 * @brief Encode a SASLInitialResponse PasswordMessage.
 * @param mechanism Non-empty SASL mechanism name, copied during the call.
 * @param client_first NUL-terminated client-first-message bytes.
 * @param out Receives buffer ownership; destroy with `galay_postgres_buffer_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_encode_sasl_initial_response(const char* mechanism,
                                                           const char* client_first,
                                                           galay_postgres_buffer_t** out);

/**
 * @brief Encode a SASLResponse PasswordMessage.
 * @param client_final NUL-terminated client-final-message bytes.
 * @param out Receives buffer ownership; destroy with `galay_postgres_buffer_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_encode_sasl_response(const char* client_final,
                                                   galay_postgres_buffer_t** out);

/**
 * @brief Encode a NUL-terminated cleartext or MD5 PasswordMessage.
 * @param password Password bytes copied into the message.
 * @param out Receives buffer ownership; destroy with `galay_postgres_buffer_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_encode_password_message(const char* password,
                                                      galay_postgres_buffer_t** out);

/**
 * @brief Encode a simple Query message.
 * @param sql NUL-terminated SQL copied into the message; must not be NULL.
 * @param out Receives buffer ownership; destroy with `galay_postgres_buffer_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_encode_query(const char* sql,
                                           galay_postgres_buffer_t** out);

/**
 * @brief Encode a Terminate message.
 * @param out Receives buffer ownership; destroy with `galay_postgres_buffer_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_encode_terminate(galay_postgres_buffer_t** out);

/**
 * @brief Encode an extended-query Parse message.
 * @param statement_name Statement name, with empty selecting the unnamed statement.
 * @param sql SQL copied into the message.
 * @param parameter_type_oids Optional parameter OID array borrowed during the call.
 * @param parameter_type_count Number of OIDs, limited to signed 16-bit range.
 * @param out Receives buffer ownership.
 * @return Explicit argument, allocation, or protocol-size status.
 */
galay_status_t galay_postgres_encode_parse(const char* statement_name,
                                           const char* sql,
                                           const uint32_t* parameter_type_oids,
                                           size_t parameter_type_count,
                                           galay_postgres_buffer_t** out);

/**
 * @brief Encode a text-format Bind message.
 * @param portal_name Portal name; empty selects the unnamed portal.
 * @param statement_name Prepared statement name.
 * @param binds Borrowed parameter array; may be NULL when count is zero.
 * @param bind_count Parameter count, limited to signed 16-bit range.
 * @param out Receives buffer ownership.
 * @return Explicit argument, allocation, or protocol-size status.
 */
galay_status_t galay_postgres_encode_bind(const char* portal_name,
                                          const char* statement_name,
                                          const galay_postgres_stmt_bind_t* binds,
                                          size_t bind_count,
                                          galay_postgres_buffer_t** out);

/**
 * @brief Encode Describe for a named or unnamed statement.
 * @param statement_name Empty selects the unnamed statement; the name is copied.
 * @param out Receives buffer ownership; destroy with `galay_postgres_buffer_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_encode_describe_statement(const char* statement_name,
                                                        galay_postgres_buffer_t** out);

/**
 * @brief Encode Describe for a named or unnamed portal.
 * @param portal_name Empty selects the unnamed portal; the name is copied.
 * @param out Receives buffer ownership; destroy with `galay_postgres_buffer_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_encode_describe_portal(const char* portal_name,
                                                     galay_postgres_buffer_t** out);

/**
 * @brief Encode Execute for a portal.
 * @param portal_name Empty selects the unnamed portal; the name is copied.
 * @param max_rows Zero requests all rows; positive values request a bounded portal run.
 * @param out Receives buffer ownership; destroy with `galay_postgres_buffer_destroy`.
 * @return Explicit argument, allocation, or protocol-size status.
 * @note A bounded execution may end in PortalSuspended and require another Execute.
 */
galay_status_t galay_postgres_encode_execute(const char* portal_name,
                                             uint32_t max_rows,
                                             galay_postgres_buffer_t** out);

/**
 * @brief Encode Sync, which makes the server emit ReadyForQuery.
 * @param out Receives buffer ownership; destroy with `galay_postgres_buffer_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_encode_sync(galay_postgres_buffer_t** out);

/**
 * @brief Encode Close for a named or unnamed prepared statement.
 * @param statement_name Empty selects the unnamed statement; the name is copied.
 * @param out Receives buffer ownership; destroy with `galay_postgres_buffer_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_encode_close_statement(const char* statement_name,
                                                     galay_postgres_buffer_t** out);

/**
 * @brief Encode Close for a named or unnamed portal.
 * @param portal_name Empty selects the unnamed portal; the name is copied.
 * @param out Receives buffer ownership; destroy with `galay_postgres_buffer_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_encode_close_portal(const char* portal_name,
                                                  galay_postgres_buffer_t** out);

/* Result-set decode and views ------------------------------------------- */
/**
 * @brief Create an empty result set for repeated query reuse.
 * @param out Receives ownership; destroy with `galay_postgres_result_set_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_result_set_create(galay_postgres_result_set_t** out);

/**
 * @brief Reset logical contents while retaining decoded storage capacity.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 * @note Invalidates every field and value view borrowed from this result set.
 */
galay_status_t galay_postgres_result_set_reset(galay_postgres_result_set_t* result);

/**
 * @brief Decode a complete response sequence ending in ReadyForQuery.
 * @param data Contiguous typed messages borrowed only during the call.
 * @param data_len Input byte count.
 * @param out Receives result-set ownership on success.
 * @return Server ErrorResponse, malformed, truncated or incomplete sequences
 * return `GALAY_PROTOCOL_ERROR`; allocation failures are explicit.
 */
galay_status_t galay_postgres_result_set_decode(const unsigned char* data,
                                                size_t data_len,
                                                galay_postgres_result_set_t** out);

/**
 * @brief Destroy an independently owned result set.
 * @param result May be NULL.
 * @note Do not pass an item borrowed from a pipeline result.
 */
void galay_postgres_result_set_destroy(galay_postgres_result_set_t* result);

/**
 * @brief Get the RowDescription field count.
 * @param result Result-set handle.
 * @param count Receives the number of result columns.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_result_set_field_count(const galay_postgres_result_set_t* result,
                                                     size_t* count);

/**
 * @brief Get the decoded DataRow count.
 * @param result Result-set handle.
 * @param count Receives the number of rows.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_result_set_row_count(const galay_postgres_result_set_t* result,
                                                   size_t* count);

/**
 * @brief Borrow metadata for one result column.
 * @return `GALAY_NOT_FOUND` for an out-of-range index.
 * @note The field name remains valid only while the result set is alive.
 */
galay_status_t galay_postgres_result_set_field(const galay_postgres_result_set_t* result,
                                               size_t index,
                                               galay_postgres_field_view_t* field);

/**
 * @brief Find the first field with an exact, case-sensitive name.
 * @param result Result-set handle.
 * @param name NUL-terminated field name borrowed during the call.
 * @param index Receives the zero-based column index.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_NOT_FOUND`.
 */
galay_status_t galay_postgres_result_set_find_field(const galay_postgres_result_set_t* result,
                                                    const char* name,
                                                    size_t* index);

/**
 * @brief Borrow one decoded cell value.
 * @return `GALAY_NOT_FOUND` for an out-of-range row or column.
 * @note Value bytes may contain NUL and remain valid until result-set destroy.
 */
galay_status_t galay_postgres_result_set_value(const galay_postgres_result_set_t* result,
                                               size_t row,
                                               size_t column,
                                               galay_postgres_value_view_t* value);

/**
 * @brief Borrow the NUL-terminated CommandComplete tag.
 * @param result Result-set handle.
 * @param tag Receives a pointer valid until reset, reuse, or destroy.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_result_set_command_tag(const galay_postgres_result_set_t* result,
                                                     const char** tag);

/**
 * @brief Get the trailing row count parsed from the CommandComplete tag.
 * @param affected_rows Receives the parsed count, or zero when no count is present.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_result_set_affected_rows(const galay_postgres_result_set_t* result,
                                                       uint64_t* affected_rows);

/**
 * @brief Get the ReadyForQuery transaction status.
 * @param status Receives `I` for idle, `T` for transaction, or `E` for failed transaction.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_result_set_transaction_status(
    const galay_postgres_result_set_t* result, char* status);

/* Prepared metadata and command pipeline -------------------------------- */
/**
 * @brief Create local prepared-statement metadata with a non-empty name.
 * @param name Statement name copied into the metadata.
 * @param out Receives ownership; destroy with `galay_postgres_stmt_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 * @note This pure local helper does not send Parse to a server.
 */
galay_status_t galay_postgres_stmt_create(const char* name,
                                          galay_postgres_stmt_t** out);

/**
 * @brief Destroy local statement metadata; no server Close message is sent.
 * @param stmt May be NULL; all borrowed names and fields become invalid.
 */
void galay_postgres_stmt_destroy(galay_postgres_stmt_t* stmt);

/**
 * @brief Borrow the statement name.
 * @param name Receives a NUL-terminated pointer valid until statement destroy.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_stmt_name(const galay_postgres_stmt_t* stmt,
                                        const char** name);

/**
 * @brief Get the server-reported ParameterDescription count.
 * @param count Receives the number of parameters.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_stmt_param_count(const galay_postgres_stmt_t* stmt,
                                               size_t* count);

/**
 * @brief Get the server-reported result-column count.
 * @param count Receives the number of result columns.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_stmt_column_count(const galay_postgres_stmt_t* stmt,
                                                size_t* count);

/**
 * @brief Borrow prepared result-column metadata.
 * @param index Zero-based result-column index.
 * @param field Receives metadata whose name is valid until statement destroy.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_NOT_FOUND`.
 */
galay_status_t galay_postgres_stmt_field(const galay_postgres_stmt_t* stmt,
                                         size_t index,
                                         galay_postgres_field_view_t* field);

/**
 * @brief Create an empty owning command pipeline.
 * @param out Receives ownership; destroy with `galay_postgres_pipeline_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_pipeline_create(galay_postgres_pipeline_t** out);

/**
 * @brief Destroy a pipeline and its copied command bytes.
 * @param pipeline May be NULL; previously produced result objects are independent.
 */
void galay_postgres_pipeline_destroy(galay_postgres_pipeline_t* pipeline);

/**
 * @brief Append a copied simple Query message, which expects one ReadyForQuery.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 * @note Not safe to call while the same pipeline is being built or executed.
 */
galay_status_t galay_postgres_pipeline_append_query(galay_postgres_pipeline_t* pipeline,
                                                    const char* sql);

/**
 * @brief Append a copied Parse message.
 * @param statement_name Named or unnamed statement name copied into the command.
 * @param sql SQL copied into the command.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 * @note Append Sync to delimit the Parse response.
 */
galay_status_t galay_postgres_pipeline_append_parse(galay_postgres_pipeline_t* pipeline,
                                                    const char* statement_name,
                                                    const char* sql);

/**
 * @brief Append Sync and one expected ReadyForQuery boundary.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_pipeline_append_sync(galay_postgres_pipeline_t* pipeline);

/**
 * @brief Concatenate pipeline messages into an owning wire buffer.
 * @param pipeline Non-empty pipeline borrowed during the call.
 * @param out Receives buffer ownership; destroy with `galay_postgres_buffer_destroy`.
 * @param expected_ready Receives the number of ReadyForQuery boundaries.
 * @return Empty pipelines and NULL arguments return `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_pipeline_build(const galay_postgres_pipeline_t* pipeline,
                                             galay_postgres_buffer_t** out,
                                             size_t* expected_ready);

/* Client/pool lifecycle. Network operations are coroutine APIs. ---------- */
/**
 * @brief Create a disconnected PostgreSQL client.
 * @param out Receives ownership; destroy with `galay_postgres_client_destroy`.
 * @return `GALAY_OK`, `GALAY_INVALID_ARGUMENT`, or `GALAY_OUT_OF_MEMORY`.
 */
galay_status_t galay_postgres_client_create(galay_postgres_client_t** out);

/**
 * @brief Destroy a client and synchronously release any socket storage.
 * @param client May be NULL.
 * @note Does not wait for active operations; all async calls must finish first.
 */
void galay_postgres_client_destroy(galay_postgres_client_t* client);

/**
 * @brief Synchronously discard the local socket without sending Terminate.
 * @param client May be NULL.
 * @note Use `galay_postgres_client_close_async` for a graceful protocol close.
 */
void galay_postgres_client_close(galay_postgres_client_t* client);

/**
 * @brief Get whether startup and authentication reached ReadyForQuery.
 * @param connected Receives `GALAY_TRUE` for an authenticated connection.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 * @note Do not call concurrently with another operation on the client.
 */
galay_status_t galay_postgres_client_is_connected(const galay_postgres_client_t* client,
                                                  galay_bool_t* connected);

/**
 * @brief Synchronous connect placeholder.
 * @return Valid arguments return `GALAY_UNSUPPORTED`; invalid state or arguments
 * return `GALAY_INVALID_ARGUMENT`.
 * @note Use `galay_postgres_client_connect_async` for network connection.
 */
galay_status_t galay_postgres_client_connect(galay_postgres_client_t* client,
                                             const galay_postgres_config_t* config);

/**
 * @brief Connect, send StartupMessage and authenticate in the current C coroutine.
 * @details Supports SCRAM-SHA-256, PostgreSQL MD5 and cleartext password requests;
 * startup messages are drained through ReadyForQuery before success.
 * @param client Disconnected client with no existing socket.
 * @param config Valid configuration borrowed until this call returns.
 * @param timeout_ms Negative uses config connect timeout; otherwise milliseconds per I/O.
 * @return Success sets `code` to `C_IOResultOk` and `ptr` to client; failures are explicit.
 * @note Suspends rather than blocking the scheduler thread; do not call concurrently.
 */
C_IOResult galay_postgres_client_connect_async(galay_postgres_client_t* client,
                                               const galay_postgres_config_t* config,
                                               int64_t timeout_ms);

/**
 * @brief Execute a simple Query and decode its complete result sequence.
 * @param client Authenticated client.
 * @param sql SQL copied into the outgoing message.
 * @param timeout_ms Milliseconds per socket operation; negative disables timeout.
 * @param result Receives result-set ownership on success and NULL on failure.
 * @return Server ErrorResponse returns `C_IOResultError` with
 * `value == GALAY_PROTOCOL_ERROR` after draining through ReadyForQuery.
 * @note Suspends the current C coroutine; the same client must not be used concurrently.
 */
C_IOResult galay_postgres_client_query_async(galay_postgres_client_t* client,
                                             const char* sql,
                                             int64_t timeout_ms,
                                             galay_postgres_result_set_t** result);

/** Alias of `galay_postgres_client_query_async` retained for result-oriented naming. */
C_IOResult galay_postgres_client_query_result_async(galay_postgres_client_t* client,
                                                    const char* sql,
                                                    int64_t timeout_ms,
                                                    galay_postgres_result_set_t** result);

/**
 * @brief Execute a simple Query into caller-owned reusable result storage.
 * @param client Authenticated client.
 * @param sql SQL copied into the outgoing message.
 * @param timeout_ms Milliseconds per socket operation; negative disables timeout.
 * @param result Result created by `galay_postgres_result_set_create`; reset automatically.
 * @return Same explicit I/O and protocol errors as `galay_postgres_client_query_async`.
 * @note Retains field, row and value capacity between calls. All previously borrowed views
 * are invalidated when the call starts; do not use the same result concurrently.
 */
C_IOResult galay_postgres_client_query_into_async(galay_postgres_client_t* client,
                                                  const char* sql,
                                                  int64_t timeout_ms,
                                                  galay_postgres_result_set_t* result);

/**
 * @brief Send `BEGIN` and decode its complete result sequence.
 * @param timeout_ms Milliseconds per socket operation; negative disables timeout.
 * @param result Receives result-set ownership on success and NULL on failure.
 * @return Same explicit I/O and protocol errors as `galay_postgres_client_query_async`.
 */
C_IOResult galay_postgres_client_begin_transaction_async(
    galay_postgres_client_t* client,
    int64_t timeout_ms,
    galay_postgres_result_set_t** result);

/**
 * @brief Send `COMMIT` and return an owning decoded result set.
 * @param timeout_ms Milliseconds per socket operation; negative disables timeout.
 * @param result Receives result-set ownership on success and NULL on failure.
 * @return Same explicit I/O and protocol errors as `galay_postgres_client_query_async`.
 */
C_IOResult galay_postgres_client_commit_async(galay_postgres_client_t* client,
                                              int64_t timeout_ms,
                                              galay_postgres_result_set_t** result);

/**
 * @brief Send `ROLLBACK` and return an owning decoded result set.
 * @param timeout_ms Milliseconds per socket operation; negative disables timeout.
 * @param result Receives result-set ownership on success and NULL on failure.
 * @return Same explicit I/O and protocol errors as `galay_postgres_client_query_async`.
 */
C_IOResult galay_postgres_client_rollback_async(galay_postgres_client_t* client,
                                                int64_t timeout_ms,
                                                galay_postgres_result_set_t** result);

/**
 * @brief Parse and describe a named prepared statement.
 * @param client Authenticated client.
 * @param statement_name Non-empty name copied into returned metadata.
 * @param sql SQL to parse.
 * @param timeout_ms Milliseconds per socket operation.
 * @param stmt Receives metadata ownership on success.
 * @return Reads ParseComplete, ParameterDescription, RowDescription/NoData and
 * ReadyForQuery; server errors are drained before returning failure.
 */
C_IOResult galay_postgres_client_stmt_prepare_async(galay_postgres_client_t* client,
                                                    const char* statement_name,
                                                    const char* sql,
                                                    int64_t timeout_ms,
                                                    galay_postgres_stmt_t** stmt);

/**
 * @brief Bind text parameters and execute a prepared statement.
 * @param client Authenticated client.
 * @param stmt Prepared metadata borrowed during this call.
 * @param binds Parameter views; may be NULL only when bind_count is zero.
 * @param bind_count Must equal the server-reported parameter count.
 * @param timeout_ms Milliseconds per socket operation.
 * @param result Receives result-set ownership on success.
 * @note Suspends the current coroutine and drains responses through ReadyForQuery.
 */
C_IOResult galay_postgres_client_stmt_execute_async(
    galay_postgres_client_t* client,
    const galay_postgres_stmt_t* stmt,
    const galay_postgres_stmt_bind_t* binds,
    size_t bind_count,
    int64_t timeout_ms,
    galay_postgres_result_set_t** result);

/**
 * @brief Send all encoded pipeline commands and read each ReadyForQuery segment.
 * @param client Authenticated client.
 * @param pipeline Non-empty pipeline with at least one ready boundary.
 * @param timeout_ms Milliseconds per socket operation.
 * @param result Receives owning ordered pipeline results.
 * @return Any segment error is returned only after its ready boundary is consumed.
 */
C_IOResult galay_postgres_client_pipeline_async(galay_postgres_client_t* client,
                                                const galay_postgres_pipeline_t* pipeline,
                                                int64_t timeout_ms,
                                                galay_postgres_pipeline_result_t** result);

/**
 * @brief Destroy a pipeline result and every result set it owns.
 * @param result May be NULL; every item borrowed from it becomes invalid.
 */
void galay_postgres_pipeline_result_destroy(galay_postgres_pipeline_result_t* result);

/**
 * @brief Get the number of owned result sets in a pipeline result.
 * @param count Receives the number of result sets.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 */
galay_status_t galay_postgres_pipeline_result_count(
    const galay_postgres_pipeline_result_t* result, size_t* count);

/**
 * @brief Borrow one pipeline result-set item.
 * @return Out-of-range indices return `GALAY_NOT_FOUND`.
 * @note The item must not be destroyed and expires with the pipeline result.
 */
galay_status_t galay_postgres_pipeline_result_at(
    const galay_postgres_pipeline_result_t* result,
    size_t index,
    const galay_postgres_result_set_t** item);

/**
 * @brief Gracefully send Terminate, close, and destroy the client socket.
 * @param timeout_ms Milliseconds for send and close operations; negative disables timeout.
 * @return Cleanup is attempted on every path; failures are returned explicitly.
 * @note Suspends the current C coroutine and leaves the client disconnected.
 */
C_IOResult galay_postgres_client_close_async(galay_postgres_client_t* client,
                                             int64_t timeout_ms);

/**
 * @brief Create a lazy PostgreSQL connection pool.
 * @param config Valid configuration copied into the pool.
 * @param max_connections Positive connection limit.
 * @param out Receives pool ownership.
 * @note The pool is single-threaded and must outlive every lease.
 */
galay_status_t galay_postgres_pool_create(const galay_postgres_config_t* config,
                                          size_t max_connections,
                                          galay_postgres_pool_t** out);

/**
 * @brief Destroy a pool and all idle clients.
 * @param pool May be NULL.
 * @note All leases must be released first; outstanding leases are not owned by the idle list.
 */
void galay_postgres_pool_destroy(galay_postgres_pool_t* pool);

/**
 * @brief Acquire an idle connection or lazily connect a new one.
 * @param pool Pool handle.
 * @param timeout_ms Connection/authentication timeout passed to the client.
 * @param lease Receives lease ownership on success.
 * @return At capacity, returns `C_IOResultError` with value `GALAY_UNSUPPORTED`;
 * this implementation has no waiting queue.
 */
C_IOResult galay_postgres_pool_acquire_async(galay_postgres_pool_t* pool,
                                             int64_t timeout_ms,
                                             galay_postgres_pool_lease_t** lease);

/**
 * @brief Borrow the client held by a lease.
 * @param lease Active lease handle.
 * @param client Receives a borrowed client pointer.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 * @note The pointer expires at release and must not be destroyed by the caller.
 */
galay_status_t galay_postgres_pool_lease_client(galay_postgres_pool_lease_t* lease,
                                                galay_postgres_client_t** client);

/**
 * @brief Return a lease client to its pool and destroy the lease handle.
 * @return `GALAY_OK` or `GALAY_INVALID_ARGUMENT`.
 * @note The pool must still be alive and the borrowed client must have no active operation.
 */
galay_status_t galay_postgres_pool_lease_release(galay_postgres_pool_lease_t* lease);

#ifdef __cplusplus
}
#endif

#endif /* GALAY_C_POSTGRES_POSTGRES_C_H */

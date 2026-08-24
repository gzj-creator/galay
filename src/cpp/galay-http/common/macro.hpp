/**
 * @file macro.hpp
 * @brief galay-http 编译期宏集中定义
 */

#ifndef GALAY_HTTP_COMMON_MACRO_HPP
#define GALAY_HTTP_COMMON_MACRO_HPP

/** @brief HTTP server version string. */
#ifndef GALAY_VERSION
#define GALAY_VERSION "3.1.0"
#endif

/** @brief Default HTTP receive timeout in milliseconds. */
#ifndef DEFAULT_HTTP_RECV_TIME_MS
#define DEFAULT_HTTP_RECV_TIME_MS 5 * 60 * 1000
#endif

/** @brief Default HTTP send timeout in milliseconds. */
#ifndef DEFAULT_HTTP_SEND_TIME_MS
#define DEFAULT_HTTP_SEND_TIME_MS 5 * 60 * 1000
#endif

/** @brief Maximum HTTP header size in bytes. */
#ifndef DEFAULT_HTTP_MAX_HEADER_SIZE
#define DEFAULT_HTTP_MAX_HEADER_SIZE 8192
#endif

/** @brief Maximum HTTP body size in bytes. */
#ifndef DEFAULT_HTTP_MAX_BODY_SIZE
#define DEFAULT_HTTP_MAX_BODY_SIZE 1 * 1024 * 1024
#endif

/** @brief Maximum HTTP URI length in bytes. */
#ifndef DEFAULT_HTTP_MAX_URI_LEN
#define DEFAULT_HTTP_MAX_URI_LEN 1024
#endif

/** @brief Maximum HTTP version string length. */
#ifndef DEFAULT_HTTP_MAX_VERSION_SIZE
#define DEFAULT_HTTP_MAX_VERSION_SIZE 32
#endif

/** @brief Default peer read step in bytes. */
#ifndef DEFAULT_HTTP_PEER_STEP_SIZE
#define DEFAULT_HTTP_PEER_STEP_SIZE 1024
#endif

/** @brief Default chunk parser buffer size in bytes. */
#ifndef DEFAULT_HTTP_CHUNK_BUFFER_SIZE
#define DEFAULT_HTTP_CHUNK_BUFFER_SIZE 2048
#endif

/** @brief Default keep-alive idle timeout in milliseconds. */
#ifndef DEFAULT_HTTP_KEEPALIVE_TIME_MS
#define DEFAULT_HTTP_KEEPALIVE_TIME_MS (7500 * 1000)
#endif

/** @brief HTTP server product name. */
#ifndef SERVER_NAME
#define SERVER_NAME "galay-http"
#endif

/** @brief HTTP Server header value. */
#ifndef GALAY_SERVER
#define GALAY_SERVER SERVER_NAME "/" GALAY_VERSION
#endif

/// HTTP chunk parser SIMD capability.
#if !defined(GALAY_HTTP_SIMD_X86) && !defined(GALAY_HTTP_SIMD_NEON)
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define GALAY_HTTP_SIMD_X86 1
#elif defined(__ARM_NEON) || defined(__aarch64__)
#define GALAY_HTTP_SIMD_NEON 1
#endif
#endif

#endif  // GALAY_HTTP_COMMON_MACRO_HPP

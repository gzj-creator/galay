#include <galay/c/galay-http2-c/http2_c.h>

#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <new>
#include <string>
#include <vector>

struct galay_http2_frame_t {
    galay_http2_frame_type_t type = GALAY_HTTP2_FRAME_DATA;
    uint8_t flags = 0;
    uint32_t stream_id = 0;
    std::vector<uint8_t> payload;
};

struct galay_http2_header_item {
    std::string name;
    std::string value;
};

struct galay_http2_headers_t {
    std::vector<galay_http2_header_item> entries;
};

constexpr uint8_t kFlagEndStream = 0x1;
constexpr uint8_t kFlagAck = 0x1;
constexpr uint8_t kFlagEndHeaders = 0x4;
constexpr uint32_t kDefaultInitialWindow = 65535;
constexpr uint32_t kDefaultMaxFrameSize = 16384;
constexpr uint32_t kMaxWindow = 0x7FFFFFFFu;
constexpr char kClientPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr size_t kClientPrefaceLength = sizeof(kClientPreface) - 1;

struct DataChunk {
    std::vector<char> bytes;
    bool end_stream = false;
};

struct RawFrame {
    galay_http2_frame_type_t type = GALAY_HTTP2_FRAME_DATA;
    uint8_t flags = 0;
    uint32_t stream_id = 0;
    std::vector<uint8_t> payload;
};

struct galay_http2_conn_t {
    galay_kernel_tcp_socket_t socket{};
    bool owns_socket = false;
    bool is_client = false;
    bool settings_ack_received = false;
    bool goaway_received = false;
    uint32_t goaway_last_stream_id = 0;
    uint32_t next_stream_id = 1;
    uint32_t local_initial_window = kDefaultInitialWindow;
    uint32_t peer_initial_window = kDefaultInitialWindow;
    uint32_t local_max_frame_size = kDefaultMaxFrameSize;
    uint32_t peer_max_frame_size = kDefaultMaxFrameSize;
    int32_t conn_send_window = static_cast<int32_t>(kDefaultInitialWindow);
    int32_t conn_recv_window = static_cast<int32_t>(kDefaultInitialWindow);
    uint32_t max_concurrent_streams = 100;
    std::vector<galay_http2_stream_t*> streams;
};

struct galay_http2_stream_t {
    galay_http2_conn_t* conn = nullptr;
    uint32_t id = 0;
    galay_http2_stream_state_t state = GALAY_HTTP2_STREAM_IDLE;
    int32_t send_window = static_cast<int32_t>(kDefaultInitialWindow);
    int32_t recv_window = static_cast<int32_t>(kDefaultInitialWindow);
    bool reset_received = false;
    bool reset_sent = false;
    std::deque<galay_http2_headers_t*> pending_headers;
    std::deque<DataChunk> pending_data;
};

struct galay_http2_client_t {
    galay_http2_config_t config{};
    std::string host;
    galay_http2_conn_t* conn = nullptr;
};

struct galay_http2_server_t {
    galay_http2_config_t config{};
    std::string host;
    galay_kernel_tcp_socket_t listener{};
    bool listening = false;
};

namespace {

C_IOResult result(C_IOResultCode code, galay_http2_error_code_t error, size_t bytes = 0)
{
    return C_IOResult{code, 0, bytes, static_cast<int64_t>(error), nullptr};
}

C_IOResult map_io_result(C_IOResult io)
{
    if (io.code == C_IOResultOk) {
        return io;
    }
    if (io.code == C_IOResultTimeout) {
        io.value = GALAY_HTTP2_ERROR_TIMEOUT;
        return io;
    }
    if (io.code == C_IOResultInvalid) {
        io.value = GALAY_HTTP2_ERROR_INTERNAL;
        return io;
    }
    io.code = C_IOResultError;
    io.value = GALAY_HTTP2_ERROR_IO;
    return io;
}

galay_http2_config_t normalize_config(const galay_http2_config_t* config)
{
    galay_http2_config_t normalized{
        "127.0.0.1",
        0,
        kDefaultInitialWindow,
        kDefaultMaxFrameSize,
        100,
    };
    if (config == nullptr) {
        return normalized;
    }
    if (config->host != nullptr && config->host[0] != '\0') {
        normalized.host = config->host;
    }
    normalized.port = config->port;
    if (config->initial_window_size > 0 && config->initial_window_size <= kMaxWindow) {
        normalized.initial_window_size = config->initial_window_size;
    }
    if (config->max_frame_size >= 16384u && config->max_frame_size <= 16777215u) {
        normalized.max_frame_size = config->max_frame_size;
    }
    if (config->max_concurrent_streams > 0) {
        normalized.max_concurrent_streams = config->max_concurrent_streams;
    }
    return normalized;
}

void copy_host(C_Host& out, const char* host, uint16_t port)
{
    out.type = C_IPTypeIPV4;
    out.port = port;
    size_t i = 0;
    if (host != nullptr) {
        for (; i + 1 < C_HOST_ADDRESS_MAX_LENGTH && host[i] != '\0'; ++i) {
            out.address[i] = host[i];
        }
    }
    out.address[i] = '\0';
}

void append_u16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<uint8_t>(value & 0xFFU));
}

void append_u32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<uint8_t>(value & 0xFFU));
}

uint32_t read_u32(const std::vector<uint8_t>& data, size_t offset)
{
    return (static_cast<uint32_t>(data[offset]) << 24U) |
        (static_cast<uint32_t>(data[offset + 1]) << 16U) |
        (static_cast<uint32_t>(data[offset + 2]) << 8U) |
        static_cast<uint32_t>(data[offset + 3]);
}

void append_setting(std::vector<uint8_t>& out, galay_http2_settings_id_t id, uint32_t value)
{
    append_u16(out, static_cast<uint16_t>(id));
    append_u32(out, value);
}

std::vector<uint8_t> make_settings_payload(const galay_http2_config_t& config)
{
    std::vector<uint8_t> payload;
    payload.reserve(12);
    append_setting(payload, GALAY_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE, config.initial_window_size);
    append_setting(payload, GALAY_HTTP2_SETTINGS_MAX_FRAME_SIZE, config.max_frame_size);
    return payload;
}

galay_http2_stream_t* find_stream(galay_http2_conn_t* conn, uint32_t stream_id)
{
    if (conn == nullptr) {
        return nullptr;
    }
    for (auto* stream : conn->streams) {
        if (stream != nullptr && stream->id == stream_id) {
            return stream;
        }
    }
    return nullptr;
}

void forget_stream(galay_http2_stream_t* stream)
{
    if (stream == nullptr || stream->conn == nullptr) {
        return;
    }
    auto& streams = stream->conn->streams;
    for (auto it = streams.begin(); it != streams.end(); ++it) {
        if (*it == stream) {
            streams.erase(it);
            break;
        }
    }
    stream->conn = nullptr;
}

void destroy_pending(galay_http2_stream_t* stream)
{
    if (stream == nullptr) {
        return;
    }
    while (!stream->pending_headers.empty()) {
        auto* headers = stream->pending_headers.front();
        stream->pending_headers.pop_front();
        delete headers;
    }
}

galay_http2_stream_t* create_stream(galay_http2_conn_t* conn, uint32_t stream_id,
                                    galay_http2_stream_state_t state)
{
    if (conn == nullptr || conn->streams.size() >= conn->max_concurrent_streams) {
        return nullptr;
    }
    auto* stream = new (std::nothrow) galay_http2_stream_t();
    if (stream == nullptr) {
        return nullptr;
    }
    stream->conn = conn;
    stream->id = stream_id;
    stream->state = state;
    stream->send_window = static_cast<int32_t>(conn->peer_initial_window);
    stream->recv_window = static_cast<int32_t>(conn->local_initial_window);
    conn->streams.push_back(stream);
    return stream;
}

void mark_local_end(galay_http2_stream_t* stream)
{
    if (stream == nullptr) {
        return;
    }
    if (stream->state == GALAY_HTTP2_STREAM_OPEN) {
        stream->state = GALAY_HTTP2_STREAM_HALF_CLOSED_LOCAL;
    } else if (stream->state == GALAY_HTTP2_STREAM_HALF_CLOSED_REMOTE) {
        stream->state = GALAY_HTTP2_STREAM_CLOSED;
    }
}

void mark_remote_end(galay_http2_stream_t* stream)
{
    if (stream == nullptr) {
        return;
    }
    if (stream->state == GALAY_HTTP2_STREAM_OPEN) {
        stream->state = GALAY_HTTP2_STREAM_HALF_CLOSED_REMOTE;
    } else if (stream->state == GALAY_HTTP2_STREAM_HALF_CLOSED_LOCAL) {
        stream->state = GALAY_HTTP2_STREAM_CLOSED;
    }
}

bool can_send_on_stream(const galay_http2_stream_t* stream)
{
    return stream != nullptr &&
        (stream->state == GALAY_HTTP2_STREAM_OPEN ||
         stream->state == GALAY_HTTP2_STREAM_HALF_CLOSED_REMOTE);
}

bool can_receive_on_stream(const galay_http2_stream_t* stream)
{
    return stream != nullptr &&
        (stream->state == GALAY_HTTP2_STREAM_OPEN ||
         stream->state == GALAY_HTTP2_STREAM_HALF_CLOSED_LOCAL);
}

C_IOResult send_all(galay_http2_conn_t* conn, const char* data, size_t length, int64_t timeout_ms)
{
    if (conn == nullptr || conn->socket.socket == nullptr || (data == nullptr && length > 0)) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    size_t sent = 0;
    while (sent < length) {
        C_IOResult io = galay_kernel_tcp_socket_send(&conn->socket, data + sent, length - sent, timeout_ms);
        if (io.code != C_IOResultOk) {
            return map_io_result(io);
        }
        if (io.bytes == 0) {
            return result(C_IOResultError, GALAY_HTTP2_ERROR_IO);
        }
        sent += io.bytes;
    }
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE, sent);
}

C_IOResult recv_exact(galay_http2_conn_t* conn, char* out, size_t length, int64_t timeout_ms)
{
    if (conn == nullptr || conn->socket.socket == nullptr || (out == nullptr && length > 0)) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    size_t received = 0;
    while (received < length) {
        C_IOResult io = galay_kernel_tcp_socket_recv(&conn->socket, out + received, length - received, timeout_ms);
        if (io.code != C_IOResultOk) {
            return map_io_result(io);
        }
        if (io.bytes == 0) {
            return result(C_IOResultEof, GALAY_HTTP2_ERROR_IO, received);
        }
        received += io.bytes;
    }
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE, received);
}

C_IOResult send_frame(galay_http2_conn_t* conn,
                      galay_http2_frame_type_t type,
                      uint8_t flags,
                      uint32_t stream_id,
                      const std::vector<uint8_t>& payload,
                      int64_t timeout_ms)
{
    if (conn == nullptr || payload.size() > conn->peer_max_frame_size) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_PROTOCOL);
    }

    std::vector<char> bytes;
    bytes.resize(GALAY_HTTP2_FRAME_HEADER_LENGTH + payload.size());
    const uint32_t len = static_cast<uint32_t>(payload.size());
    bytes[0] = static_cast<char>((len >> 16U) & 0xFFU);
    bytes[1] = static_cast<char>((len >> 8U) & 0xFFU);
    bytes[2] = static_cast<char>(len & 0xFFU);
    bytes[3] = static_cast<char>(type);
    bytes[4] = static_cast<char>(flags);
    bytes[5] = static_cast<char>((stream_id >> 24U) & 0x7FU);
    bytes[6] = static_cast<char>((stream_id >> 16U) & 0xFFU);
    bytes[7] = static_cast<char>((stream_id >> 8U) & 0xFFU);
    bytes[8] = static_cast<char>(stream_id & 0xFFU);
    for (size_t i = 0; i < payload.size(); ++i) {
        bytes[GALAY_HTTP2_FRAME_HEADER_LENGTH + i] = static_cast<char>(payload[i]);
    }
    return send_all(conn, bytes.data(), bytes.size(), timeout_ms);
}

galay_status_t validate_raw_frame(const RawFrame& frame)
{
    switch (frame.type) {
    case GALAY_HTTP2_FRAME_DATA:
    case GALAY_HTTP2_FRAME_HEADERS:
        return frame.stream_id == 0 ? GALAY_PROTOCOL_ERROR : GALAY_OK;
    case GALAY_HTTP2_FRAME_RST_STREAM:
        return frame.stream_id == 0 || frame.payload.size() != 4 ? GALAY_PROTOCOL_ERROR : GALAY_OK;
    case GALAY_HTTP2_FRAME_SETTINGS:
        if ((frame.flags & kFlagAck) != 0) {
            return frame.payload.empty() ? GALAY_OK : GALAY_PROTOCOL_ERROR;
        }
        return frame.stream_id == 0 && frame.payload.size() % 6 == 0 ? GALAY_OK : GALAY_PROTOCOL_ERROR;
    case GALAY_HTTP2_FRAME_PING:
        return frame.stream_id == 0 && frame.payload.size() == 8 ? GALAY_OK : GALAY_PROTOCOL_ERROR;
    case GALAY_HTTP2_FRAME_GOAWAY:
        return frame.stream_id == 0 && frame.payload.size() >= 8 ? GALAY_OK : GALAY_PROTOCOL_ERROR;
    case GALAY_HTTP2_FRAME_WINDOW_UPDATE:
        if (frame.payload.size() != 4) {
            return GALAY_PROTOCOL_ERROR;
        }
        return (read_u32(frame.payload, 0) & 0x7FFFFFFFu) == 0 ? GALAY_PROTOCOL_ERROR : GALAY_OK;
    }
    return GALAY_PROTOCOL_ERROR;
}

C_IOResult read_frame(galay_http2_conn_t* conn, RawFrame& frame, int64_t timeout_ms)
{
    char header[GALAY_HTTP2_FRAME_HEADER_LENGTH] = {};
    C_IOResult header_result = recv_exact(conn, header, sizeof(header), timeout_ms);
    if (header_result.code != C_IOResultOk) {
        return header_result;
    }

    const uint32_t len = (static_cast<uint32_t>(static_cast<uint8_t>(header[0])) << 16U) |
        (static_cast<uint32_t>(static_cast<uint8_t>(header[1])) << 8U) |
        static_cast<uint32_t>(static_cast<uint8_t>(header[2]));
    if (conn == nullptr || len > conn->local_max_frame_size) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_PROTOCOL);
    }

    frame.type = static_cast<galay_http2_frame_type_t>(static_cast<uint8_t>(header[3]));
    frame.flags = static_cast<uint8_t>(header[4]);
    frame.stream_id = (static_cast<uint32_t>(static_cast<uint8_t>(header[5]) & 0x7FU) << 24U) |
        (static_cast<uint32_t>(static_cast<uint8_t>(header[6])) << 16U) |
        (static_cast<uint32_t>(static_cast<uint8_t>(header[7])) << 8U) |
        static_cast<uint32_t>(static_cast<uint8_t>(header[8]));
    frame.payload.clear();
    frame.payload.resize(len);
    if (len > 0) {
        C_IOResult payload_result =
            recv_exact(conn, reinterpret_cast<char*>(frame.payload.data()), frame.payload.size(), timeout_ms);
        if (payload_result.code != C_IOResultOk) {
            return payload_result;
        }
    }
    if (validate_raw_frame(frame) != GALAY_OK) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_PROTOCOL);
    }
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE, GALAY_HTTP2_FRAME_HEADER_LENGTH + len);
}

galay_status_t encode_headers_payload(const galay_http2_headers_t* headers, std::vector<uint8_t>& out)
{
    if (headers == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    out.clear();
    size_t need = 0;
    for (const auto& entry : headers->entries) {
        if (entry.name.empty() || entry.name.size() > 255 || entry.value.size() > 255) {
            return GALAY_INVALID_ARGUMENT;
        }
        need += 3 + entry.name.size() + entry.value.size();
    }
    out.reserve(need);
    for (const auto& entry : headers->entries) {
        out.push_back(0x40);
        out.push_back(static_cast<uint8_t>(entry.name.size()));
        for (char ch : entry.name) {
            out.push_back(static_cast<uint8_t>(ch));
        }
        out.push_back(static_cast<uint8_t>(entry.value.size()));
        for (char ch : entry.value) {
            out.push_back(static_cast<uint8_t>(ch));
        }
    }
    return GALAY_OK;
}

galay_status_t decode_headers_payload(const std::vector<uint8_t>& payload, galay_http2_headers_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto* headers = new (std::nothrow) galay_http2_headers_t();
    if (headers == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }

    size_t pos = 0;
    while (pos < payload.size()) {
        if (payload[pos] != 0x40) {
            delete headers;
            return GALAY_PROTOCOL_ERROR;
        }
        ++pos;
        if (pos >= payload.size()) {
            delete headers;
            return GALAY_PROTOCOL_ERROR;
        }
        const size_t name_len = payload[pos];
        ++pos;
        if (pos + name_len >= payload.size()) {
            delete headers;
            return GALAY_PROTOCOL_ERROR;
        }
        std::string name;
        name.reserve(name_len);
        for (size_t i = 0; i < name_len; ++i) {
            name.push_back(static_cast<char>(payload[pos + i]));
        }
        pos += name_len;

        const size_t value_len = payload[pos];
        ++pos;
        if (pos + value_len > payload.size()) {
            delete headers;
            return GALAY_PROTOCOL_ERROR;
        }
        std::string value;
        value.reserve(value_len);
        for (size_t i = 0; i < value_len; ++i) {
            value.push_back(static_cast<char>(payload[pos + i]));
        }
        pos += value_len;
        headers->entries.push_back({std::move(name), std::move(value)});
    }

    *out = headers;
    return GALAY_OK;
}

C_IOResult send_settings(galay_http2_conn_t* conn, const galay_http2_config_t& config, int64_t timeout_ms)
{
    auto payload = make_settings_payload(config);
    return send_frame(conn, GALAY_HTTP2_FRAME_SETTINGS, 0, 0, payload, timeout_ms);
}

C_IOResult send_settings_ack(galay_http2_conn_t* conn, int64_t timeout_ms)
{
    const std::vector<uint8_t> payload;
    return send_frame(conn, GALAY_HTTP2_FRAME_SETTINGS, kFlagAck, 0, payload, timeout_ms);
}

C_IOResult apply_peer_settings(galay_http2_conn_t* conn, const RawFrame& frame)
{
    if (conn == nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    for (size_t pos = 0; pos < frame.payload.size(); pos += 6) {
        const auto id = static_cast<galay_http2_settings_id_t>(
            (static_cast<uint16_t>(frame.payload[pos]) << 8U) | frame.payload[pos + 1]);
        const uint32_t value = read_u32(frame.payload, pos + 2);
        galay_status_t status = galay_http2_settings_value_validate(id, value);
        if (status != GALAY_OK) {
            return result(C_IOResultError, GALAY_HTTP2_ERROR_PROTOCOL);
        }
        if (id == GALAY_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE) {
            const int32_t old_window = static_cast<int32_t>(conn->peer_initial_window);
            conn->peer_initial_window = value;
            const int32_t delta = static_cast<int32_t>(value) - old_window;
            for (auto* stream : conn->streams) {
                if (stream != nullptr) {
                    stream->send_window += delta;
                }
            }
        } else if (id == GALAY_HTTP2_SETTINGS_MAX_FRAME_SIZE) {
            conn->peer_max_frame_size = value;
        }
    }
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
}

C_IOResult process_frame(galay_http2_conn_t* conn, RawFrame&& frame, int64_t timeout_ms,
                         galay_http2_stream_t** accepted_stream = nullptr)
{
    if (conn == nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }

    switch (frame.type) {
    case GALAY_HTTP2_FRAME_SETTINGS:
        if ((frame.flags & kFlagAck) != 0) {
            conn->settings_ack_received = true;
            return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
        }
        {
            C_IOResult applied = apply_peer_settings(conn, frame);
            if (applied.code != C_IOResultOk) {
                return applied;
            }
            C_IOResult acked = send_settings_ack(conn, timeout_ms);
            if (acked.code != C_IOResultOk) {
                return acked;
            }
            return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
        }
    case GALAY_HTTP2_FRAME_WINDOW_UPDATE:
        {
            const uint32_t increment = read_u32(frame.payload, 0) & 0x7FFFFFFFu;
            if (frame.stream_id == 0) {
                if (static_cast<int64_t>(conn->conn_send_window) + increment > kMaxWindow) {
                    return result(C_IOResultError, GALAY_HTTP2_ERROR_FLOW_CONTROL);
                }
                conn->conn_send_window += static_cast<int32_t>(increment);
                return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
            }
            auto* stream = find_stream(conn, frame.stream_id);
            if (stream == nullptr) {
                return result(C_IOResultError, GALAY_HTTP2_ERROR_STREAM_CLOSED);
            }
            if (static_cast<int64_t>(stream->send_window) + increment > kMaxWindow) {
                return result(C_IOResultError, GALAY_HTTP2_ERROR_FLOW_CONTROL);
            }
            stream->send_window += static_cast<int32_t>(increment);
            return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
        }
    case GALAY_HTTP2_FRAME_RST_STREAM:
        {
            auto* stream = find_stream(conn, frame.stream_id);
            if (stream != nullptr) {
                stream->reset_received = true;
                stream->state = GALAY_HTTP2_STREAM_CLOSED;
            }
            return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
        }
    case GALAY_HTTP2_FRAME_GOAWAY:
        conn->goaway_received = true;
        conn->goaway_last_stream_id = read_u32(frame.payload, 0) & 0x7FFFFFFFu;
        for (auto* stream : conn->streams) {
            if (stream != nullptr && stream->id > conn->goaway_last_stream_id) {
                stream->state = GALAY_HTTP2_STREAM_CLOSED;
            }
        }
        return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
    case GALAY_HTTP2_FRAME_HEADERS:
        {
            galay_http2_headers_t* headers = nullptr;
            galay_status_t decoded = decode_headers_payload(frame.payload, &headers);
            if (decoded != GALAY_OK) {
                return result(C_IOResultError, GALAY_HTTP2_ERROR_PROTOCOL);
            }
            auto* stream = find_stream(conn, frame.stream_id);
            if (stream == nullptr) {
                const bool end = (frame.flags & kFlagEndStream) != 0;
                stream = create_stream(conn,
                                       frame.stream_id,
                                       end ? GALAY_HTTP2_STREAM_HALF_CLOSED_REMOTE
                                           : GALAY_HTTP2_STREAM_OPEN);
                if (stream == nullptr) {
                    delete headers;
                    return result(C_IOResultError, GALAY_HTTP2_ERROR_INTERNAL);
                }
                if (accepted_stream != nullptr) {
                    *accepted_stream = stream;
                }
            } else if (!can_receive_on_stream(stream)) {
                delete headers;
                return result(C_IOResultError, GALAY_HTTP2_ERROR_STREAM_CLOSED);
            } else if ((frame.flags & kFlagEndStream) != 0) {
                mark_remote_end(stream);
            }
            stream->pending_headers.push_back(headers);
            return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
        }
    case GALAY_HTTP2_FRAME_DATA:
        {
            auto* stream = find_stream(conn, frame.stream_id);
            if (!can_receive_on_stream(stream)) {
                return result(C_IOResultError, GALAY_HTTP2_ERROR_STREAM_CLOSED);
            }
            const size_t data_size = frame.payload.size();
            if (data_size > static_cast<size_t>(std::max(conn->conn_recv_window, 0)) ||
                data_size > static_cast<size_t>(std::max(stream->recv_window, 0))) {
                stream->state = GALAY_HTTP2_STREAM_CLOSED;
                return result(C_IOResultError, GALAY_HTTP2_ERROR_FLOW_CONTROL);
            }
            conn->conn_recv_window -= static_cast<int32_t>(data_size);
            stream->recv_window -= static_cast<int32_t>(data_size);
            DataChunk chunk;
            chunk.bytes.resize(frame.payload.size());
            for (size_t i = 0; i < frame.payload.size(); ++i) {
                chunk.bytes[i] = static_cast<char>(frame.payload[i]);
            }
            chunk.end_stream = (frame.flags & kFlagEndStream) != 0;
            if (chunk.end_stream) {
                mark_remote_end(stream);
            }
            stream->pending_data.push_back(std::move(chunk));
            return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
        }
    case GALAY_HTTP2_FRAME_PING:
        if ((frame.flags & kFlagAck) == 0) {
            C_IOResult pong = send_frame(conn,
                                         GALAY_HTTP2_FRAME_PING,
                                         kFlagAck,
                                         0,
                                         frame.payload,
                                         timeout_ms);
            if (pong.code != C_IOResultOk) {
                return pong;
            }
        }
        return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
    }
    return result(C_IOResultError, GALAY_HTTP2_ERROR_PROTOCOL);
}

C_IOResult read_and_process(galay_http2_conn_t* conn, int64_t timeout_ms,
                            galay_http2_stream_t** accepted_stream = nullptr)
{
    RawFrame frame;
    C_IOResult read = read_frame(conn, frame, timeout_ms);
    if (read.code != C_IOResultOk) {
        return read;
    }
    return process_frame(conn, std::move(frame), timeout_ms, accepted_stream);
}

C_IOResult complete_client_handshake(galay_http2_conn_t* conn,
                                     const galay_http2_config_t& config,
                                     int64_t timeout_ms)
{
    C_IOResult preface = send_all(conn, kClientPreface, kClientPrefaceLength, timeout_ms);
    if (preface.code != C_IOResultOk) {
        return preface;
    }
    C_IOResult settings = send_settings(conn, config, timeout_ms);
    if (settings.code != C_IOResultOk) {
        return settings;
    }

    bool saw_peer_settings = false;
    while (!saw_peer_settings || !conn->settings_ack_received) {
        RawFrame frame;
        C_IOResult read = read_frame(conn, frame, timeout_ms);
        if (read.code != C_IOResultOk) {
            return read;
        }
        if (frame.type == GALAY_HTTP2_FRAME_SETTINGS && (frame.flags & kFlagAck) == 0) {
            saw_peer_settings = true;
        }
        C_IOResult processed = process_frame(conn, std::move(frame), timeout_ms);
        if (processed.code != C_IOResultOk) {
            return processed;
        }
    }
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
}

C_IOResult complete_server_handshake(galay_http2_conn_t* conn,
                                     const galay_http2_config_t& config,
                                     int64_t timeout_ms)
{
    char preface[kClientPrefaceLength] = {};
    C_IOResult preface_result = recv_exact(conn, preface, sizeof(preface), timeout_ms);
    if (preface_result.code != C_IOResultOk) {
        return preface_result;
    }
    for (size_t i = 0; i < sizeof(preface); ++i) {
        if (preface[i] != kClientPreface[i]) {
            return result(C_IOResultError, GALAY_HTTP2_ERROR_PROTOCOL);
        }
    }

    bool saw_peer_settings = false;
    while (!saw_peer_settings) {
        RawFrame frame;
        C_IOResult read = read_frame(conn, frame, timeout_ms);
        if (read.code != C_IOResultOk) {
            return read;
        }
        if (frame.type == GALAY_HTTP2_FRAME_SETTINGS && (frame.flags & kFlagAck) == 0) {
            saw_peer_settings = true;
        }
        C_IOResult processed = process_frame(conn, std::move(frame), timeout_ms);
        if (processed.code != C_IOResultOk) {
            return processed;
        }
    }

    C_IOResult settings = send_settings(conn, config, timeout_ms);
    if (settings.code != C_IOResultOk) {
        return settings;
    }
    while (!conn->settings_ack_received) {
        C_IOResult processed = read_and_process(conn, timeout_ms);
        if (processed.code != C_IOResultOk) {
            return processed;
        }
    }
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
}

} // namespace

extern "C" {

const char* galay_http2_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

const char* galay_http2_error_code_get_error(galay_http2_error_code_t error)
{
    switch (error) {
    case GALAY_HTTP2_ERROR_NONE:
        return "no http2 error";
    case GALAY_HTTP2_ERROR_PROTOCOL:
        return "http2 protocol error";
    case GALAY_HTTP2_ERROR_INTERNAL:
        return "http2 internal error";
    case GALAY_HTTP2_ERROR_FLOW_CONTROL:
        return "http2 flow control error";
    case GALAY_HTTP2_ERROR_STREAM_CLOSED:
        return "http2 stream closed";
    case GALAY_HTTP2_ERROR_CANCEL:
        return "http2 cancel";
    case GALAY_HTTP2_ERROR_SETTINGS_ACK:
        return "http2 settings ack error";
    case GALAY_HTTP2_ERROR_STREAM_RESET:
        return "http2 stream reset";
    case GALAY_HTTP2_ERROR_GOAWAY:
        return "http2 goaway received";
    case GALAY_HTTP2_ERROR_IO:
        return "http2 io error";
    case GALAY_HTTP2_ERROR_TIMEOUT:
        return "http2 timeout";
    }
    return "unknown http2 error";
}

galay_status_t galay_http2_stream_id_validate(uint32_t stream_id, galay_bool_t allow_zero)
{
    if ((stream_id & 0x80000000u) != 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (stream_id == 0 && allow_zero != GALAY_TRUE) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_OK;
}

galay_status_t galay_http2_settings_value_validate(galay_http2_settings_id_t id, uint32_t value)
{
    if (id == GALAY_HTTP2_SETTINGS_ENABLE_PUSH && value > 1) {
        return GALAY_PROTOCOL_ERROR;
    }
    if (id == GALAY_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE && value > kMaxWindow) {
        return GALAY_PROTOCOL_ERROR;
    }
    if (id == GALAY_HTTP2_SETTINGS_MAX_FRAME_SIZE && (value < 16384u || value > 16777215u)) {
        return GALAY_PROTOCOL_ERROR;
    }
    return GALAY_OK;
}

galay_status_t galay_http2_ping_frame_create(const uint8_t opaque[8], galay_bool_t ack,
                                             galay_http2_frame_t** out)
{
    if (opaque == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* frame = new (std::nothrow) galay_http2_frame_t();
    if (frame == nullptr) {
        *out = nullptr;
        return GALAY_OUT_OF_MEMORY;
    }
    frame->type = GALAY_HTTP2_FRAME_PING;
    frame->flags = ack == GALAY_TRUE ? kFlagAck : 0x0;
    frame->payload.assign(opaque, opaque + 8);
    *out = frame;
    return GALAY_OK;
}

void galay_http2_frame_destroy(galay_http2_frame_t* frame)
{
    delete frame;
}

galay_status_t galay_http2_frame_encode(const galay_http2_frame_t* frame, uint8_t* out,
                                        size_t* out_len)
{
    if (frame == nullptr || out_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const size_t need = GALAY_HTTP2_FRAME_HEADER_LENGTH + frame->payload.size();
    if (out == nullptr || *out_len < need) {
        *out_len = need;
        return GALAY_OUT_OF_MEMORY;
    }
    const uint32_t len = static_cast<uint32_t>(frame->payload.size());
    out[0] = static_cast<uint8_t>((len >> 16U) & 0xFFU);
    out[1] = static_cast<uint8_t>((len >> 8U) & 0xFFU);
    out[2] = static_cast<uint8_t>(len & 0xFFU);
    out[3] = static_cast<uint8_t>(frame->type);
    out[4] = frame->flags;
    out[5] = static_cast<uint8_t>((frame->stream_id >> 24U) & 0x7FU);
    out[6] = static_cast<uint8_t>((frame->stream_id >> 16U) & 0xFFU);
    out[7] = static_cast<uint8_t>((frame->stream_id >> 8U) & 0xFFU);
    out[8] = static_cast<uint8_t>(frame->stream_id & 0xFFU);
    for (size_t i = 0; i < frame->payload.size(); ++i) {
        out[GALAY_HTTP2_FRAME_HEADER_LENGTH + i] = frame->payload[i];
    }
    *out_len = need;
    return GALAY_OK;
}

galay_status_t galay_http2_frame_decode(const uint8_t* data, size_t data_len,
                                        galay_http2_frame_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (data == nullptr || out == nullptr || data_len < GALAY_HTTP2_FRAME_HEADER_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    const uint32_t len = (static_cast<uint32_t>(data[0]) << 16U) |
        (static_cast<uint32_t>(data[1]) << 8U) | data[2];
    if (data_len < GALAY_HTTP2_FRAME_HEADER_LENGTH + len) {
        return GALAY_PROTOCOL_ERROR;
    }
    RawFrame view;
    view.type = static_cast<galay_http2_frame_type_t>(data[3]);
    view.flags = data[4];
    view.stream_id = (static_cast<uint32_t>(data[5] & 0x7FU) << 24U) |
        (static_cast<uint32_t>(data[6]) << 16U) |
        (static_cast<uint32_t>(data[7]) << 8U) | data[8];
    view.payload.assign(data + GALAY_HTTP2_FRAME_HEADER_LENGTH,
                        data + GALAY_HTTP2_FRAME_HEADER_LENGTH + len);
    if (validate_raw_frame(view) != GALAY_OK) {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* frame = new (std::nothrow) galay_http2_frame_t();
    if (frame == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    frame->type = view.type;
    frame->flags = view.flags;
    frame->stream_id = view.stream_id;
    frame->payload = std::move(view.payload);
    *out = frame;
    return GALAY_OK;
}

galay_http2_frame_type_t galay_http2_frame_type(const galay_http2_frame_t* frame)
{
    return frame == nullptr ? GALAY_HTTP2_FRAME_DATA : frame->type;
}

uint32_t galay_http2_frame_stream_id(const galay_http2_frame_t* frame)
{
    return frame == nullptr ? 0 : frame->stream_id;
}

galay_status_t galay_http2_ping_frame_opaque(const galay_http2_frame_t* frame, uint8_t out[8])
{
    if (frame == nullptr || out == nullptr || frame->type != GALAY_HTTP2_FRAME_PING ||
        frame->payload.size() != 8) {
        return GALAY_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < 8; ++i) {
        out[i] = frame->payload[i];
    }
    return GALAY_OK;
}

galay_status_t galay_http2_headers_create(galay_http2_headers_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_http2_headers_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_http2_headers_destroy(galay_http2_headers_t* headers)
{
    delete headers;
}

galay_status_t galay_http2_headers_add(galay_http2_headers_t* headers, const char* name,
                                       const char* value)
{
    if (headers == nullptr || name == nullptr || value == nullptr || name[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    headers->entries.push_back({name, value});
    return GALAY_OK;
}

size_t galay_http2_headers_count(const galay_http2_headers_t* headers)
{
    return headers == nullptr ? 0 : headers->entries.size();
}

galay_status_t galay_http2_headers_get(const galay_http2_headers_t* headers, size_t index,
                                       const char** name, const char** value)
{
    if (headers == nullptr || name == nullptr || value == nullptr || index >= headers->entries.size()) {
        return GALAY_INVALID_ARGUMENT;
    }
    *name = headers->entries[index].name.c_str();
    *value = headers->entries[index].value.c_str();
    return GALAY_OK;
}

galay_status_t galay_http2_hpack_encode(const galay_http2_headers_t* headers, uint8_t* out,
                                        size_t* out_len)
{
    if (headers == nullptr || out_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::vector<uint8_t> payload;
    galay_status_t encoded = encode_headers_payload(headers, payload);
    if (encoded != GALAY_OK) {
        return encoded;
    }
    if (out == nullptr || *out_len < payload.size()) {
        *out_len = payload.size();
        return GALAY_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < payload.size(); ++i) {
        out[i] = payload[i];
    }
    *out_len = payload.size();
    return GALAY_OK;
}

galay_status_t galay_http2_hpack_decode(const uint8_t* data, size_t data_len,
                                        galay_http2_headers_t** out)
{
    if (data == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::vector<uint8_t> payload;
    payload.assign(data, data + data_len);
    return decode_headers_payload(payload, out);
}

galay_http2_config_t galay_http2_config_default(void)
{
    return galay_http2_config_t{
        "127.0.0.1",
        0,
        kDefaultInitialWindow,
        kDefaultMaxFrameSize,
        100,
    };
}

galay_status_t galay_http2_client_create(const galay_http2_config_t* config,
                                         galay_http2_client_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* client = new (std::nothrow) galay_http2_client_t();
    if (client == nullptr) {
        *out = nullptr;
        return GALAY_OUT_OF_MEMORY;
    }
    client->config = normalize_config(config);
    client->host = client->config.host == nullptr ? "127.0.0.1" : client->config.host;
    client->config.host = client->host.c_str();
    *out = client;
    return GALAY_OK;
}

void galay_http2_client_destroy(galay_http2_client_t* client)
{
    if (client == nullptr) {
        return;
    }
    if (client->conn != nullptr) {
        galay_http2_conn_destroy(client->conn);
        client->conn = nullptr;
    }
    delete client;
}

C_IOResult galay_http2_client_connect(galay_http2_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || client->conn != nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    auto* conn = new (std::nothrow) galay_http2_conn_t();
    if (conn == nullptr) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_INTERNAL);
    }
    conn->is_client = true;
    conn->owns_socket = true;
    conn->local_initial_window = client->config.initial_window_size;
    conn->local_max_frame_size = client->config.max_frame_size;
    conn->max_concurrent_streams = client->config.max_concurrent_streams;
    conn->conn_recv_window = static_cast<int32_t>(conn->local_initial_window);

    C_TcpSocketResultCode created = galay_kernel_tcp_socket_create(&conn->socket, C_IPTypeIPV4);
    if (created != C_TcpSocketSuccess) {
        delete conn;
        return result(C_IOResultError, GALAY_HTTP2_ERROR_IO);
    }
    C_Host host{};
    copy_host(host, client->config.host, client->config.port);
    C_IOResult connected = galay_kernel_tcp_socket_connect(&conn->socket, &host, timeout_ms);
    if (connected.code != C_IOResultOk) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&conn->socket);
        delete conn;
        if (destroyed != C_TcpSocketSuccess) {
            return result(C_IOResultError, GALAY_HTTP2_ERROR_IO);
        }
        return map_io_result(connected);
    }
    C_IOResult handshake = complete_client_handshake(conn, client->config, timeout_ms);
    if (handshake.code != C_IOResultOk) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&conn->socket);
        delete conn;
        if (destroyed != C_TcpSocketSuccess) {
            return result(C_IOResultError, GALAY_HTTP2_ERROR_IO);
        }
        return handshake;
    }
    client->conn = conn;
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
}

galay_http2_conn_t* galay_http2_client_conn(galay_http2_client_t* client)
{
    return client == nullptr ? nullptr : client->conn;
}

C_IOResult galay_http2_client_open_stream(galay_http2_client_t* client,
                                          const galay_http2_headers_t* headers,
                                          galay_bool_t end_stream,
                                          galay_http2_stream_t** out_stream,
                                          int64_t timeout_ms)
{
    if (out_stream != nullptr) {
        *out_stream = nullptr;
    }
    if (client == nullptr || client->conn == nullptr || headers == nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    auto* conn = client->conn;
    if (conn->goaway_received) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_GOAWAY);
    }
    const uint32_t stream_id = conn->next_stream_id;
    conn->next_stream_id += 2;
    auto* stream = create_stream(conn,
                                 stream_id,
                                 end_stream == GALAY_TRUE ? GALAY_HTTP2_STREAM_HALF_CLOSED_LOCAL
                                                          : GALAY_HTTP2_STREAM_OPEN);
    if (stream == nullptr) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_INTERNAL);
    }

    std::vector<uint8_t> payload;
    galay_status_t encoded = encode_headers_payload(headers, payload);
    if (encoded != GALAY_OK) {
        forget_stream(stream);
        delete stream;
        return result(C_IOResultError, GALAY_HTTP2_ERROR_PROTOCOL);
    }
    const uint8_t flags = static_cast<uint8_t>(kFlagEndHeaders | (end_stream == GALAY_TRUE ? kFlagEndStream : 0));
    C_IOResult sent = send_frame(conn, GALAY_HTTP2_FRAME_HEADERS, flags, stream_id, payload, timeout_ms);
    if (sent.code != C_IOResultOk) {
        forget_stream(stream);
        delete stream;
        return sent;
    }
    if (out_stream != nullptr) {
        *out_stream = stream;
    }
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
}

galay_status_t galay_http2_server_create(const galay_http2_config_t* config,
                                         galay_http2_server_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* server = new (std::nothrow) galay_http2_server_t();
    if (server == nullptr) {
        *out = nullptr;
        return GALAY_OUT_OF_MEMORY;
    }
    server->config = normalize_config(config);
    server->host = server->config.host == nullptr ? "127.0.0.1" : server->config.host;
    server->config.host = server->host.c_str();
    *out = server;
    return GALAY_OK;
}

void galay_http2_server_destroy(galay_http2_server_t* server)
{
    if (server == nullptr) {
        return;
    }
    if (server->listener.socket != nullptr) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&server->listener);
        if (destroyed == C_TcpSocketSuccess) {
            server->listening = false;
        }
    }
    delete server;
}

C_IOResult galay_http2_server_listen(galay_http2_server_t* server, uint16_t* out_port)
{
    if (server == nullptr || server->listener.socket != nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    C_TcpSocketResultCode created =
        galay_kernel_tcp_socket_create(&server->listener, C_IPTypeIPV4);
    if (created != C_TcpSocketSuccess) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_IO);
    }
    C_Host bind_host{};
    copy_host(bind_host, server->config.host, server->config.port);
    if (galay_kernel_tcp_socket_bind(&server->listener, &bind_host) != C_TcpSocketSuccess ||
        galay_kernel_tcp_socket_listen(&server->listener, 128) != C_TcpSocketSuccess) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&server->listener);
        if (destroyed != C_TcpSocketSuccess) {
            return result(C_IOResultError, GALAY_HTTP2_ERROR_IO);
        }
        return result(C_IOResultError, GALAY_HTTP2_ERROR_IO);
    }
    C_Host local{};
    if (galay_kernel_tcp_socket_local_endpoint(&server->listener, &local) != C_TcpSocketSuccess ||
        local.port == 0) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&server->listener);
        if (destroyed != C_TcpSocketSuccess) {
            return result(C_IOResultError, GALAY_HTTP2_ERROR_IO);
        }
        return result(C_IOResultError, GALAY_HTTP2_ERROR_IO);
    }
    server->listening = true;
    if (out_port != nullptr) {
        *out_port = local.port;
    }
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE, 0);
}

C_IOResult galay_http2_server_accept(galay_http2_server_t* server,
                                     galay_http2_conn_t** out_conn,
                                     int64_t timeout_ms)
{
    if (out_conn != nullptr) {
        *out_conn = nullptr;
    }
    if (server == nullptr || !server->listening || out_conn == nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    auto* conn = new (std::nothrow) galay_http2_conn_t();
    if (conn == nullptr) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_INTERNAL);
    }
    conn->owns_socket = true;
    conn->local_initial_window = server->config.initial_window_size;
    conn->local_max_frame_size = server->config.max_frame_size;
    conn->max_concurrent_streams = server->config.max_concurrent_streams;
    conn->conn_recv_window = static_cast<int32_t>(conn->local_initial_window);
    C_IOResult accepted =
        galay_kernel_tcp_socket_accept(&server->listener, &conn->socket, nullptr, timeout_ms);
    if (accepted.code != C_IOResultOk) {
        delete conn;
        return map_io_result(accepted);
    }
    C_IOResult handshake = complete_server_handshake(conn, server->config, timeout_ms);
    if (handshake.code != C_IOResultOk) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&conn->socket);
        delete conn;
        if (destroyed != C_TcpSocketSuccess) {
            return result(C_IOResultError, GALAY_HTTP2_ERROR_IO);
        }
        return handshake;
    }
    *out_conn = conn;
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
}

C_IOResult galay_http2_server_stop(galay_http2_server_t* server, int64_t timeout_ms)
{
    if (server == nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    if (timeout_ms < -1) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    if (server->listener.socket == nullptr) {
        server->listening = false;
        return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
    }
    C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&server->listener);
    if (destroyed != C_TcpSocketSuccess) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_IO);
    }
    server->listening = false;
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
}

galay_status_t galay_http2_conn_destroy(galay_http2_conn_t* conn)
{
    if (conn == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    while (!conn->streams.empty()) {
        auto* stream = conn->streams.back();
        conn->streams.pop_back();
        destroy_pending(stream);
        delete stream;
    }
    if (conn->owns_socket && conn->socket.socket != nullptr) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&conn->socket);
        if (destroyed != C_TcpSocketSuccess) {
            delete conn;
            return GALAY_IO_ERROR;
        }
    }
    delete conn;
    return GALAY_OK;
}

galay_bool_t galay_http2_conn_settings_ack_received(const galay_http2_conn_t* conn)
{
    return conn != nullptr && conn->settings_ack_received ? GALAY_TRUE : GALAY_FALSE;
}

C_IOResult galay_http2_conn_accept_stream(galay_http2_conn_t* conn,
                                          galay_http2_stream_t** out_stream,
                                          int64_t timeout_ms)
{
    if (out_stream != nullptr) {
        *out_stream = nullptr;
    }
    if (conn == nullptr || out_stream == nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    while (*out_stream == nullptr) {
        C_IOResult processed = read_and_process(conn, timeout_ms, out_stream);
        if (processed.code != C_IOResultOk) {
            return processed;
        }
    }
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
}

C_IOResult galay_http2_conn_read_control(galay_http2_conn_t* conn, int64_t timeout_ms)
{
    if (conn == nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    for (;;) {
        RawFrame frame;
        C_IOResult read = read_frame(conn, frame, timeout_ms);
        if (read.code != C_IOResultOk) {
            return read;
        }
        const bool control = frame.type == GALAY_HTTP2_FRAME_SETTINGS ||
            frame.type == GALAY_HTTP2_FRAME_WINDOW_UPDATE ||
            frame.type == GALAY_HTTP2_FRAME_GOAWAY ||
            frame.type == GALAY_HTTP2_FRAME_RST_STREAM ||
            frame.type == GALAY_HTTP2_FRAME_PING;
        C_IOResult processed = process_frame(conn, std::move(frame), timeout_ms);
        if (processed.code != C_IOResultOk) {
            return processed;
        }
        if (control) {
            return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
        }
    }
}

C_IOResult galay_http2_conn_send_goaway(galay_http2_conn_t* conn,
                                        uint32_t last_stream_id,
                                        galay_http2_error_code_t error,
                                        int64_t timeout_ms)
{
    if (conn == nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    std::vector<uint8_t> payload;
    payload.reserve(8);
    append_u32(payload, last_stream_id & 0x7FFFFFFFu);
    append_u32(payload, static_cast<uint32_t>(error));
    return send_frame(conn, GALAY_HTTP2_FRAME_GOAWAY, 0, 0, payload, timeout_ms);
}

C_IOResult galay_http2_conn_send_window_update(galay_http2_conn_t* conn,
                                               galay_http2_stream_t* stream,
                                               uint32_t increment,
                                               int64_t timeout_ms)
{
    if (conn == nullptr || increment == 0 || increment > kMaxWindow) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    if (static_cast<int64_t>(conn->conn_recv_window) + increment > kMaxWindow) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_FLOW_CONTROL);
    }
    conn->conn_recv_window += static_cast<int32_t>(increment);
    uint32_t stream_id = 0;
    if (stream != nullptr) {
        if (stream->conn != conn ||
            static_cast<int64_t>(stream->recv_window) + increment > kMaxWindow) {
            return result(C_IOResultError, GALAY_HTTP2_ERROR_FLOW_CONTROL);
        }
        stream->recv_window += static_cast<int32_t>(increment);
        stream_id = stream->id;
    }
    std::vector<uint8_t> payload;
    append_u32(payload, increment);
    return send_frame(conn, GALAY_HTTP2_FRAME_WINDOW_UPDATE, 0, stream_id, payload, timeout_ms);
}

galay_status_t galay_http2_stream_destroy(galay_http2_stream_t* stream)
{
    if (stream == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    forget_stream(stream);
    destroy_pending(stream);
    delete stream;
    return GALAY_OK;
}

uint32_t galay_http2_stream_id(const galay_http2_stream_t* stream)
{
    return stream == nullptr ? 0 : stream->id;
}

galay_http2_stream_state_t galay_http2_stream_state(const galay_http2_stream_t* stream)
{
    return stream == nullptr ? GALAY_HTTP2_STREAM_CLOSED : stream->state;
}

C_IOResult galay_http2_stream_read_headers(galay_http2_stream_t* stream,
                                           galay_http2_headers_t** out_headers,
                                           int64_t timeout_ms)
{
    if (out_headers != nullptr) {
        *out_headers = nullptr;
    }
    if (stream == nullptr || stream->conn == nullptr || out_headers == nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    while (stream->pending_headers.empty()) {
        if (stream->reset_received) {
            return result(C_IOResultError, GALAY_HTTP2_ERROR_STREAM_RESET);
        }
        C_IOResult processed = read_and_process(stream->conn, timeout_ms);
        if (processed.code != C_IOResultOk) {
            return processed;
        }
    }
    *out_headers = stream->pending_headers.front();
    stream->pending_headers.pop_front();
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
}

C_IOResult galay_http2_stream_write_headers(galay_http2_stream_t* stream,
                                            const galay_http2_headers_t* headers,
                                            galay_bool_t end_stream,
                                            int64_t timeout_ms)
{
    if (stream == nullptr || stream->conn == nullptr || headers == nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    if (!can_send_on_stream(stream)) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_STREAM_CLOSED);
    }
    std::vector<uint8_t> payload;
    galay_status_t encoded = encode_headers_payload(headers, payload);
    if (encoded != GALAY_OK) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_PROTOCOL);
    }
    const uint8_t flags = static_cast<uint8_t>(kFlagEndHeaders | (end_stream == GALAY_TRUE ? kFlagEndStream : 0));
    C_IOResult sent =
        send_frame(stream->conn, GALAY_HTTP2_FRAME_HEADERS, flags, stream->id, payload, timeout_ms);
    if (sent.code == C_IOResultOk && end_stream == GALAY_TRUE) {
        mark_local_end(stream);
    }
    return sent.code == C_IOResultOk ? result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE) : sent;
}

C_IOResult galay_http2_stream_read_data(galay_http2_stream_t* stream,
                                        char* out,
                                        size_t out_len,
                                        size_t* read_len,
                                        galay_bool_t* end_stream,
                                        int64_t timeout_ms)
{
    if (read_len != nullptr) {
        *read_len = 0;
    }
    if (end_stream != nullptr) {
        *end_stream = GALAY_FALSE;
    }
    if (stream == nullptr || stream->conn == nullptr || out == nullptr || read_len == nullptr ||
        end_stream == nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    while (stream->pending_data.empty()) {
        if (stream->reset_received) {
            return result(C_IOResultError, GALAY_HTTP2_ERROR_STREAM_RESET);
        }
        C_IOResult processed = read_and_process(stream->conn, timeout_ms);
        if (processed.code != C_IOResultOk) {
            return processed;
        }
    }
    DataChunk chunk = std::move(stream->pending_data.front());
    stream->pending_data.pop_front();
    if (out_len < chunk.bytes.size()) {
        stream->pending_data.push_front(std::move(chunk));
        return result(C_IOResultError, GALAY_HTTP2_ERROR_INTERNAL);
    }
    for (size_t i = 0; i < chunk.bytes.size(); ++i) {
        out[i] = chunk.bytes[i];
    }
    *read_len = chunk.bytes.size();
    *end_stream = chunk.end_stream ? GALAY_TRUE : GALAY_FALSE;
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE, chunk.bytes.size());
}

C_IOResult galay_http2_stream_write_data(galay_http2_stream_t* stream,
                                         const char* data,
                                         size_t data_len,
                                         galay_bool_t end_stream,
                                         int64_t timeout_ms)
{
    if (stream == nullptr || stream->conn == nullptr || (data == nullptr && data_len > 0)) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    if (!can_send_on_stream(stream)) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_STREAM_CLOSED);
    }
    auto* conn = stream->conn;
    if (data_len > static_cast<size_t>(std::max(conn->conn_send_window, 0)) ||
        data_len > static_cast<size_t>(std::max(stream->send_window, 0))) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_FLOW_CONTROL);
    }
    std::vector<uint8_t> payload;
    payload.resize(data_len);
    for (size_t i = 0; i < data_len; ++i) {
        payload[i] = static_cast<uint8_t>(data[i]);
    }
    const uint8_t flags = end_stream == GALAY_TRUE ? kFlagEndStream : 0;
    C_IOResult sent = send_frame(conn, GALAY_HTTP2_FRAME_DATA, flags, stream->id, payload, timeout_ms);
    if (sent.code != C_IOResultOk) {
        return sent;
    }
    conn->conn_send_window -= static_cast<int32_t>(data_len);
    stream->send_window -= static_cast<int32_t>(data_len);
    if (end_stream == GALAY_TRUE) {
        mark_local_end(stream);
    }
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE, data_len);
}

C_IOResult galay_http2_stream_reset(galay_http2_stream_t* stream,
                                    galay_http2_error_code_t error,
                                    int64_t timeout_ms)
{
    if (stream == nullptr || stream->conn == nullptr) {
        return result(C_IOResultInvalid, GALAY_HTTP2_ERROR_INTERNAL);
    }
    if (stream->state == GALAY_HTTP2_STREAM_CLOSED && stream->reset_sent) {
        return result(C_IOResultError, GALAY_HTTP2_ERROR_STREAM_CLOSED);
    }
    std::vector<uint8_t> payload;
    append_u32(payload, static_cast<uint32_t>(error));
    C_IOResult sent =
        send_frame(stream->conn, GALAY_HTTP2_FRAME_RST_STREAM, 0, stream->id, payload, timeout_ms);
    if (sent.code != C_IOResultOk) {
        return sent;
    }
    stream->reset_sent = true;
    stream->state = GALAY_HTTP2_STREAM_CLOSED;
    return result(C_IOResultOk, GALAY_HTTP2_ERROR_NONE);
}

}

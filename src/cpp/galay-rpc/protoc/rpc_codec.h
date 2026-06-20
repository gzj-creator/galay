/**
 * @file rpc_codec.h
 * @brief RPC消息编解码器
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供RPC消息头部、请求和响应的静态编解码方法，
 *          支持渐进式解码（先解码头部判断完整性，再解码完整消息）。
 */

#ifndef GALAY_RPC_CODEC_H
#define GALAY_RPC_CODEC_H

#include "rpc_message.h"
#include "rpc_error.h"
#include <expected>
#include <variant>

namespace galay::rpc
{

/**
 * @brief 解码结果
 */
enum class DecodeResult {
    COMPLETE,       ///< 解码完成
    INCOMPLETE,     ///< 数据不完整，需要更多数据
    ERROR,          ///< 解码错误
};

/**
 * @brief RPC编解码器
 */
class RpcCodec {
public:
    /**
     * @brief 解码消息头
     * @param data 数据缓冲区
     * @param length 数据长度
     * @param header 输出消息头
     * @return 解码结果
     */
    static DecodeResult decodeHeader(const char* data, size_t length, RpcHeader& header) {
        if (length < RPC_HEADER_SIZE) {
            return DecodeResult::INCOMPLETE;
        }

        if (!header.deserialize(data)) {
            return DecodeResult::ERROR;
        }

        return DecodeResult::COMPLETE;
    }

    /**
     * @brief 解码请求消息
     * @param data 完整消息数据（包含头部）
     * @param length 数据长度
     * @return 解码后的请求或错误
     */
    static std::expected<RpcRequest, RpcError> decodeRequest(const char* data, size_t length) {
        RpcHeader header;
        auto result = decodeHeader(data, length, header);

        if (result == DecodeResult::INCOMPLETE) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Incomplete data"));
        }

        if (result == DecodeResult::ERROR) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Invalid header"));
        }

        if (header.m_type != static_cast<uint8_t>(RpcMessageType::REQUEST)) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Not a request message"));
        }

        if (length < RPC_HEADER_SIZE + header.m_body_length) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Incomplete body"));
        }

        RpcRequest request;
        request.requestId(header.m_request_id);
        request.callMode(rpcDecodeCallMode(header.m_flags));
        request.endOfStream(rpcIsEndStream(header.m_flags));

        if (!request.deserializeBody(data + RPC_HEADER_SIZE, header.m_body_length)) {
            return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Failed to parse request body"));
        }

        return request;
    }

    /**
     * @brief 解码响应消息
     * @param data 完整消息数据（包含头部）
     * @param length 数据长度
     * @return 解码后的响应或错误
     */
    static std::expected<RpcResponse, RpcError> decodeResponse(const char* data, size_t length) {
        RpcHeader header;
        auto result = decodeHeader(data, length, header);

        if (result == DecodeResult::INCOMPLETE) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Incomplete data"));
        }

        if (result == DecodeResult::ERROR) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Invalid header"));
        }

        if (header.m_type != static_cast<uint8_t>(RpcMessageType::RESPONSE)) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Not a response message"));
        }

        if (length < RPC_HEADER_SIZE + header.m_body_length) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Incomplete body"));
        }

        RpcResponse response;
        response.requestId(header.m_request_id);
        response.callMode(rpcDecodeCallMode(header.m_flags));
        response.endOfStream(rpcIsEndStream(header.m_flags));

        if (!response.deserializeBody(data + RPC_HEADER_SIZE, header.m_body_length)) {
            return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Failed to parse response body"));
        }

        return response;
    }

    /**
     * @brief 计算完整消息长度
     * @param data 数据缓冲区
     * @param length 当前数据长度
     * @return 完整消息长度，0表示数据不足
     */
    static size_t messageLength(const char* data, size_t length) {
        if (length < RPC_HEADER_SIZE) {
            return 0;
        }

        RpcHeader header;
        if (!header.deserialize(data)) {
            return 0;
        }

        return RPC_HEADER_SIZE + header.m_body_length;
    }
};

} // namespace galay::rpc

#endif // GALAY_RPC_CODEC_H

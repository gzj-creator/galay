/**
 * @file T38-H2ShutdownBatchRst.cc
 * @brief HTTP/2 shutdown batch RST_STREAM contract test
 */

#include <galay/cpp/galay-http2/kernel/http2_conn.h>
#include <galay/cpp/galay-http2/protoc/http2_frame.h>
#include <galay/cpp/galay-http2/protoc/http2_base.h>
#include <cassert>
#include <iostream>
#include <vector>

using namespace galay::http2;
using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief 测试 shutdown() 批量 RST_STREAM 的契约
 *
 * 验证点：
 * 1. Http2Conn 提供 forEachStream() 遍历所有流
 * 2. Http2Stream 提供 state() 查询流状态
 * 3. Http2FrameBuilder 提供 rstStreamBytes() 构建 RST_STREAM 帧
 * 4. 流状态包含 Closed 状态用于过滤
 */

int main() {
    std::cout << "[T38] Starting HTTP/2 shutdown batch RST contract tests\n";

    // ========== Static contract checks ==========
    struct StreamVisitor {
        void operator()(uint32_t, Http2Stream::ptr&) const {}
    };

    // 1. Http2Conn 必须提供 forEachStream() 方法
    static_assert(requires(Http2Conn* conn, StreamVisitor visitor) {
        conn->forEachStream(visitor);
    }, "Http2Conn must expose forEachStream() for stream iteration");

    // 2. Http2Stream 必须提供 state() 方法
    static_assert(requires(Http2Stream* stream) {
        { stream->state() } -> std::same_as<Http2StreamState>;
    }, "Http2Stream must expose state() returning Http2StreamState");

    // 3. Http2Stream 必须提供 onRstStreamSent() 方法
    static_assert(requires(Http2Stream* stream) {
        stream->onRstStreamSent();
    }, "Http2Stream must expose onRstStreamSent() for state transition");

    // 4. Http2FrameBuilder 必须提供 rstStreamBytes() 静态方法
    static_assert(requires(uint32_t stream_id, Http2ErrorCode error) {
        { Http2FrameBuilder::rstStreamBytes(stream_id, error) } -> std::same_as<std::string>;
    }, "Http2FrameBuilder must expose rstStreamBytes() for RST_STREAM frame construction");

    // 5. Http2StreamState 必须包含 Closed 状态
    static_assert(requires {
        Http2StreamState::Closed;
    }, "Http2StreamState must include Closed state");

    std::cout << "[T38] Static contract checks PASS\n";

    // ========== Scenario 1: forEachStream iteration ==========
    {
        std::cout << "[T38] Scenario 1: forEachStream iteration contract\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        // 创建几个流
        auto stream1 = conn.createStream(1);
        auto stream3 = conn.createStream(3);
        auto stream5 = conn.createStream(5);

        // 验证 forEachStream 可以遍历所有流
        int count = 0;
        conn.forEachStream([&](uint32_t stream_id, Http2Stream::ptr& stream) {
            assert(stream != nullptr);
            assert(stream_id == 1 || stream_id == 3 || stream_id == 5);
            count++;
        });

        assert(count == 3 && "Should iterate over 3 streams");

        std::cout << "[T38] Scenario 1 PASS: forEachStream iterated over " << count << " streams\n";
    }

    // ========== Scenario 2: Stream state filtering ==========
    {
        std::cout << "[T38] Scenario 2: Stream state filtering contract\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        // 创建流并设置不同状态
        auto stream1 = conn.createStream(1);
        auto stream3 = conn.createStream(3);
        auto stream5 = conn.createStream(5);

        // stream1: Open (活跃)
        stream1->setState(Http2StreamState::Open);

        // stream3: Closed (已关闭)
        stream3->setState(Http2StreamState::Closed);

        // stream5: HalfClosedLocal (半关闭)
        stream5->setState(Http2StreamState::HalfClosedLocal);

        // 统计非 Closed 状态的流
        int active_count = 0;
        conn.forEachStream([&](uint32_t stream_id, Http2Stream::ptr& stream) {
            if (stream && stream->state() != Http2StreamState::Closed) {
                active_count++;
            }
        });

        assert(active_count == 2 && "Should have 2 non-Closed streams (1 and 5)");

        std::cout << "[T38] Scenario 2 PASS: filtered " << active_count << " active streams\n";
    }

    // ========== Scenario 3: RST_STREAM frame construction ==========
    {
        std::cout << "[T38] Scenario 3: RST_STREAM frame construction contract\n";

        // 构建 RST_STREAM 帧
        auto bytes = Http2FrameBuilder::rstStreamBytes(1, Http2ErrorCode::NoError);

        // 验证帧格式：9 字节头部 + 4 字节错误码
        assert(bytes.size() == 13 && "RST_STREAM frame should be 13 bytes");

        // 验证帧类型 (第4字节应该是 RST_STREAM = 3)
        assert(static_cast<uint8_t>(bytes[3]) == 0x03 && "Frame type should be RST_STREAM (3)");

        // 验证流 ID (最后4字节的头部)
        uint32_t stream_id = (static_cast<uint32_t>(static_cast<uint8_t>(bytes[5]) & 0x7F) << 24) |
                             (static_cast<uint32_t>(static_cast<uint8_t>(bytes[6])) << 16) |
                             (static_cast<uint32_t>(static_cast<uint8_t>(bytes[7])) << 8) |
                             static_cast<uint32_t>(static_cast<uint8_t>(bytes[8]));
        assert(stream_id == 1 && "Stream ID should be 1");

        std::cout << "[T38] Scenario 3 PASS: RST_STREAM frame constructed correctly\n";
    }

    // ========== Scenario 4: Batch RST construction ==========
    {
        std::cout << "[T38] Scenario 4: Batch RST_STREAM construction\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        // 创建多个活跃流
        std::vector<uint32_t> stream_ids = {1, 3, 5, 7, 9};
        for (uint32_t id : stream_ids) {
            auto stream = conn.createStream(id);
            stream->setState(Http2StreamState::Open);
        }

        // 模拟批量构建 RST_STREAM 帧
        std::vector<std::string> rst_frames;
        conn.forEachStream([&](uint32_t stream_id, Http2Stream::ptr& stream) {
            if (stream && stream->state() != Http2StreamState::Closed) {
                auto bytes = Http2FrameBuilder::rstStreamBytes(stream_id, Http2ErrorCode::NoError);
                rst_frames.push_back(std::move(bytes));
            }
        });

        assert(rst_frames.size() == 5 && "Should construct 5 RST_STREAM frames");

        std::cout << "[T38] Scenario 4 PASS: constructed " << rst_frames.size() << " RST_STREAM frames\n";
    }

    // ========== Scenario 5: onRstStreamSent state transition ==========
    {
        std::cout << "[T38] Scenario 5: onRstStreamSent state transition\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        auto stream = conn.createStream(1);
        stream->setState(Http2StreamState::Open);

        // 调用 onRstStreamSent() 应该将流状态转换为 Closed
        stream->onRstStreamSent();

        assert(stream->state() == Http2StreamState::Closed && "Stream should be Closed after onRstStreamSent()");

        std::cout << "[T38] Scenario 5 PASS: onRstStreamSent() transitioned stream to Closed\n";
    }

    std::cout << "[T38] All scenarios PASS\n";
    return 0;
}

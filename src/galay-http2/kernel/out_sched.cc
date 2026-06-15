#include "out_sched.h"

#include <algorithm>

namespace galay::http2
{

H2OutboundSelection Http2OutboundScheduler::pickSendableFrames(H2OutboundBudget budget,
                                                                std::vector<H2StreamSendState>& streams)
{
    H2OutboundSelection out;
    if (budget.conn_window <= 0 || budget.max_frame_size == 0) {
        return out;
    }

    // 简单权重优先（高权重先发），后续可替换为更完整的公平调度。
    std::sort(streams.begin(), streams.end(), [](const H2StreamSendState& a, const H2StreamSendState& b) {
        return a.weight > b.weight;
    });

    for (auto& stream : streams) {
        while (!stream.pending_data.empty() &&
               stream.stream_window > 0 &&
               budget.conn_window > 0) {
            const size_t chunk = std::min<size_t>({
                static_cast<size_t>(budget.conn_window),
                static_cast<size_t>(stream.stream_window),
                static_cast<size_t>(budget.max_frame_size),
                stream.pending_data.size()
            });
            if (chunk == 0) {
                break;
            }

            auto frame = Http2FrameBuilder::data(stream.stream_id,
                                                 stream.pending_data.substr(0, chunk),
                                                 false);

            stream.pending_data.erase(0, chunk);
            budget.conn_window -= static_cast<int32_t>(chunk);
            stream.stream_window -= static_cast<int32_t>(chunk);
            out.total_data_bytes += chunk;

            const bool send_end = stream.end_stream && stream.pending_data.empty();
            frame->setEndStream(send_end);
            out.frames.push_back(std::move(frame));

            if (budget.conn_window <= 0) {
                break;
            }
        }
        if (budget.conn_window <= 0) {
            break;
        }
    }

    return out;
}

} // namespace galay::http2

#include "out_sched.h"

#include <algorithm>
#include <limits>

namespace galay::http2
{

namespace
{

void normalizePending(H2PendingData& pending)
{
    while (!pending.chunks.empty() &&
           pending.front_offset >= pending.chunks.front().size()) {
        pending.chunks.pop_front();
        pending.front_offset = 0;
    }
}

size_t frontPendingSize(const H2PendingData& pending)
{
    if (pending.chunks.empty() || pending.front_offset >= pending.chunks.front().size()) {
        return 0;
    }
    return pending.chunks.front().size() - pending.front_offset;
}

std::string takeFrontData(H2PendingData& pending, size_t bytes)
{
    const auto& front = pending.chunks.front();
    std::string out = front.substr(pending.front_offset, bytes);
    pending.front_offset += bytes;
    normalizePending(pending);
    return out;
}

bool shouldSendEndStreamOnly(const H2PendingData& pending)
{
    return pending.end_stream && pending.chunks.empty();
}

size_t saturatedAdd(size_t lhs, size_t rhs)
{
    if (lhs > std::numeric_limits<size_t>::max() - rhs) {
        return std::numeric_limits<size_t>::max();
    }
    return lhs + rhs;
}

size_t ceilDiv(size_t value, size_t divisor)
{
    if (value == 0 || divisor == 0) {
        return 0;
    }
    return 1 + ((value - 1) / divisor);
}

size_t quantumFor(const H2StreamSendState& stream, const H2SchedulerConfig& config)
{
    const size_t base = config.base_quantum == 0 ? 1 : config.base_quantum;
    const size_t weight = stream.weight == 0 ? 1 : stream.weight;
    if (base > std::numeric_limits<size_t>::max() / weight) {
        return std::numeric_limits<size_t>::max();
    }
    return base * weight;
}

size_t estimateDataFrameReserve(H2OutboundBudget budget)
{
    if (budget.max_frame_size == 0) {
        return 0;
    }
    const size_t conn_window = budget.conn_window > 0 ? static_cast<size_t>(budget.conn_window) : 0;
    return ceilDiv(conn_window, static_cast<size_t>(budget.max_frame_size));
}

void drainFrameQueue(std::deque<Http2Frame::uptr>& queue, H2OutboundSelection& out)
{
    while (!queue.empty()) {
        out.frames.push_back(std::move(queue.front()));
        queue.pop_front();
    }
}

} // namespace

H2OutboundSelection Http2OutboundScheduler::pickSendableFrames(H2OutboundBudget budget,
                                                                std::vector<H2StreamSendState>& streams,
                                                                H2SchedulerConfig config)
{
    H2OutboundSelection out;
    if (budget.max_frame_size == 0) {
        return out;
    }
    out.frames.reserve(estimateDataFrameReserve(budget));

    for (auto& stream : streams) {
        normalizePending(stream.pending);

        if (shouldSendEndStreamOnly(stream.pending)) {
            auto frame = Http2FrameBuilder::data(stream.stream_id, "", true);
            stream.pending.end_stream = false;
            out.frames.push_back(std::move(frame));
            continue;
        }
    }

    while (budget.conn_window > 0) {
        bool progressed = false;

        for (auto& stream : streams) {
            normalizePending(stream.pending);
            if (stream.pending.chunks.empty() || stream.stream_window <= 0) {
                stream.queued = false;
                continue;
            }

            stream.queued = true;
            const size_t quantum = quantumFor(stream, config);
            stream.deficit = std::min(std::numeric_limits<size_t>::max() - quantum,
                                      stream.deficit) + quantum;

            while (!stream.pending.chunks.empty() &&
                   stream.stream_window > 0 &&
                   budget.conn_window > 0 &&
                   stream.deficit > 0) {
                const size_t chunk = std::min<size_t>({
                    static_cast<size_t>(budget.conn_window),
                    static_cast<size_t>(stream.stream_window),
                    static_cast<size_t>(budget.max_frame_size),
                    frontPendingSize(stream.pending),
                    stream.deficit
                });
                if (chunk == 0) {
                    break;
                }

                auto payload = takeFrontData(stream.pending, chunk);
                auto frame = Http2FrameBuilder::data(stream.stream_id,
                                                     std::move(payload),
                                                     false);

                budget.conn_window -= static_cast<int32_t>(chunk);
                stream.stream_window -= static_cast<int32_t>(chunk);
                stream.deficit -= chunk;
                out.total_data_bytes += chunk;
                progressed = true;

                const bool send_end = stream.pending.end_stream && stream.pending.chunks.empty();
                if (send_end) {
                    stream.pending.end_stream = false;
                    stream.queued = false;
                }
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

        if (!progressed) {
            break;
        }
    }

    return out;
}

H2OutboundSelection Http2OutboundScheduler::pickSendableFrames(H2OutboundBudget budget,
                                                                H2OutboundQueues& queues,
                                                                H2SchedulerConfig config)
{
    H2OutboundSelection out;
    out.frames.reserve(saturatedAdd(
        saturatedAdd(queues.control_frames.size(), queues.header_frames.size()),
        estimateDataFrameReserve(budget)));
    drainFrameQueue(queues.control_frames, out);
    drainFrameQueue(queues.header_frames, out);

    auto data = pickSendableFrames(budget, queues.data_streams, config);
    out.total_data_bytes = data.total_data_bytes;
    for (auto& frame : data.frames) {
        out.frames.push_back(std::move(frame));
    }
    return out;
}

H2OutboundBytesSelection Http2OutboundScheduler::pickSendableBytes(H2OutboundBudget budget,
                                                                    std::vector<H2StreamSendState>& streams,
                                                                    H2SchedulerConfig config)
{
    H2OutboundBytesSelection out;
    if (budget.max_frame_size == 0) {
        return out;
    }
    out.frames.reserve(estimateDataFrameReserve(budget));

    for (auto& stream : streams) {
        normalizePending(stream.pending);

        if (shouldSendEndStreamOnly(stream.pending)) {
            out.frames.push_back(Http2FrameBuilder::dataBytes(stream.stream_id, "", true));
            stream.pending.end_stream = false;
            continue;
        }
    }

    while (budget.conn_window > 0) {
        bool progressed = false;

        for (auto& stream : streams) {
            normalizePending(stream.pending);
            if (stream.pending.chunks.empty() || stream.stream_window <= 0) {
                stream.queued = false;
                continue;
            }

            stream.queued = true;
            const size_t quantum = quantumFor(stream, config);
            stream.deficit = std::min(std::numeric_limits<size_t>::max() - quantum,
                                      stream.deficit) + quantum;

            while (!stream.pending.chunks.empty() &&
                   stream.stream_window > 0 &&
                   budget.conn_window > 0 &&
                   stream.deficit > 0) {
                const size_t chunk = std::min<size_t>({
                    static_cast<size_t>(budget.conn_window),
                    static_cast<size_t>(stream.stream_window),
                    static_cast<size_t>(budget.max_frame_size),
                    frontPendingSize(stream.pending),
                    stream.deficit
                });
                if (chunk == 0) {
                    break;
                }

                auto payload = takeFrontData(stream.pending, chunk);
                budget.conn_window -= static_cast<int32_t>(chunk);
                stream.stream_window -= static_cast<int32_t>(chunk);
                stream.deficit -= chunk;
                out.total_data_bytes += chunk;
                progressed = true;

                const bool send_end = stream.pending.end_stream && stream.pending.chunks.empty();
                if (send_end) {
                    stream.pending.end_stream = false;
                    stream.queued = false;
                }
                out.frames.push_back(Http2FrameBuilder::dataBytes(stream.stream_id, payload, send_end));

                if (budget.conn_window <= 0) {
                    break;
                }
            }
            if (budget.conn_window <= 0) {
                break;
            }
        }

        if (!progressed) {
            break;
        }
    }

    return out;
}

} // namespace galay::http2

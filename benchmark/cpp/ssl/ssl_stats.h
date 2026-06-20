#ifndef GALAY_SSL_BENCH_SSL_STATS_H
#define GALAY_SSL_BENCH_SSL_STATS_H

#include <cstddef>
#include <cstdint>

namespace galay::ssl::bench
{

struct SslIoStats {
    uint64_t send_ops = 0;
    uint64_t send_plain_bytes = 0;
    uint64_t recv_ops = 0;
    uint64_t recv_plain_bytes = 0;
    uint64_t recv_chunks = 0;
};

void sslStatsSetEnabled(bool enabled);
bool sslStatsEnabled();
SslIoStats sslStatsSnapshot();

void sslStatsAddSend(size_t bytes);
void sslStatsAddRecv(size_t bytes);

} // namespace galay::ssl::bench

#endif // GALAY_SSL_BENCH_SSL_STATS_H

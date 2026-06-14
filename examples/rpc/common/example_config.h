#ifndef GALAY_RPC_EXAMPLE_CONFIG_H
#define GALAY_RPC_EXAMPLE_CONFIG_H

#include <cstddef>
#include <cstdint>

namespace galay::rpc::example {

inline constexpr uint16_t kDefaultEchoPort = 9000;
inline constexpr uint16_t kDefaultStreamPort = 9100;
inline constexpr size_t kDefaultStreamRingBufferSize = 128 * 1024;
inline constexpr size_t kDefaultStreamFrameCount = 1000;
inline constexpr size_t kDefaultStreamPayloadSize = 128;

} // namespace galay::rpc::example

#endif // GALAY_RPC_EXAMPLE_CONFIG_H

#pragma once

#include <chrono>
#include <cstdint>

namespace galay::example {

inline constexpr std::chrono::seconds kDefaultWaitTimeout{5};
inline constexpr std::uint16_t kLocalIpv4PortBase = 9000;

}  // namespace galay::example

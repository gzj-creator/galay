#ifndef GALAY_HTTP_EXAMPLE_COMMON_H
#define GALAY_HTTP_EXAMPLE_COMMON_H

#include <cstdint>

namespace galay::http::example {

constexpr uint16_t kDefaultHttpEchoPort = 8080;
constexpr uint16_t kDefaultWsEchoPort = 8080;
constexpr uint16_t kDefaultHttpsEchoPort = 8443;
constexpr uint16_t kDefaultWssEchoPort = 8443;
constexpr uint16_t kDefaultH2cEchoPort = 9080;
constexpr uint16_t kDefaultH2EchoPort = 9443;
constexpr uint16_t kDefaultStaticPort = 8090;
constexpr uint16_t kDefaultProxyPort = 8081;
constexpr uint16_t kDefaultProxyUpstreamPort = 8080;
constexpr uint16_t kDefaultBlacklistPort = 8092;

}  // namespace galay::http::example

#endif  // GALAY_HTTP_EXAMPLE_COMMON_H

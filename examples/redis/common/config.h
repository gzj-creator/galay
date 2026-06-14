#ifndef GALAY_REDIS_EXAMPLE_CONFIG_H
#define GALAY_REDIS_EXAMPLE_CONFIG_H

#include <cstdint>

namespace galay::redis::example {

inline constexpr const char* kDefaultRedisHost = "127.0.0.1";
inline constexpr uint16_t kDefaultRedisPort = 6379;
inline constexpr int kDefaultTimeoutSeconds = 5;

inline constexpr const char* kDefaultDemoKey = "example:demo:key";
inline constexpr const char* kDefaultDemoValue = "hello-galay-redis";

inline constexpr const char* kDefaultPipelinePrefix = "example:pipeline:";
inline constexpr int kDefaultPipelineBatchSize = 10;

inline constexpr const char* kDefaultPubSubChannel = "example:pubsub:channel";
inline constexpr const char* kDefaultPubSubMessage = "hello-pubsub";
inline constexpr const char* kDefaultTopologyKey = "{example}:topology:key";
inline constexpr const char* kDefaultTopologyValue = "topology-ok";

}  // namespace galay::redis::example

#endif  // GALAY_REDIS_EXAMPLE_CONFIG_H

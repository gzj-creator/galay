#include "tracing/kernel/span_guard.h"
#include "tracing/log/logger.h"

int main() {
    auto span = galay::tracing::startSpan("checkout");
    GALAY_LOG_INFO("order accepted {}", 123);
}

#include <galay/cpp/galay-tracing/common/trace_id.h>

int main() {
    auto id = galay::tracing::TraceId::zero();
    return id.is_valid() ? 1 : 0;
}

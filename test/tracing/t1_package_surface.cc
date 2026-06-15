#include "galay-tracing/common/trace_id.h"

int main() {
    auto id = galay::tracing::TraceId::zero();
    return id.isValid() ? 1 : 0;
}

import galay.tracing;

int main() {
    auto span = galay::tracing::startSpan("checkout");
    GALAY_LOG_INFO("order accepted {}", 123);
}

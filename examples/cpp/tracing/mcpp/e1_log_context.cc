import galay.tracing;

int main() {
    auto span = galay::tracing::start_span("checkout");
    GALAY_LOG_INFO("order accepted {}", 123);
}

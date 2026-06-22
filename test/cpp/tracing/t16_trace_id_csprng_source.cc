/**
 * @file t16_trace_id_csprng_source.cc
 * @brief 锁定 trace/span id 随机生成必须使用系统 CSPRNG。
 */

#include <galay/cpp/galay-tracing/common/span_id.h>
#include <galay/cpp/galay-tracing/common/trace_id.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::filesystem::path projectRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
}

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void randomIdsStayValid()
{
    for (int i = 0; i < 128; ++i) {
        assert(galay::tracing::TraceId::random().isValid());
        assert(galay::tracing::SpanId::random().isValid());
    }
}

void idGeneratorUsesSystemCsprng()
{
    const auto source = readAll(projectRoot() / "src/cpp/galay-tracing/common/id_format.cc");
    assert(source.find("RAND_bytes") != std::string::npos ||
           source.find("RAND_priv_bytes") != std::string::npos);
    assert(source.find("#include <random>") == std::string::npos);
    assert(source.find("std::random_device") == std::string::npos);
    assert(source.find("SplitMix64") == std::string::npos);
}

} // namespace

int main()
{
    randomIdsStayValid();
    idGeneratorUsesSystemCsprng();
    std::cout << "T16-TraceIdCspRngSource PASS\n";
    return 0;
}

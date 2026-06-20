load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "galay-tracing",
    srcs = glob(
        ["**/*.cc"],
        exclude = [
            "adapters/spdlog_sink.cc",
            "galay-kernel/otlp_http_exporter.cc",
        ],
    ),
    hdrs = glob([
        "**/*.h",
        "**/*.hpp",
        "**/*.inl",
    ]),
    strip_include_prefix = ".",
    include_prefix = "galay/cpp/galay-tracing",
    copts = ["-std=c++23"],
    deps = [
        "//src/cpp/galay-kernel:galay-kernel",
        "//src/cpp/galay-utils:galay-utils",
    ],
)

load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "galay-tracing",
    srcs = glob(
        ["**/*.cc"],
        exclude = [
            "adapters/spdlog_sink.cc",
            "kernel/otlp_http_exporter.cc",
        ],
    ),
    hdrs = glob([
        "**/*.h",
        "**/*.hpp",
        "**/*.inl",
    ]),
    includes = [".."],
    copts = ["-std=c++23"],
    deps = [
        "//src/kernel:galay-kernel",
        "//src/utils:galay-utils",
    ],
)

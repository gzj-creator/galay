load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "galay-mysql",
    srcs = glob(["**/*.cc"]),
    hdrs = glob([
        "**/*.h",
        "**/*.hpp",
        "**/*.inl",
    ]),
    strip_include_prefix = ".",
    include_prefix = "galay/cpp/galay-mysql",
    copts = ["-std=c++23"],
    deps = [
        "//src/cpp/galay-kernel:galay-kernel",
        "//src/cpp/galay-utils:galay-utils",
    ],
)

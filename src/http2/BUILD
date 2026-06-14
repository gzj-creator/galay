load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "galay-http2",
    srcs = glob(["**/*.cc"]),
    hdrs = glob([
        "**/*.h",
        "**/*.hpp",
        "**/*.inl",
    ]),
    includes = [".."],
    copts = ["-std=c++23"],
    deps = [
        "//src/http:galay-http",
        "//src/kernel:galay-kernel",
        "//src/utils:galay-utils",
    ],
)

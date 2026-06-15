load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "galay-ws",
    srcs = glob(["**/*.cc"]),
    hdrs = glob([
        "**/*.h",
        "**/*.hpp",
        "**/*.inl",
    ]),
    includes = [".."],
    copts = ["-std=c++23"],
    deps = [
        "//src/galay-http:galay-http",
        "//src/galay-kernel:galay-kernel",
        "//src/galay-utils:galay-utils",
    ],
)

load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "galay-http",
    srcs = glob(["**/*.cc"]),
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

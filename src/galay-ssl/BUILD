load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "galay-ssl",
    srcs = glob(["**/*.cc"]),
    hdrs = glob([
        "**/*.h",
        "**/*.hpp",
        "**/*.inl",
    ]),
    includes = [".."],
    copts = ["-std=c++23"],
    linkopts = ["-lssl", "-lcrypto"],
    deps = [
        "//src/galay-kernel:galay-kernel",
        "//src/galay-utils:galay-utils",
    ],
)

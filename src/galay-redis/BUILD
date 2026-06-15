load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "galay-redis",
    srcs = glob(
        ["**/*.cc"],
        exclude = ["async/client_tls.cc"],
    ),
    hdrs = glob([
        "**/*.h",
        "**/*.hpp",
        "**/*.inl",
    ]),
    includes = [".."],
    copts = ["-std=c++23"],
    deps = [
        "//src/galay-kernel:galay-kernel",
        "//src/galay-utils:galay-utils",
    ],
)

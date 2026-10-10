load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "galay-api",
    srcs = glob(["*.cc"]),
    hdrs = glob(["*.h"]),
    strip_include_prefix = ".",
    include_prefix = "galay/cpp/galay-api",
    copts = ["-std=c++23"],
    deps = [
        "//thirdparty/fast_float:fast_float",
        "//src/cpp/galay-http:galay-http",
        "//src/cpp/galay-serde:galay-serde",
    ],
)

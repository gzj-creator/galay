load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "galay-utils",
    srcs = ["system/rdtsc.cc"],
    hdrs = glob([
        "**/*.h",
        "**/*.hpp",
        "**/*.inl",
    ]),
    strip_include_prefix = ".",
    include_prefix = "galay/cpp/galay-utils",
    copts = ["-std=c++23"],
)

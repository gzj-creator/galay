load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "galay-rpc",
    srcs = glob(["**/*.cc"]),
    hdrs = glob([
        "**/*.h",
        "**/*.hpp",
        "**/*.inl",
    ]),
    strip_include_prefix = ".",
    include_prefix = "galay/cpp/galay-rpc",
    copts = ["-std=c++23"],
    deps = [
        "//src/cpp/galay-kernel:galay-kernel",
        "//src/cpp/galay-utils:galay-utils",
    ],
)

cc_library(
    name = "galay-rpc-etcd",
    hdrs = ["discovery/etcd_service_registry.h"],
    strip_include_prefix = ".",
    include_prefix = "galay/cpp/galay-rpc",
    copts = ["-std=c++23"],
    defines = ["GALAY_RPC_HAS_ETCD"],
    deps = [
        ":galay-rpc",
        "//src/cpp/galay-etcd:galay-etcd",
    ],
)

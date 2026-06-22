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
    strip_include_prefix = ".",
    include_prefix = "galay/cpp/galay-redis",
    copts = ["-std=c++23"],
    deps = [
        "//src/cpp/galay-kernel:galay-kernel",
        "//src/cpp/galay-utils:galay-utils",
    ],
)

cc_library(
    name = "galay-redis-tls",
    srcs = ["async/client_tls.cc"],
    hdrs = glob([
        "**/*.h",
        "**/*.hpp",
        "**/*.inl",
    ]),
    strip_include_prefix = ".",
    include_prefix = "galay/cpp/galay-redis",
    copts = ["-std=c++23"],
    defines = ["GALAY_SSL_FEATURE_ENABLED"],
    deps = [
        ":galay-redis",
        "//src/cpp/galay-ssl:galay-ssl",
    ],
)

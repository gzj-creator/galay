load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "galay-kernel",
    srcs = glob(["**/*.cc"]),
    hdrs = glob([
        "**/*.h",
        "**/*.hpp",
        "**/*.inl",
    ]),
    strip_include_prefix = ".",
    include_prefix = "galay/cpp/galay-kernel",
    copts = select({
        "@platforms//os:macos": [
            "-std=c++23",
            "-I/opt/homebrew/include",
            "-I/usr/local/include",
        ],
        "@platforms//os:linux": [
            "-std=c++23",
            "-I/usr/local/include",
            "-I/usr/include",
        ],
        "//conditions:default": ["-std=c++23"],
    }),
    defines = select({
        "@platforms//os:macos": ["USE_KQUEUE"],
        "@platforms//os:linux": ["USE_EPOLL"],
        "//conditions:default": [],
    }),
    linkopts = select({
        "@platforms//os:linux": ["-laio"],
        "//conditions:default": [],
    }),
    deps = [
        "//src/cpp/galay-utils:galay-utils",
    ],
)

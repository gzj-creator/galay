load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# Build-time CMake 3.28+ is required; it is not a product runtime dependency.
genrule(
    name = "swagger_ui",
    srcs = ["//assets/swagger-ui:files", "//assets/swagger-ui:swagger-ui.css"],
    tools = ["//cmake:EmbedSwaggerUi.cmake"],
    outs = ["swagger_ui.cc"],
    cmd = "cmake -DASSET_DIR=$$(dirname $(location //assets/swagger-ui:swagger-ui.css)) " +
          "-DOUTPUT_FILE=$@ -P $(location //cmake:EmbedSwaggerUi.cmake)",
)

cc_library(
    name = "galay-api",
    srcs = glob(["*.cc"]) + [":swagger_ui"],
    hdrs = glob(["*.h"]),
    strip_include_prefix = ".",
    include_prefix = "galay/cpp/galay-api",
    copts = ["-std=c++23"],
    defines = ["GALAY_API_HTTP2_FEATURE_ENABLED"],
    deps = [
        "//src/cpp/galay-http:galay-http",
        "//src/cpp/galay-http2:galay-http2",
        "//src/cpp/galay-serde:galay-serde",
    ],
)

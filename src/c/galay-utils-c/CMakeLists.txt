file(GLOB galay_c_utils_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

add_library(galay-c-utils ${galay_c_utils_sources})
add_library(galay::c-utils ALIAS galay-c-utils)

target_compile_features(galay-c-utils PUBLIC c_std_11)
target_include_directories(galay-c-utils
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
target_link_libraries(galay-c-utils PUBLIC galay::c-common PRIVATE OpenSSL::Crypto)

set_target_properties(galay-c-utils PROPERTIES EXPORT_NAME c-utils)

add_library(galay-c-utils utils.cc)
add_library(galay::c-utils ALIAS galay-c-utils)

target_link_libraries(galay-c-utils PUBLIC galay::utils)
target_include_directories(galay-c-utils
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(galay-c-utils PROPERTIES
    EXPORT_NAME c-utils
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

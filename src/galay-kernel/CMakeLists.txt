file(GLOB_RECURSE KERNEL_SOURCES CONFIGURE_DEPENDS
    "*.cc"
    "*.h"
    "*.hpp"
    "*.inl"
)

add_library(kernel ${KERNEL_SOURCES})
add_library(galay::kernel ALIAS kernel)
add_library(galay-kernel::galay-kernel ALIAS kernel)

galay_configure_io_backend(GALAY_KERNEL_BACKEND PLATFORM_LIBS)

target_include_directories(kernel
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(kernel
    PUBLIC
        galay::utils
        ${PLATFORM_LIBS}
)

if(GALAY_KERNEL_BACKEND STREQUAL "epoll")
    target_compile_definitions(kernel PUBLIC USE_EPOLL)
elseif(GALAY_KERNEL_BACKEND STREQUAL "io_uring")
    target_compile_definitions(kernel PUBLIC USE_IOURING)
elseif(GALAY_KERNEL_BACKEND STREQUAL "kqueue")
    target_compile_definitions(kernel PUBLIC USE_KQUEUE)
else()
    message(FATAL_ERROR "GALAY_KERNEL_BACKEND is not set correctly: ${GALAY_KERNEL_BACKEND}")
endif()

set_target_properties(kernel PROPERTIES EXPORT_NAME kernel)

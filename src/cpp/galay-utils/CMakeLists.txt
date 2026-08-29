file(GLOB_RECURSE UTILS_INTERFACE_SOURCES CONFIGURE_DEPENDS
    "*.h"
    "*.hpp"
    "*.inl"
    "*.cppm"
)
file(GLOB_RECURSE UTILS_SOURCES CONFIGURE_DEPENDS "*.cc")

add_library(galay-utils ${UTILS_SOURCES})
add_library(galay::utils ALIAS galay-utils)

galay_ensure_concurrentqueue()

target_include_directories(galay-utils
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(galay-utils
    PUBLIC
        $<BUILD_INTERFACE:concurrentqueue::concurrentqueue>
)

set_target_properties(galay-utils PROPERTIES EXPORT_NAME utils)

# install(DIRECTORY ...) is merge-only.  Remove stale public headers and
# vendored third-party copies before the top-level install rules repopulate
# those directories.  This script lives in the first enabled subdirectory so
# it runs before the root-level C++23 module interface installs.
install(CODE [[
    file(REMOVE_RECURSE
        "${CMAKE_INSTALL_PREFIX}/include/galay/cpp"
        "${CMAKE_INSTALL_PREFIX}/include/galay/thirdparty/concurrentqueue"
        "${CMAKE_INSTALL_PREFIX}/include/galay/thirdparty/simdjson")
]])

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(galay-utils INTERFACE Threads::Threads)
    if(CMAKE_DL_LIBS)
        target_link_libraries(galay-utils INTERFACE ${CMAKE_DL_LIBS})
    else()
        target_link_libraries(galay-utils INTERFACE dl)
    endif()
endif()

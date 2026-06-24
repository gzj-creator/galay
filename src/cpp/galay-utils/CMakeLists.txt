file(GLOB_RECURSE UTILS_INTERFACE_SOURCES CONFIGURE_DEPENDS
    "*.h"
    "*.hpp"
    "*.inl"
    "*.cppm"
)
file(GLOB_RECURSE UTILS_SOURCES CONFIGURE_DEPENDS "*.cc")

add_library(utils ${UTILS_SOURCES})
add_library(galay::utils ALIAS utils)
add_library(galay::galay-utils ALIAS utils)

galay_ensure_concurrentqueue()

target_include_directories(utils
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(utils
    PUBLIC
        $<BUILD_INTERFACE:concurrentqueue::concurrentqueue>
)

set_target_properties(utils PROPERTIES EXPORT_NAME utils)

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(utils INTERFACE Threads::Threads)
    if(CMAKE_DL_LIBS)
        target_link_libraries(utils INTERFACE ${CMAKE_DL_LIBS})
    else()
        target_link_libraries(utils INTERFACE dl)
    endif()
endif()

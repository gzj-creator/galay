if(TARGET galay::kernel)
    set(_galay_mysql_kernel_target galay::kernel)
elseif(TARGET galay-kernel)
    add_library(galay::kernel ALIAS galay-kernel)
    set(_galay_mysql_kernel_target galay::kernel)
elseif(TARGET kernel)
    add_library(galay::kernel ALIAS kernel)
    set(_galay_mysql_kernel_target galay::kernel)
elseif(TARGET galay-kernel::galay-kernel)
    set(_galay_mysql_kernel_target galay-kernel::galay-kernel)
else()
    message(FATAL_ERROR "mysql requires an in-tree kernel target")
endif()

if(TARGET galay::utils)
    set(_galay_mysql_utils_target galay::utils)
elseif(TARGET galay-utils)
    add_library(galay::utils ALIAS galay-utils)
    set(_galay_mysql_utils_target galay::utils)
elseif(TARGET utils)
    add_library(galay::utils ALIAS utils)
    set(_galay_mysql_utils_target galay::utils)
elseif(TARGET galay::galay-utils)
    set(_galay_mysql_utils_target galay::galay-utils)
else()
    message(FATAL_ERROR "mysql requires an in-tree utils target")
endif()

if(GALAY_SSL_FEATURE_ENABLED)
    if(TARGET galay::ssl)
        set(_galay_mysql_ssl_target galay::ssl)
    elseif(TARGET galay-ssl)
        add_library(galay::ssl ALIAS galay-ssl)
        set(_galay_mysql_ssl_target galay::ssl)
    elseif(TARGET ssl)
        add_library(galay::ssl ALIAS ssl)
        set(_galay_mysql_ssl_target galay::ssl)
    else()
        message(FATAL_ERROR "mysql requires an in-tree ssl target when GALAY_SSL_FEATURE_ENABLED is ON")
    endif()
endif()

file(GLOB_RECURSE GALAY_MYSQL_SOURCES CONFIGURE_DEPENDS "*.cc")

add_library(mysql ${GALAY_MYSQL_SOURCES})
add_library(galay::mysql ALIAS mysql)

if(NOT TARGET galay-mysql)
    add_library(galay-mysql ALIAS mysql)
endif()
if(NOT TARGET galay-mysql::galay-mysql)
    add_library(galay-mysql::galay-mysql ALIAS mysql)
endif()

target_compile_features(mysql PUBLIC cxx_std_23)
target_include_directories(mysql
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(mysql
    PUBLIC
        ${_galay_mysql_kernel_target}
        ${_galay_mysql_utils_target}
)

if(GALAY_SSL_FEATURE_ENABLED)
    target_link_libraries(mysql PUBLIC ${_galay_mysql_ssl_target})
    target_compile_definitions(mysql PUBLIC GALAY_SSL_FEATURE_ENABLED)
endif()

set_target_properties(mysql PROPERTIES EXPORT_NAME mysql)

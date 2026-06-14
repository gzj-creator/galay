if(TARGET galay::kernel)
    set(_galay_mongo_kernel_target galay::kernel)
elseif(TARGET galay-kernel)
    add_library(galay::kernel ALIAS galay-kernel)
    set(_galay_mongo_kernel_target galay::kernel)
elseif(TARGET kernel)
    add_library(galay::kernel ALIAS kernel)
    set(_galay_mongo_kernel_target galay::kernel)
elseif(TARGET galay-kernel::galay-kernel)
    set(_galay_mongo_kernel_target galay-kernel::galay-kernel)
else()
    message(FATAL_ERROR "mongo requires an in-tree kernel target")
endif()

if(TARGET galay::utils)
    set(_galay_mongo_utils_target galay::utils)
elseif(TARGET galay-utils)
    add_library(galay::utils ALIAS galay-utils)
    set(_galay_mongo_utils_target galay::utils)
elseif(TARGET utils)
    add_library(galay::utils ALIAS utils)
    set(_galay_mongo_utils_target galay::utils)
elseif(TARGET galay::galay-utils)
    set(_galay_mongo_utils_target galay::galay-utils)
else()
    message(FATAL_ERROR "mongo requires an in-tree utils target")
endif()

if(GALAY_SSL_FEATURE_ENABLED)
    if(TARGET galay::ssl)
        set(_galay_mongo_ssl_target galay::ssl)
    elseif(TARGET galay-ssl)
        add_library(galay::ssl ALIAS galay-ssl)
        set(_galay_mongo_ssl_target galay::ssl)
    elseif(TARGET ssl)
        add_library(galay::ssl ALIAS ssl)
        set(_galay_mongo_ssl_target galay::ssl)
    else()
        message(FATAL_ERROR "mongo requires an in-tree ssl target when GALAY_SSL_FEATURE_ENABLED is ON")
    endif()
endif()

file(GLOB_RECURSE GALAY_MONGO_SOURCES CONFIGURE_DEPENDS "*.cc")

add_library(mongo ${GALAY_MONGO_SOURCES})
add_library(galay::mongo ALIAS mongo)

if(NOT TARGET galay-mongo)
    add_library(galay-mongo ALIAS mongo)
endif()
if(NOT TARGET galay-mongo::galay-mongo)
    add_library(galay-mongo::galay-mongo ALIAS mongo)
endif()

target_compile_features(mongo PUBLIC cxx_std_23)
target_include_directories(mongo
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(mongo
    PUBLIC
        ${_galay_mongo_kernel_target}
        ${_galay_mongo_utils_target}
)

if(GALAY_SSL_FEATURE_ENABLED)
    target_link_libraries(mongo PUBLIC ${_galay_mongo_ssl_target})
    target_compile_definitions(mongo PUBLIC GALAY_SSL_FEATURE_ENABLED)
endif()

set_target_properties(mongo PROPERTIES EXPORT_NAME mongo)

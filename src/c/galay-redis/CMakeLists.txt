add_library(galay-c-redis redis.cc)
add_library(galay::c-redis ALIAS galay-c-redis)

target_compile_features(galay-c-redis PRIVATE cxx_std_23)
target_include_directories(galay-c-redis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-c-redis
    PUBLIC
        galay::c-common
        galay::redis
)

set_target_properties(galay-c-redis PROPERTIES EXPORT_NAME c-redis)

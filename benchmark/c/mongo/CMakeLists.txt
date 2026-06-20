add_executable(benchmark_c_mongo_bson_codec_smoke b1_bson_codec_smoke.c)
target_include_directories(benchmark_c_mongo_bson_codec_smoke
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(benchmark_c_mongo_bson_codec_smoke PRIVATE galay::c-mongo)
set_target_properties(benchmark_c_mongo_bson_codec_smoke PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

add_executable(example_c_mongo_bson_uri e1_bson_uri.c)
target_include_directories(example_c_mongo_bson_uri
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(example_c_mongo_bson_uri PRIVATE galay::c-mongo)
set_target_properties(example_c_mongo_bson_uri PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

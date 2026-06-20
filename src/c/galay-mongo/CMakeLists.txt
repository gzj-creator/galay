add_library(galay-c-mongo mongo.cc)
add_library(galay::c-mongo ALIAS galay-c-mongo)

target_link_libraries(galay-c-mongo PUBLIC galay::mongo galay::c-common)
target_include_directories(galay-c-mongo
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(galay-c-mongo PROPERTIES
    EXPORT_NAME c-mongo
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

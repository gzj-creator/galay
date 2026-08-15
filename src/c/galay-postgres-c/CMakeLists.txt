file(GLOB GALAY_C_POSTGRES_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

add_library(galay-c-postgres ${GALAY_C_POSTGRES_SOURCES} postgres.h)
add_library(galay::c-postgres ALIAS galay-c-postgres)

target_compile_features(galay-c-postgres PUBLIC cxx_std_23)
target_include_directories(galay-c-postgres
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
target_link_libraries(galay-c-postgres
    PUBLIC
        galay::c-common
        galay::c-kernel
    PRIVATE
        OpenSSL::Crypto
)
set_target_properties(galay-c-postgres PROPERTIES EXPORT_NAME c-postgres)
if(NOT GALAY_C_POSTGRES_SOURCES)
    set_target_properties(galay-c-postgres PROPERTIES LINKER_LANGUAGE CXX)
endif()

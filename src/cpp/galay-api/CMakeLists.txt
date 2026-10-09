if(NOT TARGET galay::http OR NOT TARGET galay::serde)
    message(FATAL_ERROR "galay-api requires galay::http and galay::serde (serde 0.4.0)")
endif()

file(GLOB GALAY_API_SOURCES CONFIGURE_DEPENDS "*.cc")
add_library(galay-api ${GALAY_API_SOURCES})
add_library(galay::api ALIAS galay-api)
set_target_properties(galay-api PROPERTIES EXPORT_NAME api)
target_compile_features(galay-api PUBLIC cxx_std_23)
target_include_directories(galay-api PUBLIC
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(galay-api PUBLIC galay::http galay::serde)

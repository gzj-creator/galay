if(NOT TARGET galay::http OR NOT TARGET galay::serde)
    message(FATAL_ERROR "galay-doc requires galay::http and galay::serde (serde 0.4.0)")
endif()

file(GLOB GALAY_DOC_SOURCES CONFIGURE_DEPENDS "*.cc")
add_library(galay-doc ${GALAY_DOC_SOURCES})
add_library(galay::doc ALIAS galay-doc)
set_target_properties(galay-doc PROPERTIES EXPORT_NAME doc)
target_compile_features(galay-doc PUBLIC cxx_std_23)
target_include_directories(galay-doc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(galay-doc PUBLIC galay::http galay::serde)

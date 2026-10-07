if(NOT TARGET galay::http OR NOT TARGET galay::serde)
    message(FATAL_ERROR "galay-api requires galay::http and galay::serde (serde 0.4.0)")
endif()

file(GLOB GALAY_API_SOURCES CONFIGURE_DEPENDS "*.cc")
set(swagger_directory "${PROJECT_SOURCE_DIR}/assets/swagger-ui")
set(swagger_files)
foreach(name IN ITEMS swagger-ui.css swagger-ui-bundle.js swagger-ui-standalone-preset.js
        favicon-16x16.png favicon-32x32.png LICENSE NOTICE README.md SHA256SUMS)
    if(NOT EXISTS "${swagger_directory}/${name}")
        message(FATAL_ERROR "galay-api requires vendored Swagger UI resource: ${swagger_directory}/${name}")
    endif()
    list(APPEND swagger_files "${swagger_directory}/${name}")
endforeach()
set(swagger_source "${CMAKE_CURRENT_BINARY_DIR}/swagger_ui.cc")
add_custom_command(OUTPUT "${swagger_source}"
    COMMAND ${CMAKE_COMMAND} "-DASSET_DIR=${swagger_directory}" "-DOUTPUT_FILE=${swagger_source}"
        -P "${PROJECT_SOURCE_DIR}/cmake/EmbedSwaggerUi.cmake"
    DEPENDS ${swagger_files} "${PROJECT_SOURCE_DIR}/cmake/EmbedSwaggerUi.cmake"
    COMMENT "Verifying and embedding official Swagger UI"
    VERBATIM)
add_library(galay-api ${GALAY_API_SOURCES} "${swagger_source}")
add_library(galay::api ALIAS galay-api)
set_target_properties(galay-api PROPERTIES EXPORT_NAME api)
target_compile_features(galay-api PUBLIC cxx_std_23)
target_include_directories(galay-api PUBLIC
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(galay-api PUBLIC galay::http galay::serde)

install(FILES "${swagger_directory}/LICENSE" "${swagger_directory}/NOTICE"
    "${swagger_directory}/README.md" "${swagger_directory}/SHA256SUMS"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/galay/swagger-ui)

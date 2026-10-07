cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED ASSET_DIR OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "Swagger UI embedding requires ASSET_DIR and OUTPUT_FILE")
endif()

set(asset_names
    swagger-ui.css
    swagger-ui-bundle.js
    swagger-ui-standalone-preset.js
    favicon-16x16.png
    favicon-32x32.png
    LICENSE
    NOTICE
    README.md
    SHA256SUMS)
set(content_types
    "text/css; charset=utf-8"
    "application/javascript; charset=utf-8"
    "application/javascript; charset=utf-8"
    image/png
    image/png
    "text/plain; charset=utf-8"
    "text/plain; charset=utf-8"
    "text/plain; charset=utf-8"
    "text/plain; charset=utf-8")
# CMake treats semicolons as list separators, even within quoted strings.
string(REPLACE "; charset" "\\; charset" content_types "${content_types}")

foreach(name IN LISTS asset_names)
    if(NOT EXISTS "${ASSET_DIR}/${name}" OR IS_DIRECTORY "${ASSET_DIR}/${name}")
        message(FATAL_ERROR "Swagger UI resource missing: ${ASSET_DIR}/${name}")
    endif()
    file(SIZE "${ASSET_DIR}/${name}" size)
    if(size EQUAL 0 OR size GREATER 16777216)
        message(FATAL_ERROR "Swagger UI resource must be nonempty and at most 16 MiB: ${name}")
    endif()
endforeach()

file(STRINGS "${ASSET_DIR}/SHA256SUMS" manifest)
list(LENGTH manifest manifest_size)
if(NOT manifest_size EQUAL 7)
    message(FATAL_ERROR "Swagger UI SHA256SUMS must contain exactly seven upstream files")
endif()
set(verified)
foreach(entry IN LISTS manifest)
    if(NOT entry MATCHES "^([0-9a-f]+)  ([A-Za-z0-9._-]+)$")
        message(FATAL_ERROR "Invalid Swagger UI SHA256SUMS entry: ${entry}")
    endif()
    set(expected "${CMAKE_MATCH_1}")
    set(name "${CMAKE_MATCH_2}")
    string(LENGTH "${expected}" hash_size)
    list(FIND asset_names "${name}" asset_index)
    if(NOT hash_size EQUAL 64 OR asset_index LESS 0 OR asset_index GREATER 6 OR name IN_LIST verified)
        message(FATAL_ERROR "Invalid or duplicate Swagger UI SHA256SUMS entry: ${entry}")
    endif()
    file(SHA256 "${ASSET_DIR}/${name}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Swagger UI SHA256 mismatch for ${name}: expected ${expected}, got ${actual}")
    endif()
    list(APPEND verified "${name}")
endforeach()

set(generated "// Generated from verified swagger-ui-dist@5.17.14; do not edit.\n")
string(APPEND generated "#include <galay/cpp/galay-api/ui_assets.h>\n#include <array>\n\n")
string(APPEND generated "namespace galay::api::docs_detail {\nnamespace {\n")
set(index 0)
foreach(name IN LISTS asset_names)
    file(READ "${ASSET_DIR}/${name}" hex HEX)
    string(LENGTH "${hex}" hex_size)
    string(APPEND generated "constexpr char asset_${index}[] =\n")
    set(offset 0)
    while(offset LESS hex_size)
        string(SUBSTRING "${hex}" ${offset} 4096 chunk)
        string(REGEX REPLACE "(..)" "\\\\x\\1" escaped "${chunk}")
        string(APPEND generated "    \"${escaped}\"\n")
        math(EXPR offset "${offset} + 4096")
    endwhile()
    string(APPEND generated "    ;\n")
    math(EXPR index "${index} + 1")
endforeach()
string(APPEND generated "constexpr std::array<UiAsset, 9> assets{{\n")
set(index 0)
foreach(name IN LISTS asset_names)
    list(GET content_types ${index} type)
    string(APPEND generated "    {\"${name}\", \"${type}\", {asset_${index}, sizeof(asset_${index}) - 1}},\n")
    math(EXPR index "${index} + 1")
endforeach()
string(APPEND generated "}};\n} // namespace\n\n")
string(APPEND generated "std::span<const UiAsset> embedded_assets() noexcept { return assets; }\n")
string(APPEND generated "} // namespace galay::api::docs_detail\n")
file(WRITE "${OUTPUT_FILE}" "${generated}")
message(STATUS "Embedded verified swagger-ui-dist@5.17.14 (nine resources, seven SHA256 checks)")

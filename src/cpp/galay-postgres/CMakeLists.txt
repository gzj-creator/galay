file(GLOB_RECURSE GALAY_POSTGRES_SOURCES CONFIGURE_DEPENDS "*.cc")

add_library(galay-postgres ${GALAY_POSTGRES_SOURCES})
add_library(galay::postgres ALIAS galay-postgres)

target_compile_features(galay-postgres PUBLIC cxx_std_23)
target_include_directories(galay-postgres
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-postgres
    PUBLIC
        galay::kernel
        galay::utils
)
set_target_properties(galay-postgres PROPERTIES EXPORT_NAME postgres)

set(GALAY_POSTGRES_CPP23_MODULES_EFFECTIVE OFF)
if(GALAY_ENABLE_CPP23_MODULES)
    if(CMAKE_VERSION VERSION_LESS 3.28)
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires CMake >= 3.28 for galay-postgres; skipping module file set")
    elseif(NOT CMAKE_GENERATOR MATCHES "Ninja|Visual Studio")
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires a module-capable generator for galay-postgres; skipping module file set")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON is not enabled for AppleClang; skipping postgres module file set")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires Clang >= 16 for galay-postgres; skipping module file set")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires GCC >= 14 for galay-postgres; skipping module file set")
    else()
        set(GALAY_POSTGRES_CPP23_MODULES_EFFECTIVE ON)
    endif()
endif()

if(GALAY_POSTGRES_CPP23_MODULES_EFFECTIVE)
    if(GALAY_INSTALL_CPP23_MODULE_INTERFACES)
        target_sources(galay-postgres
            PUBLIC
                FILE_SET CXX_MODULES
                BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/module
                FILES ${CMAKE_CURRENT_SOURCE_DIR}/module/galay_postgres.cppm
        )
        set_target_properties(galay-postgres PROPERTIES CXX_SCAN_FOR_MODULES ON)
    else()
        # Keep the module dependency visible to in-tree consumers without
        # exporting a file set whose source interface is not installed.
        add_library(galay-postgres-cpp23-modules OBJECT)
        set_target_properties(galay-postgres-cpp23-modules PROPERTIES
            POSITION_INDEPENDENT_CODE ${GALAY_BUILD_SHARED_LIBS}
        )
        target_compile_features(galay-postgres-cpp23-modules PUBLIC cxx_std_23)
        target_include_directories(galay-postgres-cpp23-modules
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        )
        target_link_libraries(galay-postgres-cpp23-modules
            PUBLIC
                galay::kernel
                galay::utils
        )
        target_sources(galay-postgres-cpp23-modules
            PUBLIC
                FILE_SET CXX_MODULES
                BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/module
                FILES ${CMAKE_CURRENT_SOURCE_DIR}/module/galay_postgres.cppm
        )
        set_target_properties(galay-postgres-cpp23-modules PROPERTIES CXX_SCAN_FOR_MODULES ON)
        target_link_libraries(galay-postgres PUBLIC
            "$<BUILD_INTERFACE:galay-postgres-cpp23-modules>"
        )
    endif()
endif()

set(GALAY_POSTGRES_CPP23_MODULES_EFFECTIVE "${GALAY_POSTGRES_CPP23_MODULES_EFFECTIVE}" PARENT_SCOPE)

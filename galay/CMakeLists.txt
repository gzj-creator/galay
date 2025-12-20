include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# 收集所有源文件
# file(GLOB_RECURSE GALAY_SOURCES
#     "*/*.cc"
#     "*/*.cpp"
#     "*/*.c"
# )

file(GLOB_RECURSE GALAY_SOURCES
    "kernel/*.cc"
    "async/*.cc")

# 收集依赖库
set(DEPEND_LIBS ${SSL_LIB} ${CRYPTO_LIB} pthread)

if(USE_IO_URING)
    find_library(IO_URING_LIB NAMES uring)
    list(APPEND DEPEND_LIBS ${IO_URING_LIB})
elseif(USE_AIO)
    find_library(AIO_LIB NAMES aio)
    list(APPEND DEPEND_LIBS ${AIO_LIB})
endif()

if(${BUILD_STATIC})
    # 创建静态库
    add_library(galay STATIC ${GALAY_SOURCES})
    target_link_libraries(galay PRIVATE ${DEPEND_LIBS})
    if(ENABLE_DEBUG)
        target_compile_options(galay PRIVATE
                            $<$<CONFIG:Debug>:-g>
                            $<$<CONFIG:RealWithDebInfo>:-g>)
    endif()
else()
    # 创建共享库
    add_library(galay SHARED ${GALAY_SOURCES})
    target_link_libraries(galay PRIVATE ${DEPEND_LIBS})
    if(ENABLE_DEBUG)
        target_compile_options(galay PRIVATE
                            $<$<CONFIG:Debug>:-g>
                            $<$<CONFIG:RealWithDebInfo>:-g>)
    endif()
endif()

if(ENABLE_DEBUG)
    # 根据编译器为目标库设置编译选项
    if(COMPILER_IS_CLANG)
        target_compile_options(galay PRIVATE 
            $<$<CONFIG:Debug>:-g3 -O0 -ggdb3 -gdwarf-4 -fstandalone-debug -Wall -Wextra>
        )
    else()
        target_compile_options(galay PRIVATE 
            $<$<CONFIG:Debug>:-g3 -O0 -ggdb3 -gdwarf-4 -Wall -Wextra>
        )
    endif()
    target_link_options(galay PRIVATE 
        $<$<CONFIG:Debug>:-g>
    )
endif()

set_target_properties(galay PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)


install(TARGETS galay DESTINATION lib EXPORT ${PROJECT_NAME}Targets)

# 生成版本配置文件
write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(${CMAKE_SOURCE_DIR}/${PROJECT_NAME}Config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
        INSTALL_DESTINATION
        ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}/
)

install(EXPORT
        ${PROJECT_NAME}Targets
        FILE
        ${PROJECT_NAME}Targets.cmake
        NAMESPACE
        "${PROJECT_NAME}::"
        DESTINATION
        ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
        COMPONENT
        Devel
)

install(
        FILES
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
        DESTINATION
        ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
        COMPONENT
        Devel
)

install(DIRECTORY ${HEADER_SRC_DIR}
        DESTINATION ${HEADER_DST_DIR}
        COMPONENT devel
        FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
        PATTERN "*.inl"
        PATTERN "*.tcc"
)

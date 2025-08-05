cmake_minimum_required(VERSION 3.21)
project(Galay VERSION 1.0)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(HEADER_SRC_DIR galay)
set(HEADER_DST_DIR include)

include(CMakePackageConfigHelpers) 
#查找依赖
include(cmake/dependency.cmake)

if(ENABLE_INSTALL_SYSTEM)
    set(CMAKE_INSTALL_PREFIX /usr/local)
else()
    set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install)
endif()
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")

if(ENABLE_DEBUG)
    set(CMAKE_BUILD_TYPE Debug)
    message(STATUS "Debug mode enabled")
else()
    set(CMAKE_BUILD_TYPE Release)
    message(STATUS "Release mode enabled")
endif()

if(${ENABLE_DEFAULT_USE_EPOLL}) 
  message(STATUS "Enable default use epoll, add -DENABLE_DEFAULT_USE_EPOLL to cmake")
  add_definitions(-DENABLE_DEFAULT_USE_EPOLL)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(DEPEND_LIBS kernel utils algorithm common)


add_compile_options(-fPIC)

include_directories(${PROJECT_SOURCE_DIR})

#添加子目录
add_subdirectory(galay)

# 初始化对象库列表
set(OBJECT_LIBS)

foreach(depend_lib ${DEPEND_LIBS})
    message(STATUS "Processing dependency: ${depend_lib}")
    if(TARGET ${depend_lib})
        list(APPEND OBJECT_LIBS $<TARGET_OBJECTS:${depend_lib}>)
    else()
        message(FATAL_ERROR "Target ${depend_lib}_obj does not exist")
    endif()
endforeach()


if(${BUILD_STATIC})
    # 创建静态库
    add_library(galay STATIC ${OBJECT_LIBS})
    target_link_libraries(galay PRIVATE ${DEPEND_LIBS})
    if(ENABLE_DEBUG)
        target_compile_options(galay PRIVATE 
                            $<$<CONFIG:Debug>:-g>
                            $<$<CONFIG:RealWithDebInfo>:-g>)
    endif()
else()
    # 创建共享库
    add_library(galay SHARED ${OBJECT_LIBS})
    target_link_libraries(galay PRIVATE ${DEPEND_LIBS})
    if(ENABLE_DEBUG)
        target_compile_options(galay PRIVATE 
                            $<$<CONFIG:Debug>:-g>
                            $<$<CONFIG:RealWithDebInfo>:-g>)
    endif()
endif()

set_target_properties(galay PROPERTIES
        CXX_STANDARD 20
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

configure_package_config_file(${PROJECT_NAME}Config.cmake.in
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



add_subdirectory(test)






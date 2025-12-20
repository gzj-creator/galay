cmake_minimum_required(VERSION 3.21)
project(Galay VERSION 1.0)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(HEADER_SRC_DIR galay)
set(HEADER_DST_DIR include)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers) 
#查找依赖
include(cmake/dependency.cmake)

if(ENABLE_INSTALL_SYSTEM)
    set(CMAKE_INSTALL_PREFIX /usr/local)
else()
    set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install)
endif()
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")

# 检测编译器类型
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(COMPILER_IS_CLANG ON)
    message(STATUS "Using Clang compiler")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(COMPILER_IS_GCC ON)
    message(STATUS "Using GCC compiler")
endif()


if(ENABLE_DEBUG)
    set(CMAKE_BUILD_TYPE Debug)

    # 根据编译器设置不同的调试选项
    if(COMPILER_IS_CLANG)
        set(CMAKE_CXX_FLAGS_DEBUG "-g3 -O0 -ggdb3 -gdwarf-4 -fstandalone-debug -Wall -Wextra" CACHE STRING "Debug flags" FORCE)
        set(CMAKE_C_FLAGS_DEBUG "-g3 -O0 -ggdb3 -gdwarf-4 -fstandalone-debug -Wall -Wextra" CACHE STRING "Debug flags" FORCE)
    else()
        set(CMAKE_CXX_FLAGS_DEBUG "-g3 -O0 -ggdb3 -gdwarf-4 -Wall -Wextra" CACHE STRING "Debug flags" FORCE)
        set(CMAKE_C_FLAGS_DEBUG "-g3 -O0 -ggdb3 -gdwarf-4 -Wall -Wextra" CACHE STRING "Debug flags" FORCE)
    endif()
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-g" CACHE STRING "Debug linker flags" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "-g" CACHE STRING "Debug linker flags" FORCE)
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



add_compile_options(-fPIC)

include_directories(${PROJECT_SOURCE_DIR})

if(ENABLE_DEBUG)
    # 根据编译器设置全局编译选项
    if(COMPILER_IS_CLANG)
        add_compile_options(-g3 -O0 -fstandalone-debug)
    else()
        add_compile_options(-g3 -O0)
    endif()
    add_link_options(-g)
endif()

#添加子目录
add_subdirectory(galay)
add_subdirectory(test)
add_subdirectory(benchmark)






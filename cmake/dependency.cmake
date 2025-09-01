
include(cmake/options.cmake)
#openssl
find_package(OpenSSL REQUIRED)

if(OPENSSL_FOUND)
    message(STATUS "Openssl found")
    include_directories(${OPENSSL_INCLUDE_DIR})
    find_library(SSL_LIB NAMES ssl)
    find_library(CRYPTO_LIB NAMES crypto)
else()
    message(FATAL_ERROR "Openssl not found")
endif()

#spdlog 
find_path(  SPDLOG_INCLUDE_DIR 
            NAMES spdlog/spdlog.h 
            PATHS
                /usr/local/include
                /usr/include
                ${CMAKE_PREFIX_PATH}/include
                $ENV{SPDLOG_ROOT}/include
                ${SPDLOG_ROOT}/include
            DOC "The directory where spdlog headers reside"
        )

find_library(   SPDLOG_LIBRARY 
                NAMES spdlog
                PATHS
                    /usr/local/lib
                    /usr/lib
                    ${CMAKE_PREFIX_PATH}/lib
                    $ENV{SPDLOG_ROOT}/lib
                    ${SPDLOG_ROOT}/lib
                DOC "The spdlog library"
            )

if(SPDLOG_INCLUDE_DIR AND SPDLOG_LIBRARY)
    message(STATUS "Spdlog found")
    include_directories(${SPDLOG_INCLUDE_DIR})
    set(SPDLOG_FOUND TRUE)
else()
    message(FATAL_ERROR "Spdlog not found")
endif()

#concurrentqueue
find_path(  LIBCONCURRENTQUEUE_INCLUDE_DIR 
            NAMES concurrentqueue.h
            PATHS
                /usr/local/include/concurrentqueue/moodycamel
                ${CMAKE_PREFIX_PATH}/include
                $ENV{LIBCONCURRENTQUEUE_ROOT}/include
                ${LIBCONCURRENTQUEUE_ROOT}/include
            DOC "The directory where concurrentqueue headers reside"
        )

if(LIBCONCURRENTQUEUE_INCLUDE_DIR)
    message(STATUS "Concurrentqueue found")
    include_directories(${LIBCONCURRENTQUEUE_INCLUDE_DIR})
    set(CONCURRENTQUEUE TRUE)
else()
    message(FATAL_ERROR "concurrentqueue not found")
endif()

# libcuckoo
find_path(  LIBCUCKOO_INCLUDE_DIR 
            NAMES cuckoohash_map.hh
            PATHS
                /usr/local/include/libcuckoo/
                ${CMAKE_PREFIX_PATH}/include
                $ENV{LIBCUCKOO_ROOT}/include
                ${LIBCUCKOO_ROOT}/include
            DOC "The directory where libcuckoo headers reside"
        )

if(LIBCUCKOO_INCLUDE_DIR)
    message(STATUS "libcuckoo found")
    include_directories(${LIBCUCKOO_INCLUDE_DIR})
    set(LIBCUCKOO TRUE)
else()
    message(FATAL_ERROR "libcuckoo not found")
endif()


# linux
# 检测内核版本以决定使用 aio 还是 iouring
function(get_kernel_version)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(STATUS "Not running on Linux. Defaulting to basic I/O operations.")
        set(KERNEL_SUPPORTS_IOURING FALSE PARENT_SCOPE)
        return()
    endif()

    execute_process(
        COMMAND uname -r
        OUTPUT_VARIABLE KERNEL_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    
    string(REGEX MATCH "[0-9]+\\.[0-9]+" KERNEL_VERSION_MAJOR_MINOR "${KERNEL_VERSION}")
    string(REPLACE "." "" KERNEL_VERSION_NUM "${KERNEL_VERSION_MAJOR_MINOR}")
    
    if(${KERNEL_VERSION_NUM} GREATER_EQUAL 54)
        set(KERNEL_SUPPORTS_IOURING TRUE PARENT_SCOPE)
        message(STATUS "Kernel version ${KERNEL_VERSION} supports io_uring")
    else()
        set(KERNEL_SUPPORTS_IOURING FALSE PARENT_SCOPE)
        message(STATUS "Kernel version ${KERNEL_VERSION} does not support io_uring, using aio instead")
    endif()
endfunction()

# 检测内核版本并设置相应的宏定义
get_kernel_version()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # linux
    if(KERNEL_SUPPORTS_IOURING AND NOT ENABLE_DEFAULT_USE_EPOLL)
        # 检查是否安装了 liburing
        find_path(LIBURING_INCLUDE_DIR 
                NAMES liburing.h
                PATHS /usr/local/include /usr/include)
        
        find_library(LIBURING_LIBRARY 
                    NAMES uring
                    PATHS /usr/local/lib /usr/lib)
        
        if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
            message(STATUS "liburing found, using io_uring")
            include_directories(${LIBURING_INCLUDE_DIR})
            set(IO_URING_FOUND TRUE)
            add_definitions(-DUSE_IO_URING)
            set(USE_IO_URING true)
        else()
            message(STATUS "liburing not found, using aio")
            set(IO_URING_FOUND FALSE)
            add_definitions(-DUSE_AIO)
            set(USE_AIO TRUE)
        endif()
    else()
        if(ENABLE_DEFAULT_USE_EPOLL)
            message(STATUS "Using aio due to use epoll")
            add_definitions(-DUSE_AIO)
            set(USE_AIO TRUE)
        else()
            message(STATUS "Using aio due to kernel version")
            add_definitions(-DUSE_AIO)
            set(USE_AIO TRUE)
        endif()
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # mac
    set(ENABLE_DEFAULT_USE_EPOLL FALSE)
endif()


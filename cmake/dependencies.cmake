include_guard(GLOBAL)

function(galay_configure_io_backend out_backend out_platform_libs)
    set(_backend "")
    set(_platform_libs "")

    find_package(Threads REQUIRED)
    list(APPEND _platform_libs Threads::Threads)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(NOT GALAY_DISABLE_IOURING)
            find_path(GALAY_LIBURING_INCLUDE_DIR
                NAMES liburing.h
                HINTS
                    /opt/homebrew/include
                    /usr/local/include
                    /usr/include
            )
            find_library(GALAY_LIBURING_LIBRARY
                NAMES uring
                HINTS
                    /opt/homebrew/lib
                    /usr/local/lib
                    /usr/lib
                    /usr/lib64
            )
        endif()

        if(NOT GALAY_DISABLE_IOURING AND GALAY_LIBURING_INCLUDE_DIR AND GALAY_LIBURING_LIBRARY)
            set(_backend "io_uring")
            list(APPEND _platform_libs "${GALAY_LIBURING_LIBRARY}")
        else()
            set(_backend "epoll")
            find_library(GALAY_LIBAIO_LIBRARY
                NAMES aio
                HINTS
                    /opt/homebrew/lib
                    /usr/local/lib
                    /usr/lib
                    /usr/lib64
            )
            if(GALAY_LIBAIO_LIBRARY)
                list(APPEND _platform_libs "${GALAY_LIBAIO_LIBRARY}")
            endif()
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME MATCHES ".*BSD")
        set(_backend "kqueue")
    else()
        message(FATAL_ERROR "Unsupported Galay I/O platform: ${CMAKE_SYSTEM_NAME}")
    endif()

    set(${out_backend} "${_backend}" PARENT_SCOPE)
    set(${out_platform_libs} "${_platform_libs}" PARENT_SCOPE)
    set(GALAY_KERNEL_BACKEND "${_backend}" PARENT_SCOPE)
    set(PLATFORM_LIBS "${_platform_libs}" PARENT_SCOPE)
endfunction()

function(galay_ensure_concurrentqueue)
    if(TARGET concurrentqueue::concurrentqueue)
        return()
    endif()

    find_package(concurrentqueue CONFIG QUIET)
    if(TARGET concurrentqueue::concurrentqueue)
        return()
    endif()

    set(_galay_concurrentqueue_hints
        "${GALAY_CONCURRENTQUEUE_INCLUDE_DIR}"
        "${GALAY_UTILS_CONCURRENTQUEUE_INCLUDE_DIR}"
        "${GALAY_KERNEL_CONCURRENTQUEUE_INCLUDE_DIR}"
        "${MOODYCAMEL_CONCURRENTQUEUE_INCLUDE_DIR}"
        "$ENV{CONCURRENTQUEUE_ROOT}/include"
        "$ENV{MOODYCAMEL_CONCURRENTQUEUE_ROOT}/include"
        /opt/homebrew/include
        /usr/local/include
        /usr/include
    )

    find_path(GALAY_CONCURRENTQUEUE_INCLUDE_DIR
        NAMES concurrentqueue/moodycamel/concurrentqueue.h
        HINTS ${_galay_concurrentqueue_hints}
        DOC "Include prefix containing concurrentqueue/moodycamel/concurrentqueue.h"
    )

    if(NOT GALAY_CONCURRENTQUEUE_INCLUDE_DIR)
        find_path(MOODYCAMEL_CONCURRENTQUEUE_INCLUDE_DIR
            NAMES concurrentqueue.h
            HINTS ${_galay_concurrentqueue_hints}
            PATH_SUFFIXES concurrentqueue/moodycamel moodycamel
            DOC "Directory containing moodycamel concurrentqueue headers"
        )
        if(MOODYCAMEL_CONCURRENTQUEUE_INCLUDE_DIR)
            get_filename_component(_galay_moodycamel_parent "${MOODYCAMEL_CONCURRENTQUEUE_INCLUDE_DIR}" DIRECTORY)
            get_filename_component(_galay_moodycamel_root "${_galay_moodycamel_parent}" DIRECTORY)
            if(EXISTS "${_galay_moodycamel_root}/concurrentqueue/moodycamel/concurrentqueue.h")
                set(GALAY_CONCURRENTQUEUE_INCLUDE_DIR "${_galay_moodycamel_root}" CACHE PATH
                    "Include prefix containing concurrentqueue/moodycamel/concurrentqueue.h" FORCE)
            endif()
        endif()
    endif()

    if(NOT GALAY_CONCURRENTQUEUE_INCLUDE_DIR)
        message(FATAL_ERROR
            "concurrentqueue headers not found. Set GALAY_CONCURRENTQUEUE_INCLUDE_DIR "
            "to the include prefix containing concurrentqueue/moodycamel/concurrentqueue.h")
    endif()

    set(_galay_concurrentqueue_include_dirs "${GALAY_CONCURRENTQUEUE_INCLUDE_DIR}")
    if(EXISTS "${GALAY_CONCURRENTQUEUE_INCLUDE_DIR}/concurrentqueue/moodycamel/concurrentqueue.h")
        list(APPEND _galay_concurrentqueue_include_dirs
            "${GALAY_CONCURRENTQUEUE_INCLUDE_DIR}/concurrentqueue/moodycamel")
        set(MOODYCAMEL_CONCURRENTQUEUE_INCLUDE_DIR
            "${GALAY_CONCURRENTQUEUE_INCLUDE_DIR}/concurrentqueue/moodycamel"
            CACHE PATH "Directory containing moodycamel concurrentqueue headers" FORCE)
    endif()

    add_library(concurrentqueue::concurrentqueue INTERFACE IMPORTED GLOBAL)
    set_target_properties(concurrentqueue::concurrentqueue PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_galay_concurrentqueue_include_dirs}"
    )

    set(MOODYCAMEL_CONCURRENTQUEUE_INCLUDE_DIR "${MOODYCAMEL_CONCURRENTQUEUE_INCLUDE_DIR}" PARENT_SCOPE)
endfunction()

function(galay_ensure_openssl)
    if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
        return()
    endif()

    set(_galay_openssl_cache_stale OFF)
    if(OPENSSL_INCLUDE_DIR AND NOT EXISTS "${OPENSSL_INCLUDE_DIR}/openssl/err.h")
        set(_galay_openssl_cache_stale ON)
    endif()
    if(OPENSSL_SSL_LIBRARY AND NOT EXISTS "${OPENSSL_SSL_LIBRARY}")
        set(_galay_openssl_cache_stale ON)
    endif()
    if(OPENSSL_CRYPTO_LIBRARY AND NOT EXISTS "${OPENSSL_CRYPTO_LIBRARY}")
        set(_galay_openssl_cache_stale ON)
    endif()

    if(_galay_openssl_cache_stale)
        message(STATUS "Discarding stale cached OpenSSL paths")
        unset(OPENSSL_INCLUDE_DIR CACHE)
        unset(OPENSSL_SSL_LIBRARY CACHE)
        unset(OPENSSL_CRYPTO_LIBRARY CACHE)
    endif()

    if(OPENSSL_ROOT_DIR AND NOT EXISTS "${OPENSSL_ROOT_DIR}/include/openssl/err.h")
        message(STATUS "Discarding stale cached OPENSSL_ROOT_DIR")
        unset(OPENSSL_ROOT_DIR CACHE)
    endif()

    if(APPLE AND NOT OPENSSL_ROOT_DIR)
        foreach(_galay_openssl_prefix IN ITEMS
            /opt/homebrew/opt/openssl@3
            /usr/local/opt/openssl@3
            /opt/homebrew/opt/openssl
            /usr/local/opt/openssl
        )
            if(EXISTS "${_galay_openssl_prefix}/include/openssl/err.h")
                set(OPENSSL_ROOT_DIR "${_galay_openssl_prefix}" CACHE PATH
                    "OpenSSL installation root" FORCE)
                break()
            endif()
        endforeach()
    endif()

    find_package(OpenSSL REQUIRED)

    if(NOT TARGET OpenSSL::SSL OR NOT TARGET OpenSSL::Crypto)
        message(FATAL_ERROR "OpenSSL targets OpenSSL::SSL and OpenSSL::Crypto were not found")
    endif()
endfunction()

function(galay_ensure_simdjson)
    if(TARGET simdjson::simdjson)
        return()
    endif()

    find_package(simdjson CONFIG QUIET)
    if(TARGET simdjson::simdjson)
        return()
    endif()

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GALAY_SIMDJSON QUIET IMPORTED_TARGET simdjson)
        if(TARGET PkgConfig::GALAY_SIMDJSON)
            add_library(simdjson::simdjson INTERFACE IMPORTED GLOBAL)
            set_target_properties(simdjson::simdjson PROPERTIES
                INTERFACE_LINK_LIBRARIES PkgConfig::GALAY_SIMDJSON
            )
            return()
        endif()
    endif()

    find_path(GALAY_SIMDJSON_INCLUDE_DIR
        NAMES simdjson.h
        HINTS
            "$ENV{SIMDJSON_ROOT}/include"
            /opt/homebrew/include
            /usr/local/include
            /usr/include
    )
    find_library(GALAY_SIMDJSON_LIBRARY
        NAMES simdjson
        HINTS
            "$ENV{SIMDJSON_ROOT}/lib"
            /opt/homebrew/lib
            /usr/local/lib
            /usr/lib
            /usr/lib64
    )

    if(NOT GALAY_SIMDJSON_INCLUDE_DIR OR NOT GALAY_SIMDJSON_LIBRARY)
        message(FATAL_ERROR "simdjson not found. Install simdjson or set SIMDJSON_ROOT.")
    endif()

    add_library(simdjson::simdjson UNKNOWN IMPORTED GLOBAL)
    set_target_properties(simdjson::simdjson PROPERTIES
        IMPORTED_LOCATION "${GALAY_SIMDJSON_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${GALAY_SIMDJSON_INCLUDE_DIR}"
    )
endfunction()

function(galay_ensure_spdlog)
    if(TARGET spdlog::spdlog)
        return()
    endif()

    find_package(spdlog CONFIG QUIET)
    if(TARGET spdlog::spdlog)
        return()
    endif()

    if(TARGET spdlog::spdlog_header_only)
        add_library(spdlog::spdlog INTERFACE IMPORTED GLOBAL)
        set_target_properties(spdlog::spdlog PROPERTIES
            INTERFACE_LINK_LIBRARIES spdlog::spdlog_header_only
        )
        return()
    endif()

    find_path(GALAY_SPDLOG_INCLUDE_DIR
        NAMES spdlog/spdlog.h
        HINTS
            "$ENV{SPDLOG_ROOT}/include"
            /opt/homebrew/include
            /usr/local/include
            /usr/include
    )
    find_library(GALAY_SPDLOG_LIBRARY
        NAMES spdlog
        HINTS
            "$ENV{SPDLOG_ROOT}/lib"
            /opt/homebrew/lib
            /usr/local/lib
            /usr/lib
            /usr/lib64
    )

    if(NOT GALAY_SPDLOG_INCLUDE_DIR)
        message(FATAL_ERROR "spdlog not found. Install spdlog or set SPDLOG_ROOT.")
    endif()

    if(GALAY_SPDLOG_LIBRARY)
        add_library(spdlog::spdlog UNKNOWN IMPORTED GLOBAL)
        set_target_properties(spdlog::spdlog PROPERTIES
            IMPORTED_LOCATION "${GALAY_SPDLOG_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${GALAY_SPDLOG_INCLUDE_DIR}"
        )
    else()
        add_library(spdlog::spdlog INTERFACE IMPORTED GLOBAL)
        set_target_properties(spdlog::spdlog PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${GALAY_SPDLOG_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS SPDLOG_HEADER_ONLY=1
        )
    endif()
endfunction()

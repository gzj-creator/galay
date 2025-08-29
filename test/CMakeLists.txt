project(test)
file(GLOB SRC_LIST *.cc)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  list(REMOVE_ITEM SRC_LIST ${CMAKE_CURRENT_SOURCE_DIR}/test_aio.cc)
endif()

set(COMMON_LIB ${SSL_LIB} ${CRYPTO_LIB})

if(ENABLE_SYSTEM_LOG)
  add_definitions(-DENABLE_SYSTEM_LOG)
  if(ENABLE_LOG_TRACE)
    add_definitions(-DENABLE_TRACE)
    message(STATUS "Enable log trace")
  endif()
endif()

foreach(SRC ${SRC_LIST})
    get_filename_component(FILE_NAME ${SRC} NAME_WE)
    # 使用文件名作为可执行文件名
    set(EXECUTABLE_NAME ${FILE_NAME})
    add_executable(${EXECUTABLE_NAME} ${SRC})
    target_link_libraries(${EXECUTABLE_NAME} galay ${COMMON_LIB})
    set_target_properties(${EXECUTABLE_NAME} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        INSTALL_RPATH $ORIGIN/../lib
    )
    list(APPEND INSTALL_BIN ${EXECUTABLE_NAME})

endforeach()

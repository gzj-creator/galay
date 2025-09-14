cmake_minimum_required(VERSION 3.21)
project(kernel)

if(ENABLE_SYSTEM_LOG)
  add_definitions(-DENABLE_SYSTEM_LOG)
  if(ENABLE_LOG_TRACE)
    add_definitions(-DENABLE_TRACE)
    message(STATUS "Enable log trace")
  endif()
endif()

file(GLOB SRC_LIST "*.cc" "coroutine/*.cc" "event/*.cc" "async/*.cc" "time/*.cc" "runtime/*.cc" "server/*.cc" "client/*.cc")

set(DEPEND_LIBS ${SSL_LIB} ${CRYPTO_LIB} pthread)

if(${USE_IO_URING})
  find_library(IO_URING_LIB NAMES uring)
  list(APPEND DEPEND_LIBS ${IO_URING_LIB})
elseif(${USE_AIO})
  find_library(AIO_LIB NAMES aio)
  list(APPEND DEPEND_LIBS ${AIO_LIB})
else()
  
endif()

add_library(${PROJECT_NAME} OBJECT ${SRC_LIST})
target_link_libraries(${PROJECT_NAME} PRIVATE ${DEPEND_LIBS})

install(TARGETS ${PROJECT_NAME} EXPORT GalayTargets)

set_target_properties(${PROJECT_NAME} PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

target_compile_options(kernel PRIVATE -Wno-unused-parameter -Wno-reorder-ctor)
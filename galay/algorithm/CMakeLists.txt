project(algorithm)

if(ENABLE_SYSTEM_LOG)
  add_definitions(-DENABLE_SYSTEM_LOG)
  if(ENABLE_LOG_TRACE)
    add_definitions(-DENABLE_TRACE)
    message(STATUS "Enable log trace")
  endif()
endif()

file(GLOB SRC_LIST "*.cc")

add_library(${PROJECT_NAME} OBJECT ${SRC_LIST})
link_libraries(${PROJECT_NAME} ssl crypto)
install(TARGETS ${PROJECT_NAME} EXPORT GalayTargets)

set_target_properties(${PROJECT_NAME} PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
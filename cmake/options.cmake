option(ENABLE_DEBUG "Enable debug" ON)
option(ENABLE_INSTALL_SYSTEM "Enable install system" ON) 
option(BUILD_STATIC "Build static" OFF)
option(ENABLE_LOG_TRACE "Enable log trace" ON)

#gtest
find_path(GTEST_INCLUDE_DIR NAMES gtest)
find_library(GTEST_LIB NAMES gtest)
if (GTEST_INCLUDE_DIR AND GTEST_LIB) 
  option(ENABLE_GTEST "Enable gtest" ON)
else()
  option(ENABLE_GTEST "Not Enable gtest" OFF)
endif()

#json
find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp)
if(NLOHMANN_JSON_INCLUDE_DIR)
  set(NLOHMANN_JSON_FOUND TRUE) 
endif()

if(NLOHMANN_JSON_FOUND)
  option(ENABLE_NLOHMANN_JSON "Enable nlohmann/json" ON)
else()
  option(ENABLE_NLOHMANN_JSON "Not Enable nlohmann/json" OFF)
endif()
# log
option(ENABLE_TRACE_LOG "Enable trace log" OFF)


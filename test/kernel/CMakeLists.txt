# 测试可执行文件 - 自动扫描目录

# 扫描 test/ 下所有测试源文件，并统一注册到 CTest
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "*.cc")

# 为每个测试文件创建可执行文件
foreach(TEST_SOURCE ${TEST_SOURCES})
    # 获取文件名（不含路径和扩展名）
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)

    # 创建可执行文件
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME} PRIVATE galay::kernel)
    target_include_directories(${TEST_NAME}
        PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src
    )
    if(TEST_NAME STREQUAL "t121_taskresult_storage")
        target_compile_options(${TEST_NAME} PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-Werror=free-nonheap-object>)
    endif()

    add_test(NAME ${TEST_NAME} COMMAND $<TARGET_FILE:${TEST_NAME}>)
    set_tests_properties(${TEST_NAME} PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endforeach()

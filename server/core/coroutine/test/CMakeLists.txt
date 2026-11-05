# core 协程调度器测试

add_executable(test_coroutine
    test_coroutine_scheduler.cpp
)

target_include_directories(test_coroutine PRIVATE
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/../third/libcopp/include
)

target_link_libraries(test_coroutine PRIVATE
    core
    common
    GTest::gtest_main
    pthread
)

gtest_discover_tests(test_coroutine
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    PROPERTIES TIMEOUT 120
)

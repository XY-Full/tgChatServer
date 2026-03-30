cmake_minimum_required(VERSION 3.15)

# ── shm 模块单元测试 ───────────────────────────────────────────────
# 依赖：
#   - core 静态库（包含 shm_*.cpp）
#   - common 静态库（GlobalSpace.cpp + Log.h）
#   - GTest::gtest_main
#   - pthread, rt (shm_open / shm_unlink)

add_executable(test_shm
    test_shm_spinlock.cpp
    test_shm_ringbuffer.cpp
    test_shm_slab.cpp
)

target_include_directories(test_shm PRIVATE
    ${CMAKE_SOURCE_DIR}/core/shm
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/../third
    ${CMAKE_SOURCE_DIR}/../third/nlohmann-json
)

target_link_libraries(test_shm PRIVATE
    core
    common
    GTest::gtest_main
    pthread
    rt          # shm_open / shm_unlink
)

# 让 ctest 自动发现此可执行文件中的所有测试
gtest_discover_tests(test_shm
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    PROPERTIES TIMEOUT 60
)

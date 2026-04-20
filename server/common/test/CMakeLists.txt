cmake_minimum_required(VERSION 3.15)

# ── common 模块单元测试 ────────────────────────────────────────────
add_executable(test_common
    test_helper.cpp
    test_helper_forward.cpp
)

target_include_directories(test_common PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/../third
    ${CMAKE_SOURCE_DIR}/../third/nlohmann-json
    ${CMAKE_SOURCE_DIR}/../public/proto_files
    ${Protobuf_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(test_common PRIVATE
    common
    core
    proto
    GTest::gtest_main
    pthread
    rt
)

gtest_discover_tests(test_common
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    PROPERTIES TIMEOUT 60
)

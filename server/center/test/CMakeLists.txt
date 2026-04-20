cmake_minimum_required(VERSION 3.15)

# ── center 模块单元测试 ────────────────────────────────────────────
# 只编译测试需要的 center 源文件（跳过 CenterService/TcpRegistrar/HttpServer）

add_executable(test_center
    test_load_balancer.cpp
    test_service_registry.cpp
    # LoadBalancer/ServiceInstance 已迁移至 common/
    ${CMAKE_SOURCE_DIR}/common/LoadBalancer.cpp
    ${CMAKE_SOURCE_DIR}/common/ServiceInstance.cpp
    ${CMAKE_SOURCE_DIR}/center/ServiceRegistry.cpp
)

target_include_directories(test_center PRIVATE
    ${CMAKE_SOURCE_DIR}/center
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/../third
    ${CMAKE_SOURCE_DIR}/../third/nlohmann-json
    ${CMAKE_SOURCE_DIR}/../public/proto_files
)

target_link_libraries(test_center PRIVATE
    common
    core
    GTest::gtest_main
    pthread
    rt
)

gtest_discover_tests(test_center
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    PROPERTIES TIMEOUT 60
)

cmake_minimum_required(VERSION 3.15)

# ── connd 模块单元测试 ─────────────────────────────────────────────
# 只编译测试需要的源文件（跳过 ConndService, ConndMsgDispatcher）

add_executable(test_connd
    test_session_manager.cpp
    test_jwt_auth.cpp
    ${CMAKE_SOURCE_DIR}/connd/SessionManager.cpp
    ${CMAKE_SOURCE_DIR}/connd/auth/JwtAuthProvider.cpp
)

target_include_directories(test_connd PRIVATE
    ${CMAKE_SOURCE_DIR}/connd
    ${CMAKE_SOURCE_DIR}/connd/auth
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/core/app
    ${CMAKE_SOURCE_DIR}/../third
    ${CMAKE_SOURCE_DIR}/../third/nlohmann-json
    ${CMAKE_SOURCE_DIR}/../public/proto_files
    ${Protobuf_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(test_connd PRIVATE
    common
    core
    proto
    GTest::gtest_main
    pthread
    rt
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
)

gtest_discover_tests(test_connd
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    PROPERTIES TIMEOUT 60
)

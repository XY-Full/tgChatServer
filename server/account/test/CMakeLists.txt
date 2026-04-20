cmake_minimum_required(VERSION 3.15)

# ── account 模块单元测试 ───────────────────────────────────────────
# 只测试 signJwt 核心逻辑（不依赖 IApp/GlobalSpace/bus）

add_executable(test_account
    test_account_service.cpp
    ${CMAKE_SOURCE_DIR}/connd/auth/JwtAuthProvider.cpp
)

target_include_directories(test_account PRIVATE
    ${CMAKE_SOURCE_DIR}/account
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

target_link_libraries(test_account PRIVATE
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

gtest_discover_tests(test_account
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    PROPERTIES TIMEOUT 60
)

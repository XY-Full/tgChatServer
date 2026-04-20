# account: JWT Token 签发微服务
# 处理 CS_PLAYER_APPLY_TOKEN 请求，签发 HS256 JWT 并通过 bus 发回 connd

file(GLOB_RECURSE ACCOUNT_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)
# 排除 test/ 子目录（避免测试文件混入主 binary）
list(FILTER ACCOUNT_SRC EXCLUDE REGEX ".*/test/.*")

add_executable(account ${ACCOUNT_SRC})

target_include_directories(account
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/
        ${CMAKE_SOURCE_DIR}/core
        ${CMAKE_SOURCE_DIR}/common
        ${CMAKE_SOURCE_DIR}/protobuf/include
        ${CMAKE_SOURCE_DIR}/protobuf/proto_files
        ${CMAKE_SOURCE_DIR}/../third          # nlohmann/json.hpp
        ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(account
    PRIVATE
        core
        common
        proto
)

set_target_properties(account PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/account
)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()

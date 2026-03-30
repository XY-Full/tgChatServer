# 自动收集当前目录所有 .cpp 文件（排除 test 子目录）
file(GLOB COMMON_CPP_FILES "${CMAKE_CURRENT_LIST_DIR}/*.cpp" "${CMAKE_CURRENT_LIST_DIR}/*.h")
file(GLOB PROTO_CC_FILES "${CMAKE_CURRENT_LIST_DIR}/../../third/*.cc")

# 生成静态库
add_library(common STATIC
    ${COMMON_CPP_FILES}
    ${PROTO_CC_FILES}
)

# 添加头文件搜索路径
target_include_directories(common
    PUBLIC 
    ${CMAKE_SOURCE_DIR}/../third
    ${CMAKE_SOURCE_DIR}/../third/nlohmann-json
    ${CMAKE_SOURCE_DIR}/../third/httplib
    ${CMAKE_SOURCE_DIR}/../public/proto_files
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/core/common
)

target_include_directories(common PUBLIC ${Protobuf_INCLUDE_DIRS})


target_link_libraries(common PUBLIC 
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()

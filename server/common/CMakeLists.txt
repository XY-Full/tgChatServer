# 自动收集当前目录所有 .cpp 文件
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
)

target_include_directories(common PUBLIC ${Protobuf_INCLUDE_DIRS})

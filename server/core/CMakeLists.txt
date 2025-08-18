# 自动收集当前目录所有 .cpp 文件
file(GLOB_RECURSE CORE_CPP_FILES "${CMAKE_CURRENT_LIST_DIR}/*.cpp" "${CMAKE_CURRENT_LIST_DIR}/*.h")

# 生成静态库
add_library(core STATIC
    ${CORE_CPP_FILES}
)

# 添加头文件搜索路径
target_include_directories(core
    PUBLIC 
    ${CMAKE_SOURCE_DIR}/../third
    ${CMAKE_SOURCE_DIR}/../third/nlohmann-json
)

target_include_directories(core PUBLIC)
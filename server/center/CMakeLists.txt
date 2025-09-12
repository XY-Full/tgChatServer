# 自动收集当前目录所有 .cpp 文件
file(GLOB_RECURSE CENTER_CPP_FILES "${CMAKE_CURRENT_LIST_DIR}/*.cpp" "${CMAKE_CURRENT_LIST_DIR}/*.h")

add_executable(center ${CENTER_CPP_FILES})

# 添加头文件搜索路径
target_include_directories(center
    PUBLIC 
    ${CMAKE_SOURCE_DIR}/../third
    ${CMAKE_SOURCE_DIR}/../third/nlohmann-json
    ${CMAKE_SOURCE_DIR}/../third/libcopp/include
    ${CMAKE_SOURCE_DIR}/../public/proto_files
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/core/common
)

target_link_libraries(center
    PRIVATE
	core
	common
	proto
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
)
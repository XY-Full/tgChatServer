
cmake_minimum_required(VERSION 3.10)
project(Server)
set(CMAKE_CXX_STANDARD 17)

# Protobuf 路径
set(PROTOBUF_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/../third/protobuf/include)
set(PROTOBUF_LIB_DIR     ${CMAKE_SOURCE_DIR}/../third/protobuf/lib)
link_directories(${PROTOBUF_LIB_DIR})
include_directories(${PROTOBUF_INCLUDE_DIR})

find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# 各子模块
add_subdirectory(../public proto)
add_subdirectory(network)
add_subdirectory(busd)
add_subdirectory(logic)
add_subdirectory(common)
add_subdirectory(core)

get_target_property(curl_location CURL::libcurl LOCATION)
message(STATUS "CURL lib location: ${curl_location}")

# 可执行文件
add_executable(server main.cpp)
target_link_libraries(server
    PRIVATE network
            busd_core
            core
            logic_modules
            proto
            common
            OpenSSL::SSL 
            OpenSSL::Crypto 
            pthread
            CURL::libcurl
)


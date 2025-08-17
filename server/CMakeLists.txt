
cmake_minimum_required(VERSION 3.10)
project(Server)
set(CMAKE_CXX_STANDARD 17)

# 指定第三方库的根目录
set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/../third)

# 告诉 CMake 去这些目录下找包
list(APPEND CMAKE_PREFIX_PATH
    ${THIRD_PARTY_DIR}/openssl
    ${THIRD_PARTY_DIR}/protobuf
    ${THIRD_PARTY_DIR}/curl
)

# Protobuf 路径
set(PROTOBUF_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/../third/protobuf/include)
set(PROTOBUF_LIB_DIR     ${CMAKE_SOURCE_DIR}/../third/protobuf/lib)
link_directories(${PROTOBUF_LIB_DIR})
include_directories(${PROTOBUF_INCLUDE_DIR})

# 设置 OpenSSL 路径
set(OPENSSL_ROOT_DIR ${CMAKE_SOURCE_DIR}/../third/openssl)
set(OPENSSL_INCLUDE_DIR ${OPENSSL_ROOT_DIR}/include)
set(OPENSSL_LIBRARIES ${OPENSSL_ROOT_DIR}/lib)

find_package(OpenSSL REQUIRED)

if (OpenSSL_FOUND)
    message(STATUS "Found OpenSSL: ${OPENSSL_LIBRARIES}")
    include_directories(${OPENSSL_INCLUDE_DIR})
    link_directories(${OPENSSL_ROOT_DIR}/lib)
endif()

# 查找 curl 模块
find_package(CURL REQUIRED)

# 各子模块
add_subdirectory(../public proto)
add_subdirectory(network)
add_subdirectory(busd)
add_subdirectory(logic)
add_subdirectory(common)

get_target_property(curl_location CURL::libcurl LOCATION)
message(STATUS "CURL lib location: ${curl_location}")

# 可执行文件
add_executable(server main.cpp)
target_link_libraries(server
    PRIVATE network
            busd_core
            logic_modules
            proto
            common
            OpenSSL::SSL 
            OpenSSL::Crypto 
            pthread
            CURL::libcurl
)


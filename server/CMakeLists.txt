cmake_minimum_required(VERSION 3.10)
project(Server LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 第三方库根目录
set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/../third)

# 优先找静态库，动态库做保底
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a;.so")

list(APPEND CMAKE_PREFIX_PATH
    ${THIRD_PARTY_DIR}
    ${THIRD_PARTY_DIR}/openssl
    ${THIRD_PARTY_DIR}/protobuf
    ${THIRD_PARTY_DIR}/curl
    ${THIRD_PARTY_DIR}/zlib
)

# -----------------------------
# Protobuf
# -----------------------------
set(Protobuf_USE_STATIC_LIBS ON)
find_package(Protobuf REQUIRED)
if(Protobuf_FOUND)
    message(STATUS "Found Protobuf: ${Protobuf_INCLUDE_DIRS}")
    include_directories(${Protobuf_INCLUDE_DIRS})
endif()

# -----------------------------
# OpenSSL
# -----------------------------
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)
if(OpenSSL_FOUND)
    message(STATUS "Found OpenSSL: ${OpenSSL_VERSION}")
    include_directories(${OPENSSL_INCLUDE_DIR})
endif()

# -----------------------------
# CURL
# -----------------------------
set(CURL_USE_STATIC_LIBS TRUE)
find_package(CURL REQUIRED)
if(CURL_FOUND)
    message(STATUS "Found CURL: ${CURL_LIBRARY}")
    include_directories(${CURL_INCLUDE_DIRS})
endif()

# -----------------------------
# ZLIB
# -----------------------------
set(ZLIB_USE_STATIC_LIBS TRUE)
find_package(ZLIB REQUIRED)
if(ZLIB_FOUND)
    message(STATUS "Found zlib: ${ZLIB_LIBRARY}")
    include_directories(${ZLIB_INCLUDE_DIRS})
endif()

# -----------------------------
# 添加子模块
# -----------------------------
add_subdirectory(../public proto)
add_subdirectory(network)
add_subdirectory(busd)
add_subdirectory(logic)
add_subdirectory(common)
add_subdirectory(core)

# -----------------------------
# 可执行文件
# -----------------------------
add_executable(server main.cpp)

target_link_libraries(server
    PRIVATE
        proto
        network
        logic_modules
        pthread
)

# -----------------------------
# 平台特定依赖
# -----------------------------
if(APPLE)
    message(STATUS "Configuring for macOS")
    target_link_libraries(server PRIVATE
        "-framework CoreFoundation"
        "-framework Security"
        "-framework SystemConfiguration"
        "iconv"
    )
else()
    message(STATUS "Configuring for Linux")
    target_link_libraries(server PRIVATE
        pthread
        dl
    )
endif()

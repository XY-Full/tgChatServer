cmake_minimum_required(VERSION 3.15)
project(Server LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 启动 ccache
set(CMAKE_USE_CCACHE ON)
# 生成 compile_commands.json 文件来为 clangd 工具提供语法支持
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------
# 基础配置
# -----------------------------
add_compile_definitions(_REENTRANT)                # 在编译时启用线程安全的标准C库函数
add_definitions(-DCPNAME=${CPNAME})                # 编译信息
add_definitions(-DCPTIME=${CURRENT_DATE})          # 编译日期

# 设置公共编译参数, 这些参数会追加到现有编译参数后
# -w                            # 如果觉得waring 太多太烦影响调试, 开启这个就只会显示 Error
# -Wextra                       # 启用一些额外的警告信息
# -Wno-reorder                  # 不产生关于重排列数据成员的警告
# -Wreturn-type                 # 不产生检测到没有返回值时产生的告警
# -Wno-deprecated               # 不产生有关使用被弃用的函数或变量的警告
# -Wno-unused-variable          # 不产生关于未使用的变量的警告
# -Wno-unused-parameter         # 不产生关于未使用函数参数的警告
# -Wno-deprecated-declarations  # 不产生使用『已弃用（deprecated）』的函数或变量时产生警告信息
set(CMAKE_CXX_FLAGS "\
  -Wno-reorder \
  -Wreturn-type \
  -Wno-deprecated \
  -Wno-unused-variable \
  -Wno-unused-parameter \
  -Wno-deprecated-declarations \
  $ENV{CFLAGS}\
")
# TODO: 综合考虑一下, 以上需要开启哪些

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
    ${THIRD_PARTY_DIR}/libcopp
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
# LIBCOPP
# -----------------------------
set(LIBCOPP_USE_STATIC_LIBS TRUE)
find_package(LIBCOPP REQUIRED)
if(LIBCOPP_FOUND)
    message(STATUS "Found libcopp: ${LIBCOPP_LIBRARY}")
    include_directories(${LIBCOPP_INCLUDE_DIRS})
endif()

# -----------------------------
# 添加子模块
# -----------------------------
add_subdirectory(../public proto)
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
    network
	busd_core
	core
    logic_modules
	common
	proto
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
	    CURL::libcurl
	    OpenSSL::SSL
        OpenSSL::Crypto
	    ZLIB::ZLIB
        pthread
        dl
    )
endif()

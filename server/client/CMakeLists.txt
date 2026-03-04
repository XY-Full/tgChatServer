cmake_minimum_required(VERSION 3.15)
project(ConndClient CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 路径配置（相对于 server/client/）────────────────────────────────
set(PROTO_FILES_DIR ${CMAKE_SOURCE_DIR}/../../public/proto_files)
set(PROTOBUF_DIR    ${CMAKE_SOURCE_DIR}/../../third/protobuf)

# ── 头文件路径 ───────────────────────────────────────────────────────
include_directories(
    ${PROTO_FILES_DIR}
    ${PROTOBUF_DIR}/include
)

# ── 链接库路径 ───────────────────────────────────────────────────────
link_directories(${PROTOBUF_DIR}/lib)

# ── 可执行文件 ───────────────────────────────────────────────────────
add_executable(client
    main.cpp
    ${PROTO_FILES_DIR}/login.pb.cc
    ${PROTO_FILES_DIR}/gate.pb.cc
    ${PROTO_FILES_DIR}/err_code.pb.cc
)

target_link_libraries(client
    PRIVATE
        protobuf
        pthread
)

set_target_properties(client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}
)

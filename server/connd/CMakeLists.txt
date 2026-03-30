# connd: Connection Daemon
# Handles TCP / WebSocket / KCP client connections with JWT auth

# ── C++ sources (everything under connd/, excluding test/) ─────────────────
file(GLOB_RECURSE CONND_CXX_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)
list(FILTER CONND_CXX_SRC EXCLUDE REGEX ".*/test/.*")

# ── KCP C source (plain C, compiled with the C compiler) ───────────────────
set(KCP_SRC ${CMAKE_SOURCE_DIR}/../third/kcp/ikcp.c)
set_source_files_properties(${KCP_SRC} PROPERTIES LANGUAGE C)

add_executable(connd ${CONND_CXX_SRC} ${KCP_SRC})

target_include_directories(connd
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/
        ${CMAKE_SOURCE_DIR}/core
        ${CMAKE_SOURCE_DIR}/busd
        ${CMAKE_SOURCE_DIR}/protobuf/include
        ${CMAKE_SOURCE_DIR}/protobuf/proto_files
        ${CMAKE_SOURCE_DIR}/../third          # nlohmann/json.hpp, kcp/ikcp.h
        ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(connd
    PRIVATE
        core
        common
        proto
)

set_target_properties(connd PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/connd
)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()

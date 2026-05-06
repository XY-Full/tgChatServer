file(GLOB_RECURSE DBPROXY_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_executable(dbproxy ${DBPROXY_SRC})

target_include_directories(dbproxy
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/
        ${CMAKE_SOURCE_DIR}/core
        ${CMAKE_SOURCE_DIR}/common
        /usr/include                  # for <mariadb/mysql.h>
)

target_link_libraries(dbproxy
    PRIVATE
        core
        common
        proto
        mariadb                       # libmariadb (MySQL 协议兼容)
)

set_target_properties(dbproxy PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/dbproxy
)

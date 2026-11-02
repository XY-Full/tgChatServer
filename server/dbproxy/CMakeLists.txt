file(GLOB_RECURSE DBPROXY_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

list(FILTER DBPROXY_SRC EXCLUDE REGEX ".*/test/.*")

add_executable(dbproxy ${DBPROXY_SRC})

target_compile_definitions(dbproxy PRIVATE
    MARIADB_PLUGIN_DIR="${THIRD_PARTY_DIR}/libmariadb/lib/mariadb/plugin"
)

target_include_directories(dbproxy
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/
        ${CMAKE_SOURCE_DIR}/core
        ${CMAKE_SOURCE_DIR}/common
        ${THIRD_PARTY_DIR}/libmariadb/include
)

target_link_libraries(dbproxy
    PRIVATE
        core
        common
        proto
        ${THIRD_PARTY_DIR}/libmariadb/lib/mariadb/libmariadb.a
        ssl crypto z dl pthread m
)

set_target_properties(dbproxy PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/dbproxy
)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()

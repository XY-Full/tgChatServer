# dbproxy 模块测试

add_executable(test_dbproxy
    test_async_prepared.cpp
    test_tx_timeout_recovery.cpp
    test_auth_tls.cpp
    test_stress.cpp
    ${CMAKE_SOURCE_DIR}/dbproxy/AsyncMySql.cpp
    ${CMAKE_SOURCE_DIR}/dbproxy/DbIoLoop.cpp
    ${CMAKE_SOURCE_DIR}/dbproxy/DbWorkerPool.cpp
    ${CMAKE_SOURCE_DIR}/dbproxy/MySQLConnection.cpp
    ${CMAKE_SOURCE_DIR}/dbproxy/MySQLConnectionPool.cpp
    ${CMAKE_SOURCE_DIR}/dbproxy/StmtParams.cpp
    ${CMAKE_SOURCE_DIR}/dbproxy/TransactionManager.cpp
)

target_compile_definitions(test_dbproxy PRIVATE
    MARIADB_PLUGIN_DIR="${THIRD_PARTY_DIR}/libmariadb/lib/mariadb/plugin"
)

target_include_directories(test_dbproxy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/dbproxy
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/core
    ${THIRD_PARTY_DIR}/libmariadb/include
)

target_link_libraries(test_dbproxy PRIVATE
    common
    ${THIRD_PARTY_DIR}/libmariadb/lib/mariadb/libmariadb.a
    ssl crypto z dl pthread m
    GTest::gtest_main
)

gtest_discover_tests(test_dbproxy
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    PROPERTIES TIMEOUT 600
)

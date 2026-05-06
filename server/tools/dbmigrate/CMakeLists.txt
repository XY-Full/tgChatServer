file(GLOB_RECURSE DBMIGRATE_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_executable(dbmigrate ${DBMIGRATE_SRC})

target_include_directories(dbmigrate
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/
        /usr/include                  # for <mariadb/mysql.h>
)

target_link_libraries(dbmigrate
    PRIVATE
        mariadb
        OpenSSL::Crypto
        OpenSSL::SSL
)

set_target_properties(dbmigrate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/tools
)

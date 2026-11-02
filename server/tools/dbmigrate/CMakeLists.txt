file(GLOB_RECURSE DBMIGRATE_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_executable(dbmigrate ${DBMIGRATE_SRC})

target_include_directories(dbmigrate
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/
        ${CMAKE_SOURCE_DIR}/../public/proto_files
        ${THIRD_PARTY_DIR}/libmariadb/include
)

target_link_libraries(dbmigrate
    PRIVATE
        ${THIRD_PARTY_DIR}/libmariadb/lib/mariadb/libmariadb.a
        ssl crypto z dl pthread m
        OpenSSL::Crypto
        OpenSSL::SSL
)

set_target_properties(dbmigrate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/tools
)

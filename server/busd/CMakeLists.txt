file(GLOB BUSD_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_library(busd STATIC
    ${BUSD_SRC}
)

target_include_directories(busd
                            PUBLIC 
                            ${CMAKE_SOURCE_DIR}/busd
                            ${CMAKE_SOURCE_DIR}/network
                            ${CMAKE_SOURCE_DIR}/protobuf/include
                            ${CMAKE_SOURCE_DIR}/../public/proto_files
                            ${CMAKE_SOURCE_DIR}/common
                            ${CMAKE_SOURCE_DIR}/core/bus
                            ${CMAKE_SOURCE_DIR}/core/common
                            ${CMAKE_SOURCE_DIR}/core
                            )
                            
target_include_directories(busd PUBLIC common network)

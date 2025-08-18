file(GLOB_RECURSE LOGIC_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_library(logic_modules STATIC
    ${LOGIC_SRC}
)


target_include_directories(logic_modules 
                            PUBLIC 
                            ${CMAKE_CURRENT_SOURCE_DIR}/
                            ${CMAKE_SOURCE_DIR}/logic
                            ${CMAKE_SOURCE_DIR}/busd
                            ${CMAKE_SOURCE_DIR}/protobuf/include
                            ${CMAKE_SOURCE_DIR}/protobuf/proto_files
                            )
                            
target_link_libraries(logic_modules PUBLIC common busd_core ${Protobuf_LIBRARY})

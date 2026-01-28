file(GLOB_RECURSE LOGIC_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_executable(logic ${LOGIC_SRC})


target_include_directories(logic
                            PUBLIC 
                            ${CMAKE_CURRENT_SOURCE_DIR}/
                            ${CMAKE_SOURCE_DIR}/core
                            ${CMAKE_SOURCE_DIR}/busd
                            ${CMAKE_SOURCE_DIR}/protobuf/include
                            ${CMAKE_SOURCE_DIR}/protobuf/proto_files
                            )
                            
 
target_link_libraries(logic
    PRIVATE
	core
	common
	proto
)

set_target_properties(logic PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/logic
)
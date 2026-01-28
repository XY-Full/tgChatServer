file(GLOB BUSD_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_executable(busd ${BUSD_SRC})

target_include_directories(busd
                            PUBLIC 
                            ${CMAKE_CURRENT_SOURCE_DIR}
                            ${CMAKE_SOURCE_DIR}/protobuf/include
                            ${CMAKE_SOURCE_DIR}/../public/proto_files
                            ${CMAKE_SOURCE_DIR}/common
                            ${CMAKE_SOURCE_DIR}/core
                            )
      
target_link_libraries(busd
    PRIVATE
	core
	common
	proto
)

set_target_properties(busd PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/busd
)
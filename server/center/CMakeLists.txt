# 自动收集当前目录所有 .cpp 文件（排除 test 子目录）
file(GLOB_RECURSE CENTER_CPP_FILES "${CMAKE_CURRENT_LIST_DIR}/*.cpp" "${CMAKE_CURRENT_LIST_DIR}/*.h")
list(FILTER CENTER_CPP_FILES EXCLUDE REGEX ".*/test/.*")

add_executable(center ${CENTER_CPP_FILES})

# 添加头文件搜索路径
target_include_directories(center
    PUBLIC 
    ${CMAKE_SOURCE_DIR}/../third
    ${CMAKE_SOURCE_DIR}/../third/nlohmann-json
    ${CMAKE_SOURCE_DIR}/../third/libcopp/include
    ${CMAKE_SOURCE_DIR}/../public/proto_files
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/core
)

target_link_libraries(center
    PRIVATE
	core
	common
	proto
)

set_target_properties(center PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/center
)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
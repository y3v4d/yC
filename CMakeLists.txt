cmake_minimum_required(VERSION 3.20)
project(app C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(INC_DIR ${CMAKE_SOURCE_DIR}/include)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

file(GLOB SOURCES ${SRC_DIR}/*.c ${SRC_DIR}/*.h)

add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/wasmtime/crates/c-api
    ${CMAKE_CURRENT_BINARY_DIR}/wasmtime
)

add_executable(app ${SOURCES})
target_include_directories(app PRIVATE ${INC_DIR})

target_include_directories(app PUBLIC wasmtime)
target_link_libraries(app PUBLIC wasmtime)

target_compile_options(app PRIVATE -Wall -Wextra)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(app PRIVATE -g)
endif()

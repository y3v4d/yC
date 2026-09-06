cmake_minimum_required(VERSION 3.20)
project(app C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(INC_DIR ${CMAKE_SOURCE_DIR}/include)

# Put binaries in build/ regardless of build type, to mirror the Makefile.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

file(GLOB SOURCES ${SRC_DIR}/*.c ${SRC_DIR}/*.h)

# All sources (including our own malloc.c / printf.c / string.c) are built
# for BOTH targets. On macOS, symbols defined in a linked object file take
# priority over the same symbol exported by libSystem.dylib, so our
# implementations simply override the system ones — no duplicate-symbol
# error, and you get to debug your own malloc/printf natively with lldb.
add_executable(app ${SOURCES})
target_include_directories(app PRIVATE ${INC_DIR})
target_compile_options(app PRIVATE -Wall -Wextra)

# Native (e.g. macOS) debug-friendly build.
target_compile_options(app PRIVATE -g)

cmake_minimum_required(VERSION 3.16)

project(cmem_cma LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(cmem_cma STATIC
    src/cmem_cma_buffer.cpp
)

target_include_directories(cmem_cma
    PUBLIC
        ${PROJECT_SOURCE_DIR}/headers
)

cmake_minimum_required(VERSION 3.16)

project(cmem_cma LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

add_library(cmem_cma STATIC
    src/cmem_cma_buffer.cpp
)

target_include_directories(cmem_cma
    PUBLIC
        ${PROJECT_SOURCE_DIR}/headers
)

add_executable(cmem_cma_example_buffer examples/buffer.cpp)
target_link_libraries(cmem_cma_example_buffer PRIVATE cmem_cma)

add_executable(cmem_cma_example_vector examples/vector.cpp)
target_link_libraries(cmem_cma_example_vector PRIVATE cmem_cma)

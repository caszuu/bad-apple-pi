cmake_minimum_required(VERSION 3.5)

project(bv LANGUAGES C)
add_library(bv INTERFACE)

target_sources(bv INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/bvdec.c
)

target_include_directories(bv INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
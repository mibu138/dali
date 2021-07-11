cmake_minimum_required(VERSION 3.17)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/cmake-common)

project(Dali VERSION 0.1.0)

include(header)

find_package(Obsidian REQUIRED)
find_package(Coal REQUIRED)

add_subdirectory(cmake)
add_subdirectory(src/lib)
add_subdirectory(src/shaders)
add_subdirectory(src/bin)

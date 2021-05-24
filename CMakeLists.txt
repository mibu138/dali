cmake_minimum_required(VERSION 3.17)

project(Painter VERSION 1.0.0)

include(GNUInstallDirs)

include(cmake/shaders.cmake)

add_subdirectory(cmake)

find_package(Obsidian REQUIRED FATAL_ERROR)

add_subdirectory(src/lib)
add_subdirectory(src/bin)

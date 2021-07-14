cmake_minimum_required(VERSION 3.17)

project(Dali VERSION 0.1.0)

include(FetchContent)
FetchContent_GetProperties(cmake-common)
if(NOT ${cmake-common_POPULATED})
    FetchContent_Declare(
        cmake-common
        GIT_REPOSITORY https://github.com/mokchira/cmake-common
        GIT_TAG origin/master)
    FetchContent_Populate(cmake-common)
endif()
list(APPEND CMAKE_MODULE_PATH ${cmake-common_SOURCE_DIR})

include(header)

find_package(Obsidian REQUIRED)
find_package(Coal REQUIRED)

add_subdirectory(cmake)
add_subdirectory(src/lib)
add_subdirectory(src/shaders)
add_subdirectory(src/bin)

cmake_minimum_required(VERSION 3.10)

project(painter)

if (NOT DEFINED GLOBAL_CMAKE_PATH)
    include(/home/michaelb/dev/cmake/global.cmake)
else()
    include({GLOBAL_CMAKE_PATH})
endif()

set(LIBRARY_NAME painter)

add_subdirectory(src)

cmake_minimum_required(VERSION 3.17)

project(Dali VERSION 0.1.0)

option(DALI_SKIP_EXECUTABLE "Skip building executable" OFF)

if(NOT DEFINED GIT_BASE_URL)
    set(GIT_BASE_URL https://github.com/mokchira)
endif()

include(FetchContent)
FetchContent_Declare(
    cmake-common
    GIT_REPOSITORY ${GIT_BASE_URL}/cmake-common
    GIT_TAG master)
FetchContent_GetProperties(cmake-common)
if(NOT ${cmake-common_POPULATED})
    FetchContent_Populate(cmake-common)
endif()
list(APPEND CMAKE_MODULE_PATH ${cmake-common_SOURCE_DIR})

include(header)

FetchContent_Declare(
    obsidian 
    GIT_REPOSITORY ${GIT_BASE_URL}/obsidian 
    GIT_TAG master)

FetchContent_MakeAvailable(obsidian)

add_subdirectory(src/lib)
add_subdirectory(src/shaders)
if(NOT ${DALI_SKIP_EXECUTABLE})
    add_subdirectory(src/bin)
endif()

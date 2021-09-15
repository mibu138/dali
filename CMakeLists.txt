cmake_minimum_required(VERSION 3.17)

project(Dali VERSION 0.1.0)

option(DALI_SKIP_EXECUTABLE "Skip building executable" OFF)

if(NOT DEFINED GIT_BASE_URL)
    set(GIT_BASE_URL https://github.com/mokchira)
endif()

include(FetchContent)

FetchContent_Declare(
    obsidian 
    GIT_REPOSITORY /home/michael/dev/obsidian 
    GIT_TAG master)

FetchContent_MakeAvailable(obsidian)

# So we can use the author_shaders cmake function 
list(APPEND CMAKE_MODULE_PATH ${obsidian_SOURCE_DIR}/cmake)

add_subdirectory(src/lib)
add_subdirectory(src/shaders)
if(NOT ${DALI_SKIP_EXECUTABLE})
    add_subdirectory(src/bin)
endif()

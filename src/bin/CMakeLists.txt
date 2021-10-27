if (NOT DEFINED SHIV_URL)
    set(SHIV_URL https://github.com/mokchira/shiv)
endif()

FetchContent_Declare(
    shiv
    GIT_REPOSITORY ${SHIV_URL}
    GIT_TAG master)
set(SHIV_SKIP_EXAMPLES TRUE)

FetchContent_Declare(
    leaves 
    GIT_REPOSITORY /home/michaelb/dev/leaves 
    GIT_TAG dali)

FetchContent_MakeAvailable(shiv leaves)

add_executable(paint painter-bin.c)
target_link_libraries(paint Dali::Dali Shiv::Shiv Leaves::Leaves)

if(WIN32)
set_target_properties(paint PROPERTIES WIN32_EXECUTABLE TRUE)
else()
set_target_properties(paint PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

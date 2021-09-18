if (NOT DEFINED SHIV_URL)
    set(SHIV_URL https://github.com/mokchira/shiv)
endif()

FetchContent_Declare(
    shiv
    GIT_REPOSITORY ${SHIV_URL}
    GIT_TAG dali-mask32support)
set(SHIV_SKIP_EXAMPLES TRUE)

FetchContent_MakeAvailable(shiv)

add_executable(paint painter-bin.c)
target_link_libraries(paint Dali::Dali Shiv::Shiv)
set_target_properties(paint PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(WIN32)
set_target_properties(paint PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

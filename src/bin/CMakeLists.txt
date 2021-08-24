
FetchContent_Declare(
    shiv
    GIT_REPOSITORY ${git_base_url}/shiv
    GIT_TAG master)

FetchContent_MakeAvailable(shiv)

add_executable(paint painter-bin.c)
target_link_libraries(paint Dali::Dali Shiv::Shiv)
set_target_properties(paint PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(WIN32)
set_target_properties(paint PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

#$install(TARGETS paint DESTINATION ${CMAKE_INSTALL_BINDIR})
#//
#add_executable(exr2aces main.cpp)
#target_link_libraries(exr2aces OpenEXR::OpenEXR)
#set_target_properties(exr2aces PROPERTIES
#  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
#)
#install(TARGETS exr2aces DESTINATION ${CMAKE_INSTALL_BINDIR})
#if(WIN32 AND (BUILD_SHARED_LIBS OR OPENEXR_BUILD_BOTH_STATIC_SHARED))
#  target_compile_definitions(exr2aces PRIVATE OPENEXR_DLL)
#endif()
#


add_executable(paint painter-bin.c)
find_package(Shiv REQUIRED)
target_link_libraries(paint Dali::Dali Shiv::Shiv)
set_target_properties(paint PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS paint DESTINATION ${CMAKE_INSTALL_BINDIR})

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

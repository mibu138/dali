add_executable(painter-bin painter-bin.c)
target_link_libraries(painter-bin Painter::Painter)
set_target_properties(painter-bin PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS painter-bin DESTINATION ${CMAKE_INSTALL_BINDIR})

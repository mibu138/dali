set(SRCS 
    layer.c
    paint.c 
    painter.c
    render.c
    undo.c)

set(PUBLIC_HEADERS
    common.h
    dtags.h
    g_api.h
    g_houdini_api.h
    layer.h
    painter.h
    paint.h
    render.h
    undo.h)

#add_library(chalkboard SHARED g_chalkboard.c)
#add_library(standalone SHARED g_standalone.c)

add_library(painter SHARED ${SRCS})

set(CURDIR ${CMAKE_CURRENT_SOURCE_DIR})

target_include_directories(painter INTERFACE $<BUILD_INTERFACE:${CURDIR}>)
target_link_libraries(painter PUBLIC ${PROJECT_NAME}::Config Obsidian::Obsidian)

#target_include_directories(painter
#    PUBLIC
#    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
#    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
#
set_target_properties(painter PROPERTIES PUBLIC_HEADER "${PUBLIC_HEADERS}")
set_target_properties(painter PROPERTIES EXPORT_NAME Painter)
set_target_properties(painter PROPERTIES
      OUTPUT_NAME painter 
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_library(${PROJECT_NAME}::Painter ALIAS painter)

install(TARGETS painter EXPORT ${PROJECT_NAME}Targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/painter)

set(SHADER_TARGET_NAME painter-shaders)
add_subdirectory(shaders)
add_dependencies(painter painter-shaders)


set(SRCS 
    layer.c
    main.c
    paint.c painter.c
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

add_library(chalkboard SHARED g_chalkboard.c)
#add_library(houdini SHARED g_houdini.c)
add_library(standalone SHARED g_standalone.c)
add_library(painter SHARED ${SRCS})

set_target_properties(painter PROPERTIES PUBLIC_HEADER "${PUBLIC_HEADERS}")
#target_link_libraries(houdini coal hell obsidian)
target_link_libraries(painter coal hell obsidian vulkan)

if (NOT DEFINED LIB_ONLY)
    add_executable(painter-bin main.c)
    target_link_libraries(painter-bin painter)
    add_dependencies(painter-bin standalone chalkboard)
endif()

add_subdirectory(shaders)
add_dependencies(painter painter-shaders)

install(TARGETS ${LIBRARY_NAME} 
    LIBRARY DESTINATION ${LIB_INSTALL_PREFIX})

if (NOT DEFINED NO_HEADER_INSTALL)
    install(TARGETS ${LIBRARY_NAME} 
        PUBLIC_HEADER DESTINATION ${HEADER_INSTALL_PREFIX}${LIBRARY_NAME})
endif()

if (NOT DEFINED LIB_ONLY)
    install(TARGETS painter-bin)
endif()

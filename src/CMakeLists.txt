set(SRCS 
    layer.c
    main.c
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

add_library(chalkboard SHARED g_chalkboard.c)
add_library(houdini SHARED g_houdini.c)
add_library(standalone SHARED g_standalone.c)
add_library(painter SHARED ${SRCS})
add_executable(painter-bin main.c)
set_target_properties(painter PROPERTIES PUBLIC_HEADER "${PUBLIC_HEADERS}")
target_link_libraries(painter hell obsidian vulkan)
target_link_libraries(painter-bin painter)
add_subdirectory(shaders)
add_dependencies(painter shaders)
add_dependencies(painter-bin standalone chalkboard)
install(TARGETS painter 
    LIBRARY DESTINATION lib 
    PUBLIC_HEADER DESTINATION include/painter)

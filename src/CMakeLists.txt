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

add_library(${LIBRARY_NAME} SHARED ${SRCS})

target_link_libraries(${LIBRARY_NAME} PRIVATE coal hell obsidian vulkan)

install_library()

if (NOT DEFINED LIB_ONLY)
    add_executable(painter-bin main.c)
    target_link_libraries(painter-bin ${LIBRARY_NAME})
    add_dependencies(painter-bin standalone chalkboard)
    install(TARGETS painter-bin)
endif()

set(SHADER_TARGET_NAME painter-shaders)
add_subdirectory(shaders)
add_dependencies(${LIBRARY_NAME} painter-shaders)


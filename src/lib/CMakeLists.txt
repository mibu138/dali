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

include(author_library)
author_library(painter
    EXPORT_NAME Painter
    SOURCES ${SRCS}
    PUBLIC_HEADERS ${PUBLIC_HEADERS}
    DEPS Obsidian::Obsidian)

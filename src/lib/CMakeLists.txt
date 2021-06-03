set(SRCS 
    layer.c
    engine.c 
    brush.c
    undo.c)

set(PUBLIC_HEADERS
    common.h
    dtags.h
    g_api.h
    g_houdini_api.h
    layer.h
    brush.h
    engine.h
    undo.h)

include(author_library)
author_library(dali
    EXPORT_NAME Dali
    SOURCES ${SRCS}
    PUBLIC_HEADERS ${PUBLIC_HEADERS}
    DEPS Obsidian::Obsidian)

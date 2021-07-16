set(SRCS 
    layer.c
    engine.c 
    brush.c
    undo.c)

set(PUBLIC_HEADERS
    dali.h
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

author_library(daliObj
    TYPE OBJECT
    EXPORT_NAME DaliObj
    SOURCES ${SRCS}
    PUBLIC_HEADERS ${PUBLIC_HEADERS}
    DEPS Obsidian::Obsidian)

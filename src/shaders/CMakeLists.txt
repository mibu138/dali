set(SRCS
    comp2a.frag
    comp3a.frag
    comp4a.frag
    comp.frag
    paint.rchit
    paint.rgen
    paint32R.rgen
    paint-image-r8g8b8a8.rgen
    paint.rmiss)

include(author_shaders)
author_shaders(dali_shaders dali Obsidian::glslc
    SOURCES ${SRCS} 
    DEPS 
    fireray.glsl 
    brush.glsl 
    common.glsl 
    raycommon.glsl)

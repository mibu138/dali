set(SRCS
    applyPaint.frag
    comp2a.frag
    comp2.frag
    comp3a.frag
    comp4a.frag
    comp.frag
    layerStack.frag
    paint.rchit
    paint.rgen
    paint.rmiss
    paint-vec2.rchit
    post.frag
    post.vert
    raster.frag
    raster.vert
    select-float.rchit
    select.rchit
    select.rgen
    select.rmiss)

include(author_shaders)
author_shaders(dali_shaders dali
    SOURCES ${SRCS})
